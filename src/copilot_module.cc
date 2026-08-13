#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include <cstdlib>
#include <filesystem>

#include <glog/logging.h>

#include "copilot.h"
#include "copilot_engine.h"
#include "copilot_translator.h"

#include "auto_spacer.h"
#include "filters.h"
#include "rerank_filter.h"
#include "select_character.h"

using namespace rime;

namespace {

// librime statically links glog and does not export its symbols (`nm -gU
// librime.1.dylib | grep google::LogMessage` finds none of its 2967 exported
// symbols). This plugin also statically links glog -- it has to, since it
// cannot bind to a copy librime never exposes -- which means the plugin owns
// a second, completely separate glog instance
// (`nm -U librime-copilot.dylib | grep -c google.*LogMessage` -> 72, none of
// them imported). librime initializes *its* copy in
// rime::SetupLogging()/InitGoogleLogging(); that call never touches this
// plugin's copy, whose FLAGS_log_dir stays empty and which is never
// initialized. Every LOG(...) call in this plugin -- including the
// unconditional ones -- has therefore always been silently discarded, with
// no error and no output anywhere (not stderr, not the macOS unified log,
// not any file), regardless of the `copilot/surrounding_debug` or
// `copilot/ime_bridge/debug` flags. This is the plugin's own
// InitGoogleLogging call, mirroring librime's src/rime/setup.cc SetupLogging,
// so this copy actually writes somewhere.
//
// Do not delete this as "redundant with librime's logging setup": librime
// initializing its glog instance does nothing for this plugin's glog
// instance. They are two different static-linked copies in two different
// binaries.
void InitCopilotLogging() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;

  if (google::IsGoogleLoggingInitialized()) {
    // Some other copy in this same process already claimed the global glog
    // state InitGoogleLogging touches (argv0, etc). Do not call it twice --
    // glog aborts on a second InitGoogleLogging in the same instance -- and
    // there is nothing else to do: this plugin's own static glog copy is a
    // distinct translation unit's globals, so if this ever fires it means
    // something upstream changed, not that our job is already done.
    return;
  }

  // Squirrel's process cwd is inside the read-only, root-owned app bundle,
  // so glog cannot fall back to writing there. Don't hardcode Squirrel's
  // `rime.squirrel` subdirectory either -- this plugin also runs under
  // fcitx/ibus on Linux, where that directory has no reason to exist, and
  // glog fails outright if FLAGS_log_dir doesn't exist.
  const char* tmpdir = std::getenv("TMPDIR");
  std::filesystem::path log_dir = (tmpdir && *tmpdir) ? tmpdir : "/tmp";

  std::error_code ec;
  std::filesystem::create_directories(log_dir, ec);
  // If create_directories failed (e.g. unwritable parent), fall through and
  // let glog try anyway; glog degrades to logging nothing rather than
  // crashing when it can't open a log file, so this is safe either way.

  FLAGS_log_dir = log_dir.string();
  // Matches rime::SetupLogging's call for librime's own copy -- without it,
  // glog's default filename has no trailing extension, and the two log
  // families' names would only *look* parallel while actually differing.
  google::SetLogFilenameExtension(".log");
  // Distinct program name from Squirrel's own `rime.squirrel.*` log files,
  // so a user tailing logs can tell the two apart:
  // rime_copilot.<host>.<user>.log.INFO.<date>.<pid>.log
  google::InitGoogleLogging("rime_copilot");

  // glog creates the log file lazily, on the first LOG() call -- not here in
  // InitGoogleLogging -- so without this banner, a process that never
  // happens to hit another LOG() call (e.g. no schema loaded, or every
  // debug flag off) leaves no file at all, and "the fix ran" is
  // indistinguishable from "the fix is broken". This unconditional line is
  // what forces the file into existence and makes the fix verifiable.
  // Deliberate, not noise: do not delete it as redundant with the LOG(...)
  // sites it exists to make visible in the first place.
  LOG(INFO) << "[copilot] logging initialized; plugin diagnostics go to " << FLAGS_log_dir
            << "/rime_copilot.*.log.INFO.*.log  (NOT the "
            << "rime.squirrel.* log -- see src/copilot_module.cc)";
}

}  // namespace

static void rime_copilot_initialize() {
  InitCopilotLogging();

  Registry& r = Registry::instance();
  an<CopilotEngineComponent> engine_factory = New<CopilotEngineComponent>();
  r.Register("copilot", new CopilotComponent(engine_factory));
  r.Register("copilot_translator", new CopilotTranslatorComponent(engine_factory));

  r.Register("auto_spacer", new CopilotPluginComponent<AutoSpacer>());
  r.Register("select_character", new CopilotPluginComponent<SelectCharacter>());

  r.Register("auto_spacer_filter", new Component<AutoSpacerFilter>);
  r.Register("raw_input_filter", new Component<RawInputFilter>);
  r.Register("copilot_filter", new Component<CopilotFilter>);

  // Contextual candidate re-ranking. Opt in by adding `copilot_rerank_filter`
  // to engine/filters, ahead of any pinning filter so pinned candidates keep
  // winning.
  r.Register("copilot_rerank_filter", new CopilotRerankFilterComponent(engine_factory));
}

static void rime_copilot_finalize() {}

RIME_REGISTER_MODULE(copilot)
