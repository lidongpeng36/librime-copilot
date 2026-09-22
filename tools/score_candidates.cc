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
//                     [--limit N] [--n-ctx N] [--n-gpu-layers N] [--context-chars N]
//                     [--word-end --lexicon <dict.yaml|wordlist> ...]
//
// --word-end additionally scores each candidate as a COMPLETE word: the raw
// score is P(candidate is the next text), which for a single character counts
// every word the character begins -- after 可以, 91% of P(安) is 安装/安排/
// 安全/... -- while the user, who types by word, would have typed those as
// words. So the candidate's last token is decoded too, and
//
//   end_logprob = log(1 - sum P(next token) over tokens whose first
//                          character c makes (candidate + c) a prefix of
//                          some lexicon word)
//
// i.e. the probability that the word stops here. It is written beside the raw
// numbers, never folded into them, so any combination can be recomputed
// offline; the `wordend` ranking below is simply raw + end_logprob.
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
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "scoring_form.h"  // BuildScoringContext, TokenizeScoringForm

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

// Same shape as llama::Backend::Tokenize (src/llm.cc): add_special controls
// whether BOS is inserted, parse_special is always true (no chat template,
// no special-token markup expected in the corpus text anyway). Every call
// site here passes false -- BOS never occurs in the training stream (see
// src/scoring_form.h) -- used to tokenize the pieces TokenizeScoringForm
// splits the context into around each EOS carrier, and to tokenize a
// candidate continuing it.
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
  int context_chars = 64;
  bool word_end = false;
  std::vector<std::string> lexicons;
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
    } else if (arg == "--context-chars") {
      a.context_chars = std::stoi(next());
    } else if (arg == "--word-end") {
      a.word_end = true;
    } else if (arg == "--lexicon") {
      a.lexicons.push_back(next());
    } else {
      throw std::runtime_error("unknown arg: " + arg);
    }
  }
  if (a.model.empty() || a.input.empty() || a.output.empty()) {
    throw std::runtime_error(
        "usage: score_candidates --model M --input I --output O [--limit N] "
        "[--n-ctx N] [--n-gpu-layers N] [--context-chars N] "
        "[--word-end --lexicon L ...]");
  }
  // Without a lexicon every candidate's continuation set is empty and
  // end_logprob is 0 everywhere -- a run that silently measures nothing.
  if (a.word_end && a.lexicons.empty()) {
    throw std::runtime_error("--word-end needs at least one --lexicon");
  }
  return a;
}

struct CandScore {
  std::string text;
  float raw = 0.0f;   // summed log-prob over candidate tokens
  float norm = 0.0f;  // raw / n_tokens
  int n_tokens = 0;
  // --word-end only: log P(the word ends after this candidate), and the
  // probability mass it removed.
  float end_logprob = 0.0f;
  float p_cont = 0.0f;
};

// Length in bytes of the UTF-8 character starting at s[i]; 1 for a stray byte.
size_t Utf8Len(const std::string& s, size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  size_t n = (c & 0x80) == 0      ? 1
             : (c & 0xE0) == 0xC0 ? 2
             : (c & 0xF0) == 0xE0 ? 3
             : (c & 0xF8) == 0xF0 ? 4
                                  : 1;
  return std::min(n, s.size() - i);
}

// Every multi-character word in the given files, sorted and unique. A Rime
// *.dict.yaml is read from after its "..." header line, one word per line as
// the text before the first tab; any other file is a plain word list (first
// whitespace-separated field). Comment lines (#) are skipped either way.
std::vector<std::string> LoadLexicon(const std::vector<std::string>& paths) {
  std::vector<std::string> words;
  for (const auto& path : paths) {
    std::ifstream in(path);
    if (!in) {
      throw std::runtime_error("cannot open lexicon: " + path);
    }
    const bool yaml = path.size() > 5 && path.compare(path.size() - 5, 5, ".yaml") == 0;
    bool in_body = !yaml;
    std::string line;
    size_t before = words.size();
    while (std::getline(in, line)) {
      if (!in_body) {
        in_body = line == "...";
        continue;
      }
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const std::string word = line.substr(0, line.find_first_of(yaml ? "\t" : " \t"));
      if (!word.empty() && Utf8Len(word, 0) < word.size()) {
        words.push_back(word);
      }
    }
    std::cerr << "lexicon " << path << ": " << (words.size() - before) << " words\n";
  }
  std::sort(words.begin(), words.end());
  words.erase(std::unique(words.begin(), words.end()), words.end());
  return words;
}

