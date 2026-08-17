//
// Drives a real Rime session over the C API so the corpus-eval harness
// measures what the user's plugin actually returns for a given
// (context, keystrokes, expected text) triple -- not a reimplementation of
// it. Reads request JSONL on stdin, writes response JSONL on stdout.
//
// See .superpowers/sdd/2026-08-15-corpus-eval-harness-poc/task-5-brief.md for
// the full contract. librime/tools/rime_api_console.cc is the reference for
// every C-API call sequence used below; the structure here says what to do
// differently, not how each call is spelled.
//
#include <rime_api.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

// This binary links rime_library directly (same as build_copilot /
// dump_copilot), so the C++ Session/Context API is available alongside the C
// API -- used below only to reach Context::commit_history(), which the C API
// has no accessor for at all.
#include <rime/context.h>
#include <rime/service.h>
#include <rime/ticket.h>

#include "copilot_engine.h"  // CopilotEngineComponent, GetCopilotEngineComponentForTools
#include "replay_align.h"
#include "rerank.h"              // TrailingCjkRun
#include "rerank_llm.h"          // llm_rerank::SkipReason, SkipReasonName
#include "rerank_trace.h"        // RerankTraceStore, RerankTrace
#include "scorer.h"              // Scorer
#include "surrounding_source.h"  // GetSurroundingContext, SurroundingSource(Name)

using json = nlohmann::json;

namespace {

// --window's default MUST equal copilot/rerank/window in the replay schema
// (the user's real setting, double_pinyin_flypy.custom.yaml). Re-ranking can
// only promote a candidate from inside its own window, so this is not a
// tuning knob to tidy away -- it is the boundary between "an opportunity
// re-ranking could take" and "out of reach". See task-5-brief.md.
constexpr int kDefaultWindow = 32;
// Bounds how far candidate_list_next is walked per segment, so a
// pathological candidate list cannot blow up a run of thousands of samples.
constexpr int kDefaultMaxScan = 200;
// ImeBridgeState::Config's own default (src/ime_bridge.h); used only as a
// last resort if the schema's own socket_path cannot be read back.
constexpr const char* kFallbackSocketPath = "/tmp/rime_copilot_ime.sock";

struct Options {
  std::string rime_dir;
  int window = kDefaultWindow;
  int max_scan = kDefaultMaxScan;
  bool self_check = false;
  bool verify_speller = false;
  // Measurement-only (task-6-brief.md correction, ruled before dispatch): forces
  // every segment's LLM re-rank scorer to be warmed and confirmed hot BEFORE the
  // keystroke that would consult it is fed, instead of relying on the normal
  // fire-and-forget triggers (Copilot::WarmRerankContext, src/copilot.cc) that
  // assume 1-2 seconds of real typing between compositions. Replay has none of
  // that: every request is its own run of Han text with a brand-new context, so
  // without this flag the warm cache is cold at essentially every scoring
  // opportunity and the LLM path measures as permanently off -- not because
  // re-ranking is broken, but because the harness never waits.
  //
  // NOT a default, and not implied by anything else: forcing the warm removes a
  // real cost (warm-up latency) the live system actually pays on a cold cache,
  // so a run made with this on measures scoring quality with warming forced,
  // not warm-hit rate in live use (see the "Warming" section of
  // docs/superpowers/specs/2026-08-17-llm-rerank-design.md for why the latter
  // can only be measured live). Every response also carries
  // resp["wait_for_warm"] (see ProcessRequest/MakeErrorResponse) so a saved
  // result file can never be mistaken for an unforced, live-shaped measurement.
  bool wait_for_warm = false;
};

bool ParseArgs(int argc, char** argv, Options* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next_value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << arg << " requires a value\n";
        std::exit(64);
      }
      return argv[++i];
    };
    if (arg == "--rime-dir") {
      opts->rime_dir = next_value();
    } else if (arg == "--window") {
      opts->window = std::stoi(next_value());
    } else if (arg == "--max-scan") {
      opts->max_scan = std::stoi(next_value());
    } else if (arg == "--self-check") {
      opts->self_check = true;
    } else if (arg == "--verify-speller") {
      opts->verify_speller = true;
    } else if (arg == "--wait-for-warm") {
      opts->wait_for_warm = true;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if (opts->rime_dir.empty()) {
    std::cerr << "--rime-dir is required\n";
    return false;
  }
  return true;
}

// ============================================================================
// ImeBridge client
//
// Fact 2 (task-5-brief.md): the plugin is linked directly into this binary,
// not into librime.dylib, so there is exactly ONE ImeBridgeServer::Instance()
// in this process -- the one the engine started while building the session's
// Copilot processor chain. Connecting to its socket from here is a real
// client of that real singleton, not a second, disconnected server.
// ============================================================================

int g_bridge_fd = -1;
std::string g_bridge_socket_path;

