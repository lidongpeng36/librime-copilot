//
// Offline experiment: does an LLM's P(candidate | context) re-rank Chinese IME
// candidates better than the n-gram re-ranker does?
//
// Reads the corpus eval JSONL (id, bucket, ctx, cands[], gold, gold_idx),
// scores every candidate under the given base model by summed log-probability
// of its own tokens continuing the (shared, prefilled-once) context, and
// writes one result line per input line plus a summary to stderr.
//
// Score by likelihood, not generation: no chat template, no sampling, greedy
// or otherwise -- this is teacher-forced scoring of candidate token IDs
// against the model's own logits.
//
//   score_candidates --model <gguf> --input <jsonl> --output <jsonl> \
//                     [--limit N] [--n-ctx N] [--n-gpu-layers N]
//
#include <llama.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

// Same shape as llama::Backend::Tokenize (src/llm.cc): add_special controls
// whether BOS is inserted, parse_special is always true (no chat template,
// no special-token markup expected in the corpus text anyway).
std::vector<llama_token> Tokenize(const llama_vocab* vocab, const std::string& text,
                                  bool add_special) {
  int n = -llama_tokenize(vocab, text.data(), (int)text.size(), nullptr, 0, add_special, true);
  std::vector<llama_token> toks(n > 0 ? n : 0);
  if (n > 0) {
    llama_tokenize(vocab, text.data(), (int)text.size(), toks.data(), n, add_special, true);
  }
  return toks;
}

// log P(target) under the categorical distribution defined by `logits`
// (length n_vocab), computed via a numerically stable log-softmax.
float LogProbOf(const float* logits, int32_t n_vocab, llama_token target) {
  float max_logit = logits[0];
  for (int32_t i = 1; i < n_vocab; ++i) {
    max_logit = std::max(max_logit, logits[i]);
  }
  double sum_exp = 0.0;
  for (int32_t i = 0; i < n_vocab; ++i) {
    sum_exp += std::exp((double)(logits[i] - max_logit));
  }
  double log_sum_exp = (double)max_logit + std::log(sum_exp);
  return (float)((double)logits[target] - log_sum_exp);
}

struct Args {
  std::string model;
  std::string input;
  std::string output;
  int limit = -1;
  int n_ctx = 4096;
  int n_gpu_layers = 99;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + arg);
      }
      return argv[++i];
    };
    if (arg == "--model") {
      a.model = next();
    } else if (arg == "--input") {
      a.input = next();
    } else if (arg == "--output") {
      a.output = next();
    } else if (arg == "--limit") {
      a.limit = std::stoi(next());
    } else if (arg == "--n-ctx") {
      a.n_ctx = std::stoi(next());
    } else if (arg == "--n-gpu-layers") {
      a.n_gpu_layers = std::stoi(next());
    } else {
      throw std::runtime_error("unknown arg: " + arg);
    }
  }
  if (a.model.empty() || a.input.empty() || a.output.empty()) {
    throw std::runtime_error("usage: score_candidates --model M --input I --output O [--limit N]");
  }
  return a;
}

struct CandScore {
  std::string text;
  float raw = 0.0f;   // summed log-prob over candidate tokens
  float norm = 0.0f;  // raw / n_tokens
  int n_tokens = 0;
};

constexpr llama_seq_id kCtxSeq = 0;
constexpr llama_seq_id kScratchSeq = 1;

}  // namespace