// The characters that can follow `cand` inside a lexicon word: for 安, the 装
// of 安装, the 排 of 安排, ... `words` is sorted, so every word starting with
// `cand` sits in one contiguous run from its lower bound.
std::vector<std::string> ContinuationChars(const std::vector<std::string>& words,
                                           const std::string& cand) {
  std::vector<std::string> out;
  for (auto it = std::lower_bound(words.begin(), words.end(), cand);
       it != words.end() && it->compare(0, cand.size(), cand) == 0; ++it) {
    if (it->size() > cand.size()) {
      out.push_back(it->substr(cand.size(), Utf8Len(*it, cand.size())));
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

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

  if (!getenv("SCORE_CANDIDATES_VERBOSE"))
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

  // --word-end: the lexicon, and every token indexed by the first character
  // of its text, so "which tokens continue 安" is a lookup per continuation
  // character rather than a scan of the vocabulary per candidate. A token
  // whose text is empty (EOS and the other specials) or starts mid-character
  // (a byte-fallback piece) indexes nowhere, so it always counts as an end.
  std::vector<std::string> lexicon;
  std::unordered_map<std::string, std::vector<llama_token>> tokens_by_first_char;
  if (args.word_end) {
    try {
      lexicon = LoadLexicon(args.lexicons);
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 1;
    }
    char piece[256];
    for (llama_token t = 0; t < n_vocab; ++t) {
      const int n = llama_token_to_piece(vocab, t, piece, sizeof(piece), 0, /*special=*/false);
      if (n <= 0) {
        continue;
      }
      const std::string text(piece, static_cast<size_t>(n));
      const size_t len = Utf8Len(text, 0);
      if ((static_cast<unsigned char>(text[0]) & 0xC0) == 0x80 || len == 0) {
        continue;
      }
      tokens_by_first_char[text.substr(0, len)].push_back(t);
    }
    std::cerr << "word-end: " << lexicon.size() << " words, " << tokens_by_first_char.size()
              << " distinct token first characters\n";
  }

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
  int64_t n_hit_wordend = 0, n_false_promo_wordend = 0;
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
    // The C++ side truncates AND aligns; evalset.py deliberately keeps writing
    // the full raw ctx so the alignment rules have exactly one implementation
    // and there is no Python copy of them to drift from it.
    std::string context =
        rime::BuildScoringContext(j.at("ctx").get<std::string>(), args.context_chars);
    std::vector<std::string> cands = j.at("cands").get<std::vector<std::string>>();
    std::string gold = j.at("gold").get<std::string>();
    int gold_idx = j.at("gold_idx").get<int>();

    // Fresh KV cache per line: contexts are independent, and this keeps
    // position bookkeeping (and n_ctx headroom) simple across 1498 lines.
    llama_memory_seq_rm(mem, kCtxSeq, -1, -1);
    llama_memory_seq_rm(mem, kScratchSeq, -1, -1);

    // add_special = false: BOS never occurs in the training stream (see
    // src/scoring_form.h). The carrier splice is what turns the aligned
    // string's 0x02 bytes into real EOS tokens -- llama_tokenize's
    // parse_special would byte-fall-back on them otherwise.
    std::vector<llama_token> ctx_tokens = rime::TokenizeScoringForm(
        context, llama_vocab_eos(vocab),
        [vocab](const std::string& run) { return Tokenize(vocab, run, /*add_special=*/false); });
    // An empty context (raw ctx was empty, or alignment stripped it down to
    // nothing but whitespace) tokenizes to nothing now that BOS is no longer
    // spliced in, and then the prefill loop below never runs, llama_decode is
    // never called, and llama_get_logits_ith(ctx, -1) returns a null pointer
    // that the very next line reads n_vocab floats from -- a silent SIGSEGV
    // with no output at all, which is how this was found. Every candidate
    // needs SOME distribution to score its first token against; BOS is that
    // distribution -- for this one bail-out only, never as a normal context
    // token.
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
        // With --word-end the LAST token is decoded too: its logits are the
        // distribution over what follows the candidate.
        const size_t n_decode = cand_tokens.size() - (args.word_end ? 0 : 1);
        float end_logprob = 0.0f, p_cont = 0.0f;
        for (size_t ti = 0; ti < n_decode; ++ti) {
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
          ++pos;
          if (ti + 1 < cand_tokens.size()) {
            logprob_sum += LogProbOf(logits, n_vocab, cand_tokens[ti + 1]);
            continue;
          }
          // Past the candidate's end: P(continuing a lexicon word), in the
          // same numerically stable form LogProbOf uses.
          float max_logit = logits[0];
          for (int32_t v = 1; v < n_vocab; ++v) max_logit = std::max(max_logit, logits[v]);
          double sum_all = 0.0;
          for (int32_t v = 0; v < n_vocab; ++v)
            sum_all += std::exp((double)(logits[v] - max_logit));
          double sum_cont = 0.0;
          for (const auto& c : ContinuationChars(lexicon, cand_text)) {
            auto it = tokens_by_first_char.find(c);
            if (it == tokens_by_first_char.end()) continue;
            for (llama_token t : it->second) sum_cont += std::exp((double)(logits[t] - max_logit));
          }
          p_cont = (float)(sum_cont / sum_all);
          // Floored: a candidate the model is certain continues must still
          // compare as very unlikely, not as -inf, so rankings stay total.
          end_logprob = (float)std::log(std::max(1.0 - sum_cont / sum_all, 1e-6));
        }
        int n_tok = (int)cand_tokens.size();
        scores.push_back({cand_text, (float)logprob_sum, (float)(logprob_sum / n_tok), n_tok,
                          end_logprob, p_cont});
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
      std::vector<int> order_wordend = order_raw;
      std::stable_sort(order_wordend.begin(), order_wordend.end(), [&](int a, int b) {
        return scores[a].raw + scores[a].end_logprob > scores[b].raw + scores[b].end_logprob;
      });
      const std::string top1_wordend = scores[order_wordend.front()].text;

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
        json c = {{"text", s.text}, {"logprob", s.raw}, {"n_tokens", s.n_tokens}};
        if (args.word_end) {
          c["end_logprob"] = s.end_logprob;
          c["p_cont"] = s.p_cont;
        }
        cand_dump.push_back(c);
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
      if (args.word_end) {
        rec["llm_top1_wordend"] = top1_wordend;
      }
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
        if (top1_wordend == gold) ++n_hit_wordend;
      } else if (bucket == "A") {
        ++n_bucket_a;
        if (top1_raw != gold) ++n_false_promo_raw;
        if (top1_norm != gold) ++n_false_promo_norm;
        if (top1_wordend != gold) ++n_false_promo_wordend;
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
  if (args.word_end) {
    std::cerr << "hit_rate (wordend)    = "
              << (n_bucket_c ? (double)n_hit_wordend / (double)n_bucket_c : 0.0) << " ("
              << n_hit_wordend << "/" << n_bucket_c << ")\n";
    std::cerr << "false_promo (wordend) = "
              << (n_bucket_a ? (double)n_false_promo_wordend / (double)n_bucket_a : 0.0) << " ("
              << n_false_promo_wordend << "/" << n_bucket_a << ")\n";
  }
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