// Resolve the socket path from the deployed schema rather than hard-coding
// it: the replay Rime directory's double_pinyin_flypy.custom.yaml is EDITED
// (not regenerated) per task-5-brief.md Step 5, precisely so this stays the
// single source of truth instead of drifting from it.
std::string ReadBridgeSocketPath(RimeApi* rime, RimeSessionId session_id) {
  char schema_id[256] = {0};
  if (!rime->get_current_schema(session_id, schema_id, sizeof(schema_id))) {
    return kFallbackSocketPath;
  }
  RimeConfig config = {nullptr};
  if (!rime->schema_open(schema_id, &config)) {
    return kFallbackSocketPath;
  }
  std::string socket_path = kFallbackSocketPath;
  char buf[512] = {0};
  if (rime->config_get_string(&config, "copilot/ime_bridge/socket_path", buf, sizeof(buf))) {
    socket_path = buf;
  }
  rime->config_close(&config);
  return socket_path;
}

// Connects lazily, on first use, and caches the fd for the rest of the
// process. A run that cannot even dial the socket has nothing to self-check
// against, so failure here is just another way PushSurrounding() returns
// false -- SelfCheck() turns that into the same exit 2 as every other cause.
bool EnsureBridgeConnected() {
  if (g_bridge_fd >= 0) {
    return true;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "could not create a socket: " << strerror(errno) << "\n";
    return false;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, g_bridge_socket_path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "could not connect to ime bridge at " << g_bridge_socket_path << ": "
              << strerror(errno) << "\n";
    close(fd);
    return false;
  }
  // Drain the unprompted greeting (ImeBridgeServer::HandleConnection writes
  // it before reading anything, see src/ime_bridge.cc) so it never sits in
  // front of a real reply this process might read later.
  char greeting[512];
  ssize_t n = read(fd, greeting, sizeof(greeting));
  (void)n;  // best-effort; a short read here does not affect correctness below.
  g_bridge_fd = fd;
  return true;
}

