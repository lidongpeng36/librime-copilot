#include "select_character.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>

#include "history.h"

namespace rime {

enum struct SelectCharacter::Selection {
  kNone = 0,
  kFirst = 1,
  kLast = 2,
  kLeft = 3,
  kRight = 4,
};

void SelectCharacter::Init(const Ticket& ticket) {
  auto* schema = ticket.schema;
  if (!schema) {
    return;
  }
  std::string key_repr;
  auto* config = schema->config();
  if (config->GetString("key_binder/select_first_character", &key_repr)) {
    LOG(INFO) << "key_binder/select_first_character: " << key_repr;
    selection_map_.emplace(key_repr, Selection::kFirst);
  }
  if (config->GetString("key_binder/select_last_character", &key_repr)) {
    LOG(INFO) << "key_binder/select_last_character: " << key_repr;
    selection_map_.emplace(key_repr, Selection::kLast);
  }
  if (config->GetString("key_binder/select_left_characters", &key_repr)) {
    LOG(INFO) << "key_binder/select_left_characters: " << key_repr;
    selection_map_.emplace(key_repr, Selection::kLeft);
  }
  if (config->GetString("key_binder/select_right_characters", &key_repr)) {
    LOG(INFO) << "key_binder/select_right_characters: " << key_repr;
    selection_map_.emplace(key_repr, Selection::kRight);
  }
}

ProcessResult SelectCharacter::Process(const KeyEvent& key_event, std::string* output) {
  if (!engine_ || key_event.release()) {
    return kNoop;
  }
  auto* ctx = engine_->context();
  if (!ctx) {
    return kNoop;
  }

  if (ctx->IsComposing() || ctx->HasMenu()) {
    auto repr = key_event.repr();
    auto it = selection_map_.find(repr);
    if (it == selection_map_.end()) {
      return kNoop;
    }
    auto selection = it->second;
    auto c = ctx->GetSelectedCandidate();
    if (c) {
      const auto text = c->text();
      ::copilot::UTF8 utf8(text);
      // Keep the slice in its own string: utf8 is a view over `text`, so
      // assigning back into `text` would rewrite the buffer being viewed.
      std::string picked;
      switch (selection) {
        case Selection::kFirst:
          picked = std::string(utf8[0]);
          break;
        case Selection::kLast:
          picked = std::string(utf8[-1]);
          break;
        case Selection::kLeft:
          picked = std::string(utf8.left());
          break;
        case Selection::kRight:
          picked = std::string(utf8.right());
          break;
        default:
          return kNoop;
      }
      // Nothing on that side of the candidate (e.g. leading punctuation):
      // leave the key to the rest of the chain instead of committing "".
      if (picked.empty()) {
        return kNoop;
      }
      *output = picked;
      DLOG(INFO) << "Select Text: " << picked;
      engine_->CommitText(picked);
      ctx->Clear();
      return kAccepted;
    }
  }

  return kNoop;
}

}  // namespace rime