int main(int argc, char** argv) {
  Args args;
  try {
    args = ParseArgs(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
  llama_backend_init();

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = args.n_gpu_layers;
  llama_model* model = llama_model_load_from_file(args.model.c_str(), model_params);
  if (!model) {
    std::cerr << "failed to load model: " << args.model << "\n";
    return 1;
  }
  const llama_vocab* vocab = llama_model_get_vocab(model);
  const int32_t n_vocab = llama_vocab_n_tokens(vocab);

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = args.n_ctx;
  ctx_params.n_batch = 512;
  // Two live sequences: kCtxSeq (the prefilled context) and kScratchSeq (the
  // per-candidate branch copied from it). Left at the default of 1, every
  // llama_decode() against kScratchSeq fails to find a KV slot.
  ctx_params.n_seq_max = 2;
  ctx_params.no_perf = true;
  ctx_params.n_threads = (int32_t)std::thread::hardware_concurrency();
  ctx_params.n_threads_batch = ctx_params.n_threads;

  llama_context* ctx = llama_init_from_model(model, ctx_params);
  if (!ctx) {
    std::cerr << "failed to create context\n";
    return 1;
  }
  llama_memory_t mem = llama_get_memory(ctx);
  const uint32_t n_batch = ctx_params.n_batch;

  llama_batch batch = llama_batch_init((int32_t)n_batch, 0, 1);

  std::ifstream in(args.input);
  if (!in) {
    std::cerr << "cannot open input: " << args.input << "\n";
    return 1;
  }
  std::ofstream out(args.output);
  if (!out) {
    std::cerr << "cannot open output: " << args.output << "\n";
    return 1;
  }

  int64_t n_lines = 0;
  int64_t n_bucket_c = 0, n_bucket_a = 0;
  int64_t n_hit_raw = 0, n_hit_norm = 0;
  int64_t n_false_promo_raw = 0, n_false_promo_norm = 0;
  std::vector<double> prefill_us_all;
  std::vector<double> score_all_us_all;

  int64_t t_start = NowUs();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (args.limit >= 0 && n_lines >= args.limit) {
      break;
    }
    json j;
    try {
      j = json::parse(line);
    } catch (const std::exception& e) {
      std::cerr << "skip malformed line " << (n_lines + 1) << ": " << e.what() << "\n";
      continue;
    }
    std::string id = j.at("id").get<std::string>();
    std::string bucket = j.at("bucket").get<std::string>();
    std::string context = j.at("ctx").get<std::string>();
    std::vector<std::string> cands = j.at("cands").get<std::vector<std::string>>();
    std::string gold = j.at("gold").get<std::string>();
    int gold_idx = j.at("gold_idx").get<int>();

    // Fresh KV cache per line: contexts are independent, and this keeps
    // position bookkeeping (and n_ctx headroom) simple across 1498 lines.
    llama_memory_seq_rm(mem, kCtxSeq, -1, -1);
    llama_memory_seq_rm(mem, kScratchSeq, -1, -1);

    std::vector<llama_token> ctx_tokens = Tokenize(vocab, context, /*add_special=*/true);
    // An empty context tokenizes to nothing on a vocab with no BOS (Qwen3 is
    // one), and then the prefill loop below never runs, llama_decode is never
    // called, and llama_get_logits_ith(ctx, -1) returns a null pointer that
    // the very next line reads n_vocab floats from -- a silent SIGSEGV with no
    // output at all, which is how this was found. Every candidate needs SOME
    // distribution to score its first token against; BOS is that distribution.
    //
    // Not a nicety: whole-sentence scoring sets have empty contexts by
    // construction wherever the text starts a message (measured: 619 of 3287
    // runs), so refusing them would drop a fifth of the population rather than
    // a stray record.
    if (ctx_tokens.empty()) {
      const llama_token bos = llama_vocab_bos(vocab);
      if (bos == LLAMA_TOKEN_NULL) {
        std::cerr << "line " << (n_lines + 1) << " (" << id
                  << "): empty context and no BOS token to stand in for it, skipping\n";
        ++n_lines;
        continue;
      }
      ctx_tokens.push_back(bos);
    }
    if ((int)ctx_tokens.size() >= args.n_ctx - 32) {
      std::cerr << "line " << (n_lines + 1) << " (" << id << "): context too long ("
                << ctx_tokens.size() << " tokens), skipping\n";
      ++n_lines;
      continue;
    }

    // --- Shared prefill: decode the context once, request logits only for
    // the last token (that's the distribution every candidate's first token
    // is scored against). ---
    int64_t t_prefill0 = NowUs();
    int n_ctx_tok = (int)ctx_tokens.size();
    for (int i = 0; i < n_ctx_tok; i += (int)n_batch) {
      int chunk = std::min((int)n_batch, n_ctx_tok - i);
      batch.n_tokens = chunk;
      for (int k = 0; k < chunk; ++k) {
        batch.token[k] = ctx_tokens[i + k];
        batch.pos[k] = i + k;
        batch.n_seq_id[k] = 1;
        batch.seq_id[k][0] = kCtxSeq;
        bool is_last_overall = (i + k == n_ctx_tok - 1);
        batch.logits[k] = is_last_overall ? 1 : 0;
      }
      if (llama_decode(ctx, batch) != 0) {
        std::cerr << "line " << (n_lines + 1) << " (" << id << "): context decode failed\n";
        goto next_line;
      }
    }
    {
      float* ctx_last_logits_ptr = llama_get_logits_ith(ctx, -1);
      std::vector<float> ctx_last_logits(ctx_last_logits_ptr, ctx_last_logits_ptr + n_vocab);
      int64_t t_prefill1 = NowUs();
      double prefill_us = (double)(t_prefill1 - t_prefill0);

      // --- Per-candidate scoring, branching off the shared context state. ---
      int64_t t_score0 = NowUs();
      std::vector<CandScore> scores;
      scores.reserve(cands.size());
      for (const auto& cand_text : cands) {
        std::vector<llama_token> cand_tokens = Tokenize(vocab, cand_text, /*add_special=*/false);
        if (cand_tokens.empty()) {
          scores.push_back({cand_text, -1e30f, -1e30f, 0});
          continue;
        }
        // Branch the KV cache: copy the (already-prefilled) context into the
        // scratch sequence fresh for every candidate.
        llama_memory_seq_rm(mem, kScratchSeq, -1, -1);
        llama_memory_seq_cp(mem, kCtxSeq, kScratchSeq, -1, -1);

        double logprob_sum = LogProbOf(ctx_last_logits.data(), n_vocab, cand_tokens[0]);
        int pos = n_ctx_tok;
        for (size_t ti = 0; ti + 1 < cand_tokens.size(); ++ti) {
          batch.n_tokens = 1;
          batch.token[0] = cand_tokens[ti];
          batch.pos[0] = pos;
          batch.n_seq_id[0] = 1;
          batch.seq_id[0][0] = kScratchSeq;
          batch.logits[0] = 1;
          if (llama_decode(ctx, batch) != 0) {
            std::cerr << "line " << (n_lines + 1) << " (" << id
                      << "): candidate decode failed for '" << cand_text << "'\n";
            break;
          }
          float* logits = llama_get_logits_ith(ctx, -1);
          logprob_sum += LogProbOf(logits, n_vocab, cand_tokens[ti + 1]);
          ++pos;
        }
        int n_tok = (int)cand_tokens.size();
        scores.push_back({cand_text, (float)logprob_sum, (float)(logprob_sum / n_tok), n_tok});
      }
      int64_t t_score1 = NowUs();
      double score_all_us = (double)(t_score1 - t_score0);

      // Rank by raw sum and by length-normalised mean, independently.
      std::vector<int> order_raw(scores.size()), order_norm(scores.size());
      for (size_t i = 0; i < scores.size(); ++i) {
        order_raw[i] = (int)i;
        order_norm[i] = (int)i;
      }
      std::stable_sort(order_raw.begin(), order_raw.end(),
                       [&](int a, int b) { return scores[a].raw > scores[b].raw; });
      std::stable_sort(order_norm.begin(), order_norm.end(),
                       [&](int a, int b) { return scores[a].norm > scores[b].norm; });

      std::string top1_raw = scores[order_raw.front()].text;
      std::string top1_norm = scores[order_norm.front()].text;
      int rank_of_gold_raw = -1, rank_of_gold_norm = -1;
      for (size_t r = 0; r < order_raw.size(); ++r) {
        if (scores[order_raw[r]].text == gold && rank_of_gold_raw < 0) {
          rank_of_gold_raw = (int)r;
        }
      }
      for (size_t r = 0; r < order_norm.size(); ++r) {
        if (scores[order_norm[r]].text == gold && rank_of_gold_norm < 0) {
          rank_of_gold_norm = (int)r;
        }
      }

      // Raw per-candidate numbers, same order as the input `cands` array, so
      // downstream analysis (formula experiments) can recompute rankings
      // without paying for another model pass. `logprob` is CandScore::raw
      // (summed log-prob, unchanged); `n_tokens` is the divisor already used
      // for CandScore::norm above -- dumping it under its real name rather
      // than implying it's always a plain token count.
      json cand_dump = json::array();
      for (const auto& s : scores) {
        cand_dump.push_back({{"text", s.text}, {"logprob", s.raw}, {"n_tokens", s.n_tokens}});
      }

      json rec;
      rec["id"] = id;
      rec["bucket"] = bucket;
      rec["gold"] = gold;
      rec["gold_idx"] = gold_idx;
      rec["llm_top1_raw"] = top1_raw;
      rec["llm_top1_norm"] = top1_norm;
      rec["llm_rank_of_gold_raw"] = rank_of_gold_raw;
      rec["llm_rank_of_gold_norm"] = rank_of_gold_norm;
      rec["n_candidates"] = (int)cands.size();
      rec["candidates"] = cand_dump;
      rec["times"] = {{"prefill_us", prefill_us}, {"score_all_us", score_all_us}};
      out << rec.dump() << "\n";

      prefill_us_all.push_back(prefill_us);
      score_all_us_all.push_back(score_all_us);

      if (bucket == "C") {
        ++n_bucket_c;
        if (top1_raw == gold) ++n_hit_raw;
        if (top1_norm == gold) ++n_hit_norm;
      } else if (bucket == "A") {
        ++n_bucket_a;
        if (top1_raw != gold) ++n_false_promo_raw;
        if (top1_norm != gold) ++n_false_promo_norm;
      }
    }

  next_line:
    ++n_lines;
    if (n_lines % 25 == 0) {
      int64_t elapsed_us = NowUs() - t_start;
      double rate = (double)n_lines / ((double)elapsed_us / 1e6);
      std::cerr << "... " << n_lines << " lines, " << rate << " lines/s\n";
    }
  }

  int64_t t_end = NowUs();
  double total_s = (double)(t_end - t_start) / 1e6;

  auto median = [](std::vector<double> v) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  auto p95 = [](std::vector<double> v) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(0.95 * (double)(v.size() - 1));
    return v[idx];
  };

  double hit_rate_raw = n_bucket_c ? (double)n_hit_raw / (double)n_bucket_c : 0.0;
  double hit_rate_norm = n_bucket_c ? (double)n_hit_norm / (double)n_bucket_c : 0.0;
  double fp_rate_raw = n_bucket_a ? (double)n_false_promo_raw / (double)n_bucket_a : 0.0;
  double fp_rate_norm = n_bucket_a ? (double)n_false_promo_norm / (double)n_bucket_a : 0.0;

  std::cerr << "\n=== summary (n_lines=" << n_lines << ", bucket C=" << n_bucket_c
            << ", bucket A=" << n_bucket_a << ") ===\n";
  std::cerr << "hit_rate (raw)        = " << hit_rate_raw << " (" << n_hit_raw << "/" << n_bucket_c
            << ")\n";
  std::cerr << "hit_rate (norm)       = " << hit_rate_norm << " (" << n_hit_norm << "/"
            << n_bucket_c << ")\n";
  std::cerr << "false_promo (raw)     = " << fp_rate_raw << " (" << n_false_promo_raw << "/"
            << n_bucket_a << ")\n";
  std::cerr << "false_promo (norm)    = " << fp_rate_norm << " (" << n_false_promo_norm << "/"
            << n_bucket_a << ")\n";
  std::cerr << "prefill_us median/p95 = " << median(prefill_us_all) << " / " << p95(prefill_us_all)
            << "\n";
  std::cerr << "score_all_us median/p95 = " << median(score_all_us_all) << " / "
            << p95(score_all_us_all) << "\n";
  std::cerr << "total wall time = " << total_s << "s for " << n_lines << " lines ("
            << (n_lines / std::max(total_s, 1e-9)) << " lines/s)\n";

  llama_batch_free(batch);
  llama_free(ctx);
  llama_model_free(model);
  llama_backend_free();
  return 0;
}