bool SendBridgeLine(const json& msg) {
  if (!EnsureBridgeConnected()) {
    return false;
  }
  const std::string line = msg.dump() + "\n";
  size_t sent = 0;
  while (sent < line.size()) {
    ssize_t n = write(g_bridge_fd, line.data() + sent, line.size() - sent);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

// Whether a push landed, and -- since this is also Sentinel 2's report --
// exactly why when it did not. `detail` is empty when `ok`.
struct PushOutcome {
  bool ok = false;
  std::string detail;
};

// The server processes our message on the per-connection thread it spawns for
// this socket (src/ime_bridge.cc's HandleConnection, detached from
// RunServer's accept loop); the write() in SendBridgeLine does not wait for
// that thread to run. Without closing this race, GetSurroundingContext() --
// called immediately after by both SelfCheck() and, every request, right
// before the first RimeProcessKey -- can observe the bridge before that
// thread has processed our line, making context flakily absent or stale.
// That is exactly the nondeterminism Sentinel 4 (Task 7) exists to catch, so
// it must be closed here rather than left to chance. Measured on this build
// tree: without this wait, self-check missed the pushed context on every run.
//
// This is also the ONLY place that ever inspects what GetSurroundingContext()
// returns after a push, so the "which source answered, and was it what we
// pushed" diagnostics live here rather than in a branch after the wait that
// could never be reached (the wait itself already requires an exact match to
// return early).
PushOutcome WaitForPush(const std::string& text, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::optional<rime::SurroundingText> last;
  do {
    last = rime::GetSurroundingContext();
    if (last && last->source == rime::SurroundingSource::kBridge && last->before == text) {
      return {true, ""};
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  } while (std::chrono::steady_clock::now() < deadline);

  if (!last) {
    return {false, "no surrounding context; is ime_bridge enabled in the replay schema?"};
  }
  if (last->source != rime::SurroundingSource::kBridge) {
    return {false, "context came from " + std::string(rime::SurroundingSourceName(last->source)) +
                       ", not the bridge. Disable the tmux source in the replay schema."};
  }
  return {false, "context is '" + last->before + "', expected '" + text +
                     "'. Something else is answering."};
}

// Pushes surrounding text over the bridge exactly as a real client (e.g. the
// Neovim plugin) would: one JSON-Lines "context" message, followed by a wait
// (see WaitForPush) for it to actually land. The push must precede any key
// fed for the same request -- ProcessRequest() and SelfCheck() both rely on
// that ordering.
PushOutcome PushSurrounding(const std::string& text) {
  json msg = {
      {"v", 1},
      {"ns", "rime.ime"},
      {"type", "ascii"},
      {"src", {{"app", "replay_copilot"}, {"instance", "main"}}},
      {"data", {{"action", "context"}, {"before", text}, {"after", ""}}},
  };
  if (!SendBridgeLine(msg)) {
    return {false, "could not push context over the bridge"};
  }
  return WaitForPush(text, std::chrono::milliseconds(200));
}

bool ClearSurrounding() {
  json msg = {
      {"v", 1},
      {"ns", "rime.ime"},
      {"type", "ascii"},
      {"src", {{"app", "replay_copilot"}, {"instance", "main"}}},
      {"data", {{"action", "clear_context"}}},
  };
  return SendBridgeLine(msg);
}

// ============================================================================
// Sentinel 2 -- contamination detection.
//
// GetSurroundingContext() resolves IMK > ImeBridge > tmux > none
// (src/surrounding_source.cc:22). Run inside tmux -- which is where this tool
// will usually be run -- priority 3 scrapes the current pane and hands back
// terminal output as context. The resulting report looks entirely normal and
// would be acted on.
//
// So this exits rather than warning. A red failure is far cheaper than a
// plausible wrong number. The same lesson cost this repo two falsely-green
// runs in clients/neovim/test/.
// ============================================================================
int SelfCheck() {
  const std::string sentinel = "⟦SENTINEL⟧回放哨兵";
  const PushOutcome outcome = PushSurrounding(sentinel);
  if (!outcome.ok) {
    std::cerr << "self-check: " << outcome.detail << "\n";
    return 2;
  }
  return 0;
}

// ============================================================================
// Sentinel 1 -- --verify-speller.
//
// tools/rime_corpus/speller.py derives each syllable's keys by executing the
// schema's speller/algebra rules in Python, and picks sorted()[0] where
// `derive` produced more than one two-key spelling -- an outright assumption
// about which branch this schema actually accepts. This mode is what checks
// it: send Rime the exact keys the table claims for a syllable, then read
// back what Rime thinks was typed via translator/preedit_format, which every
// 双拼 schema carries precisely to render the full pinyin for display (task-6
// brief, "facts already established"). Agreement is the only evidence the
// table can be trusted; a green CI is not, since this needs a real schema and
// dictionary and cannot run there.
//
// Deliberately independent of the IME bridge and SelfCheck(): this mode never
// reads surrounding context, so the tmux-contamination SelfCheck guards
// against cannot affect it, and coupling the two would make a pure keystroke
// check depend on a socket it has no use for.
// ============================================================================
int RunVerifySpeller(RimeApi* rime, RimeSessionId session_id) {
  std::string line;
  int error_count = 0;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      std::cerr << "skipping malformed line (no tab): " << line << "\n";
      ++error_count;
      continue;
    }
    const std::string syllable = line.substr(0, tab);
    const std::string keys = line.substr(tab + 1);

    rime->clear_composition(session_id);
    for (char c : keys) {
      rime->process_key(session_id, static_cast<unsigned char>(c), 0);
    }

    RIME_STRUCT(RimeContext, context);
    std::string preedit;
    if (rime->get_context(session_id, &context)) {
      if (context.composition.preedit) {
        preedit = context.composition.preedit;
      }
      rime->free_context(&context);
    }
    rime->clear_composition(session_id);

    json rec;
    rec["syllable"] = syllable;
    rec["keys"] = keys;
    rec["preedit"] = preedit;
    rec["ok"] = (preedit == syllable);
    std::cout << rec.dump() << "\n";
    std::cout.flush();
  }
  if (error_count > 0) {
    std::cerr << error_count << " malformed line(s) on stdin (see above)\n";
    return 4;  // Does not collide with 2 (Sentinel 2) or 3 (request errors).
  }
  return 0;
}

// ============================================================================
// Replay loop
// ============================================================================

// Up to max_scan candidate texts for the CURRENTLY exposed segment, in menu
// order. Deliberately NOT RimeContext.menu.candidates: the menu holds only
// one page, and this user's default.yaml sets page_size: 5, which would cap
// every `hit` at 4 and misreport nearly the whole corpus as unreachable (see
// task-5-brief.md fact 3).
std::vector<std::string> WalkCandidates(RimeApi* rime, RimeSessionId session_id, int max_scan) {
  std::vector<std::string> out;
  RimeCandidateListIterator iterator = {nullptr};
  if (!rime->candidate_list_begin(session_id, &iterator)) {
    return out;
  }
  while (static_cast<int>(out.size()) < max_scan && rime->candidate_list_next(&iterator)) {
    out.emplace_back(iterator.candidate.text ? iterator.candidate.text : "");
  }
  rime->candidate_list_end(&iterator);
  return out;
}

// Clears librime's own commit history (Context::commit_history()), which
// `RimeClearComposition` does NOT touch -- Context::Clear()
// (src/rime/context.cc) only resets input_/composition_. AutoSpacer's filter
// (plugins/copilot/src/filters.cc) reads commit_history().back() directly to
// decide whether to prepend a space to the next segment's candidates. Left
// uncleared, one request that commits raw Latin text silently changes the
// NEXT request's candidate list -- Sentinel 4 (Task 7) cannot catch this
// because the corruption is order-deterministic, not random. Reached via the
// C++ Session/Context API (rime_library, already linked): the C API has no
// accessor for commit history at all.
void ClearCommitHistory(RimeSessionId session_id) {
  if (auto session = rime::Service::instance().GetSession(session_id)) {
    if (auto* ctx = session->context()) {
      ctx->commit_history().clear();
    }
  }
}

// ============================================================================
// --wait-for-warm (Options::wait_for_warm) -- see its own comment for why
// this exists at all.
// ============================================================================

// What Apply() (rerank_filter.cc) reads to decide whether the LLM path is
// even reachable, gathered once at startup rather than per segment: the
// schema config doesn't change mid-run, and re-opening it on every candidate
// read would be needless overhead on a keystroke-shaped loop.
struct WarmConfig {
  // The SAME Scorer CopilotRerankFilter::Apply() and Copilot::WarmRerankContext
  // consult for this session's schema -- see GetCopilotEngineComponentForTools
  // (src/copilot_engine.h) for why this is the identical instance and not a
  // second, disconnected one. Null when rerank/llm is off or unconfigured, in
  // which case every WarmAndWait() call below is a no-op by construction.
  rime::Scorer* scorer = nullptr;
  // RerankOptions::max_context_chars' own default (rerank_filter.h) -- used
  // whenever the schema does not set copilot/rerank/max_context_chars, same as
  // the filter itself.
  int max_context_chars = 8;
  // The SAME RerankTraceStore CopilotRerankFilter writes each segment's real
  // decision into (rerank_trace.h) -- see ObserveLlm below for why this tool
  // reads that instead of re-deriving the decision itself.
  rime::an<rime::RerankTraceStore> traces;
};

WarmConfig ReadWarmConfig(RimeApi* rime, RimeSessionId session_id) {
  WarmConfig cfg;
  char schema_id[256] = {0};
  const bool have_schema_id = rime->get_current_schema(session_id, schema_id, sizeof(schema_id));
  if (have_schema_id) {
    RimeConfig config = {nullptr};
    if (rime->schema_open(schema_id, &config)) {
      rime->config_get_int(&config, "copilot/rerank/max_context_chars", &cfg.max_context_chars);
      rime->config_close(&config);
    }
  }
  // Reached via the SAME plugin-internal path CopilotComponent/
  // CopilotRerankFilterComponent use to build their own CopilotEngine
  // (GetInstance caches by schema_id, copilot_engine.cc), so this resolves to
  // the identical, already-live instance those processors are using.
  if (auto component = rime::GetCopilotEngineComponentForTools()) {
    if (auto session = rime::Service::instance().GetSession(session_id)) {
      rime::Ticket ticket;
      ticket.schema = session->schema();
      if (auto engine = component->GetInstance(ticket)) {
        cfg.scorer = engine->scorer();
      }
    }
    if (have_schema_id) {
      cfg.traces = component->GetRerankTraces(schema_id);
    }
  }
  return cfg;
}

// Generous on purpose: the first call in the process also pays for loading
// the ~500 MB model (llm_scorer.cc's EnsureLoaded, lazy on the first
// WarmUp()), which is the expensive case --wait-for-warm exists to force
// through rather than race.
constexpr auto kWarmTimeout = std::chrono::seconds(30);

// Blocks until `scorer` reports `context` hot, or kWarmTimeout elapses.
// Null `scorer` and empty `context` are both "nothing to warm" and return
// true trivially, so call sites don't have to special-case either one --
// Apply()'s own fallback chain treats them the same way (rerank_filter.cc).
bool WarmAndWait(rime::Scorer* scorer, const std::string& context) {
  if (!scorer || context.empty()) {
    return true;
  }
  if (scorer->IsWarm(context)) {
    return true;
  }
  scorer->WarmUp(context);
  const auto deadline = std::chrono::steady_clock::now() + kWarmTimeout;
  while (!scorer->IsWarm(context)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// What Apply()'s fallback chain actually decided for one segment, plus the
// real Score() latency when it ran -- read from the filter's own
// RerankTrace (rerank_trace.h), not re-derived.
//
// An earlier version of this tool re-implemented the fallback chain locally
// (llm.enable, on-AC-power, warmed, Loaded()). That was wrong in a way that
// did not show up as a crash: every one of those guards is CONSTANT across a
// single replay run (the schema config and power source do not change
// mid-run), so the only guard that ever actually varied was "is the context
// non-empty" -- the re-implementation collapsed to a restatement of
// TrailingCjkRun's own result, i.e. "is this segment 0 of its request",
// which is not an observation of what the LLM path did at all. Reading the
// real trace instead reports what Apply() actually decided, including guards
// (kBattery, kNoModel, kCold from a timed-out warm) this tool has no
// independent way to know were even hit.
struct ObservedLlm {
  // One of llm_rerank::SkipReasonName's strings (including "none" for
  // "eligible; the scorer was consulted") when a fresh RerankTrace was found;
  // "noctx" when this segment's own context is empty -- a direct, honest
  // TrailingCjkRun result, not a guess, and the one case a trace can never
  // exist for (Apply() early-returns before building a translation at all);
  // "notrace" for the residual case: non-empty context, but neither the LLM
  // nor the db path built a translation (e.g. the db truly has nothing for
  // this exact key either) -- rare, and not otherwise explained.
  std::string label = "noctx";
  int64_t score_us = -1;  // -1 unless `label` is "none" (the scorer actually ran)
};

// Reads whatever RerankTraceStore recorded during the ONE call the caller
// already made between capturing `traces_before` and calling this -- the
// first WalkCandidates() of a request (segment 0) or a select_candidate()
// (segment i+1), the two points that can materialize a NEW segment's
// translation and are the only ones this tool wraps this way. See
// RerankTraceStore::Last()'s own comment for why a size() delta, not exact
// (input, start, end) matching, is what proves the trace is fresh.
ObservedLlm ObserveLlm(const WarmConfig& cfg, size_t traces_before, const std::string& context) {
  if (cfg.traces && cfg.traces->size() > traces_before) {
    if (const auto* trace = cfg.traces->Last()) {
      ObservedLlm out;
      out.label = rime::llm_rerank::SkipReasonName(trace->llm_skip);
      out.score_us = trace->score_us;
      return out;
    }
  }
  ObservedLlm out;
  out.label = context.empty() ? "noctx" : "notrace";
  return out;
}

// The context Apply() will compute for the segment about to be exposed --
// TrailingCjkRun(base, cfg.max_context_chars) -- and, when opts.wait_for_warm,
// whether WarmAndWait() got the scorer to report it hot in time. Shared by the
// pre-keys warm (segment 0 of a request, base == ctx) and the mid-request warm
// (segment i+1, base == the surrounding text currently live over the bridge
// plus everything committed through segment i, see ProcessRequest) so both
// compute the exact same string Apply() will and can never drift apart into
// two different ideas of "the context". `observed` starts as the "nothing
// materialized yet" default and is filled in by ObserveLlm() once the call
// that actually triggers this segment's Apply()/Replenish() has happened
// (see ProcessRequest) -- it is NOT valid to read before that point.
struct PendingSegmentContext {
  std::string context;
  bool warmed = true;  // trivially "warmed": nothing to wait for unless the flag is on
  ObservedLlm observed;
};

PendingSegmentContext BuildAndWarmContext(const Options& opts, const WarmConfig& cfg,
                                          const std::string& base) {
  PendingSegmentContext out;
  out.context = rime::TrailingCjkRun(base, cfg.max_context_chars);
  if (opts.wait_for_warm) {
    out.warmed = WarmAndWait(cfg.scorer, out.context);
  }
  return out;
}

// Shared shape for "this request cannot be trusted, refuse it" -- used by
// both the pre-keys push (ctx alone) and the mid-request re-push (ctx plus
// what this request has committed so far, see ProcessRequest). Matches the
// existing per-request push's contract: LOUD refusal (stderr) rather than a
// silently mis-measured segment, empty `segments` so nothing partial is
// mistaken for real data, and status:"error" so metrics.summarize's
// non-"diverged"-is-ok bucketing does not quietly absorb it (see the
// pre-keys push's own comment for the full rationale).
json MakeErrorResponse(RimeApi* rime, RimeSessionId session_id, const std::string& id,
                       const std::string& detail, bool wait_for_warm) {
  std::cerr << "request " << id << ": " << detail << "\n";
  rime->clear_composition(session_id);
  ClearCommitHistory(session_id);
  ClearSurrounding();
  json resp;
  resp["id"] = id;
  resp["status"] = "error";
  resp["error"] = detail;
  resp["segments"] = json::array();
  // Present on every response, error or not (see Options::wait_for_warm): a
  // saved result file must never be mistaken for a live, unforced measurement.
  resp["wait_for_warm"] = wait_for_warm;
  return resp;
}

// One request's keys, matched against the ground-truth text a candidate at a
// time (see FindLongestPrefixMatch) rather than a fixed one-syllable stride.
//
// On divergence -- no candidate is a prefix of what's left -- one final
// segment is recorded with hit:-1 and want set to the whole unmatched
// remainder, and the request stops there. This overrides the brief's
// "continue recording segments after a divergence": with variable-length
// matching there is no way to know how much input a wrongly-chosen candidate
// would have consumed, so every later span would be fiction (superseding
// ruling, task-5-report.md).
json ProcessRequest(RimeApi* rime, RimeSessionId session_id, const json& req, const Options& opts,
                    const WarmConfig& warm_cfg) {
  const std::string id = req.value("id", "");
  const std::string ctx = req.value("ctx", "");
  const std::string keys = req.value("keys", "");
  const std::string text = req.value("text", "");

  rime->clear_composition(session_id);
  // Precedes any key fed below, per the bridge's contract (Step 2 of the
  // brief): a connection that sends no action registers no client state.
  const PushOutcome push = PushSurrounding(ctx);
  if (!push.ok) {
    // The per-request push is the only context guard during a run --
    // SelfCheck() is startup-only. A stale or absent context here would
    // produce `hit` values indistinguishable from good data, so this request
    // is refused rather than silently measured against the wrong context.
    //
    // That refusal must be LOUD: metrics.summarize (Task 8) buckets every
    // status other than "diverged" as ok, so a silent status:"error" with an
    // empty segments array would inflate `samples`, deflate
    // `divergence_rate`, and vanish from the report -- a bridge stall
    // partway through a run would silently shrink the denominator of the
    // number this whole project exists to produce. stderr here plus a
    // non-zero exit at the end of the run (see main) is what keeps a refusal
    // from reading as a smaller-but-clean sample.
    return MakeErrorResponse(rime, session_id, id, push.detail, opts.wait_for_warm);
  }

  // Segment 0 of this request is retranslated on every one of the keys fed
  // below (Rime rebuilds the active, not-yet-selected segment's menu on each
  // keystroke), always against `ctx` alone -- nothing has been selected yet,
  // so ConfirmedPrefix (rerank.h) contributes nothing for it. Warming (and, if
  // requested, waiting for) that context BEFORE any key is fed is therefore
  // enough to cover every one of those retranslations, including the one live
  // when WalkCandidates() first reads it below.
  const PendingSegmentContext seg0 = BuildAndWarmContext(opts, warm_cfg, ctx);

  int64_t us_keys = 0;
  for (char c : keys) {
    const auto t0 = std::chrono::steady_clock::now();
    rime->process_key(session_id, static_cast<unsigned char>(c), 0);
    const auto t1 = std::chrono::steady_clock::now();
    us_keys += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  }

  json segments = json::array();
  bool diverged = false;
  int key_pos = 0;
  const int total_keys = static_cast<int>(keys.size());
  // Exact under 双拼: every syllable is two keys and one Han character. Also
  // cheaper and safer than measuring `text` itself -- a malformed request
  // degrades to a short `remaining` rather than touching copilot::UTF8 on a
  // string that might be empty.
  const int total_chars = total_keys / 2;
  // What this request has committed so far, across earlier iterations of this
  // loop. By construction every request's ctx ends on a non-Han character (it
  // is the text immediately before a maximal Han run), so TrailingCjkRun
  // (src/rerank.h) sees nothing to key n-gram lookups on for segment 1 -- that
  // is correct, ctx alone really has no trailing Han. But segment 2+'s REAL
  // surrounding text, once segment 1 has committed, is ctx + committed_so_far,
  // which typically does end in Han. Never re-pushing this made re-ranking
  // fire on essentially none of the corpus (measured: 3818/3819 segments saw
  // empty trailing context) -- this loop is what fixes that.
  std::string committed_so_far;
  // What is CURRENTLY live over the bridge -- i.e. the argument of the most
  // recent successful PushSurrounding() call. Segment i+1's Apply() fires
  // synchronously inside select_candidate(i) (measured directly: instrumenting
  // rerank_filter.cc showed segment i+1's `before` is always the push meant
  // for segment i, one iteration stale -- the mid-loop push below lands too
  // late for the very call it was written to feed, and ConfirmedPrefix
  // (rerank.h), not the fresh push, is what actually supplies segment i+1's
  // newest confirmed text). --wait-for-warm must warm the SAME string Apply()
  // will actually see, staleness included, or it warms a context that is
  // never the one asked about and every wait looks successful while every
  // Apply() still finds it cold.
  std::string live_context = ctx;
  // segment0's context/warm result, computed above (before any key was fed);
  // carries into the first loop iteration. Each iteration then computes the
  // NEXT segment's pending context (see below) and hands it forward the same
  // way.
  PendingSegmentContext pending = seg0;
  // Segment 0 is the one case whose materializing call is a WalkCandidates()
  // rather than a select_candidate() -- see BuildAndWarmContext's call above.
  // True only for the loop's first pass; segment i+1 (i>=1)'s observation was
  // already captured at the end of the PREVIOUS iteration (see next_pending
  // below), and re-observing here on a later iteration would read a
  // WalkCandidates() call that is just draining an already-scored cache
  // (measured: 1-287 us, not a Score() call at all -- task-6-report.md) and
  // wrongly overwrite a real decision with "notrace".
  bool first_iteration = true;

  while (key_pos < total_keys) {
    const size_t traces_before = warm_cfg.traces ? warm_cfg.traces->size() : 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> cands = WalkCandidates(rime, session_id, opts.max_scan);
    const auto t1 = std::chrono::steady_clock::now();
    const int64_t us_menu = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if (opts.wait_for_warm && first_iteration) {
      pending.observed = ObserveLlm(warm_cfg, traces_before, pending.context);
    }
    first_iteration = false;

    const int char_pos = key_pos / 2;
    const std::string remaining = rime::replay::SliceChars(text, char_pos, total_chars);
    const rime::replay::PrefixMatch match =
        cands.empty() ? rime::replay::PrefixMatch{}
                      : rime::replay::FindLongestPrefixMatch(cands, remaining);

    json cands_out = json::array();
    for (int i = 0; i < static_cast<int>(cands.size()) && i < opts.window; ++i) {
      cands_out.push_back(cands[i]);
    }

    if (match.hit < 0) {
      // No candidate says what's left -- record the miss against the whole
      // remaining answer and stop; see the function comment for why later
      // segments cannot be recovered from here.
      diverged = true;
      json seg;
      seg["span"] = json::array({key_pos, total_keys});
      seg["want"] = remaining;
      seg["cands"] = std::move(cands_out);
      seg["hit"] = -1;
      seg["us"] = {{"keys", us_keys}, {"menu", us_menu}};
      if (opts.wait_for_warm) {
        seg["llm_skip"] = pending.observed.label;
        if (pending.observed.score_us >= 0) {
          seg["us"]["llm_score"] = pending.observed.score_us;
        }
      }
      segments.push_back(std::move(seg));
      break;
    }

    // The NEXT segment's Apply() (if there is one) fires inside
    // select_candidate() below, synchronously -- see the note on live_context
    // above -- so its context must be warmed and (if requested) confirmed hot
    // BEFORE that call, using exactly what it will see: the surrounding text
    // currently live plus everything confirmed through and including this
    // segment.
    const std::string next_committed = committed_so_far + match.want;
    const bool has_next_segment = (key_pos + 2 * match.chars) < total_keys;
    PendingSegmentContext next_pending;
    if (has_next_segment) {
      next_pending = BuildAndWarmContext(opts, warm_cfg, live_context + next_committed);
    }

    // select_candidate() is where segment i+1's translation is actually
    // materialized (measured directly -- see PendingSegmentContext's comment
    // and task-6-report.md), so this is the ONE call whose trace-store delta
    // can be trusted to belong to next_pending rather than to some earlier,
    // already-observed segment.
    const size_t traces_before_select = warm_cfg.traces ? warm_cfg.traces->size() : 0;
    rime->select_candidate(session_id, static_cast<size_t>(match.hit));
    if (has_next_segment && opts.wait_for_warm) {
      next_pending.observed = ObserveLlm(warm_cfg, traces_before_select, next_pending.context);
    }
    committed_so_far = next_committed;

    json seg;
    seg["span"] = json::array({key_pos, key_pos + 2 * match.chars});
    seg["want"] = match.want;
    seg["cands"] = std::move(cands_out);
    seg["hit"] = match.hit;
    seg["us"] = {{"keys", us_keys}, {"menu", us_menu}};
    if (opts.wait_for_warm) {
      seg["llm_skip"] = pending.observed.label;
      if (pending.observed.score_us >= 0) {
        seg["us"]["llm_score"] = pending.observed.score_us;
      }
    }
    segments.push_back(std::move(seg));

    key_pos += 2 * match.chars;
    pending = next_pending;

    if (key_pos < total_keys) {
      // Land the next segment's real context BEFORE its WalkCandidates() call
      // -- that call is what triggers the filter chain
      // (CopilotRerankFilter::Apply) that reads it, so pushing any later is
      // too late for this iteration and pushing any earlier (e.g. right after
      // select_candidate, always) would do needless work on a request's final
      // segment. Same failure contract as the pre-keys push: a request whose
      // context cannot be trusted partway through must not silently keep
      // measuring it.
      const PushOutcome push = PushSurrounding(ctx + committed_so_far);
      if (!push.ok) {
        return MakeErrorResponse(rime, session_id, id, push.detail, opts.wait_for_warm);
      }
      live_context = ctx + committed_so_far;
    }
  }

  // Nothing may leak into the next request -- Sentinel 4 (Task 7) polices
  // exactly this.
  rime->clear_composition(session_id);
  ClearCommitHistory(session_id);
  ClearSurrounding();

  json resp;
  resp["id"] = id;
  resp["status"] = diverged ? "diverged" : "ok";
  resp["segments"] = std::move(segments);
  // See Options::wait_for_warm: present on every response so a saved result
  // file can never be mistaken for a live, unforced measurement.
  resp["wait_for_warm"] = opts.wait_for_warm;
  return resp;
}

// Runs every request on stdin to completion regardless of individual
// failures -- a single count at the end is more diagnostic than a stack
// trace on request 900 of 2320 -- but the PROCESS still exits non-zero if
// any request errored, so a stalled run is loud rather than merely smaller.
// See ProcessRequest's error path for why silence here is not an option.
int RunReplay(RimeApi* rime, RimeSessionId session_id, const Options& opts,
              const WarmConfig& warm_cfg) {
  std::string line;
  int error_count = 0;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    json req;
    try {
      req = json::parse(line);
    } catch (const json::exception& e) {
      std::cerr << "skipping malformed request line: " << e.what() << "\n";
      continue;
    }
    const json resp = ProcessRequest(rime, session_id, req, opts, warm_cfg);
    if (resp.value("status", "") == "error") {
      ++error_count;
    }
    // ensure_ascii=false: keep Chinese literal rather than escaping it to
    // \uXXXX, matching telemetry_event.h's SerializeJsonl convention.
    std::cout << resp.dump() << "\n";
    std::cout.flush();
  }
  if (error_count > 0) {
    std::cerr << error_count << " request(s) errored (see above); refusing to report success\n";
    return 3;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  Options opts;
  if (!ParseArgs(argc, argv, &opts)) {
    std::cerr << "usage: " << argv[0]
              << " --rime-dir DIR [--window N] [--max-scan N] [--self-check] "
                 "[--verify-speller] [--wait-for-warm]\n";
    return 64;
  }

  RimeApi* rime = rime_get_api();

  RIME_STRUCT(RimeTraits, traits);
  traits.app_name = "rime.replay_copilot";
  traits.user_data_dir = opts.rime_dir.c_str();
  // The replay directory is a full rsync of the user's real ~/Library/Rime
  // (task-5-brief.md Step 5), so it is self-contained and needs no separate
  // shared_data_dir -- pointing both at the same place avoids depending on
  // this process's current working directory for anything.
  traits.shared_data_dir = opts.rime_dir.c_str();
  // Fact 1 (task-5-brief.md): librime.dylib in this build tree contains ZERO
  // copilot code (RIME_EXTRA_MODULES is empty). The plugin exists only in
  // rime-copilot-objs, linked directly into this binary.
  // RIME_REGISTER_MODULE(copilot)'s static initializer registers the module
  // from inside this executable, but kDefaultModules is {"default"} and
  // LoadModules loads only what it is told to -- registration is not
  // loading. Without this array, double_pinyin_flypy's
  // `engine/processors/@0: copilot` finds no such component and replay
  // silently measures a plain pinyin engine with no re-ranking at all.
  const char* modules[] = {"default", "copilot", nullptr};
  traits.modules = modules;  // must outlive RimeInitialize, so it stays on main's stack.

  rime->setup(&traits);
  rime->initialize(&traits);

  const Bool full_check = True;
  if (rime->start_maintenance(full_check)) {
    rime->join_maintenance_thread();
  }

  const RimeSessionId session_id = rime->create_session();
  if (!session_id) {
    std::cerr << "failed to create rime session\n";
    rime->finalize();
    return 1;
  }

  int exit_code = 0;
  if (opts.verify_speller) {
    // Skips ReadBridgeSocketPath()/SelfCheck() entirely -- see
    // RunVerifySpeller's header comment for why this mode has no business
    // with the bridge at all.
    exit_code = RunVerifySpeller(rime, session_id);
  } else {
    // The ImeBridge sub-processor starts ImeBridgeServer::Instance() as part
    // of building this session's Copilot processor chain
    // (src/ime_bridge.cc), so the socket only exists from this point on.
    g_bridge_socket_path = ReadBridgeSocketPath(rime, session_id);

    // Unconditional, before reading any request (or, in --self-check mode,
    // before anything else at all): a run that cannot prove its own context
    // source does not start.
    const int check = SelfCheck();

    if (opts.self_check) {
      exit_code = check;
    } else if (check != 0) {
      exit_code = check;
    } else {
      // Only read when the flag is actually on: a WarmConfig{} default (null
      // scorer, null traces) makes every WarmAndWait() and ObserveLlm() call
      // below a guaranteed no-op, so a plain replay run never even opens the
      // schema config for this.
      const WarmConfig warm_cfg =
          opts.wait_for_warm ? ReadWarmConfig(rime, session_id) : WarmConfig{};
      exit_code = RunReplay(rime, session_id, opts, warm_cfg);
    }
  }

  rime->destroy_session(session_id);
  rime->finalize();
  if (g_bridge_fd >= 0) {
    close(g_bridge_fd);
  }
  return exit_code;
}
