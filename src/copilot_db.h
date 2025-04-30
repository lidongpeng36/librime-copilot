#ifndef RIME_PREDICT_DB_H_
#define RIME_PREDICT_DB_H_

#include <darts.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/string_table.h>
#include <rime/dict/table.h>
#include <rime/resource.h>

namespace rime {

namespace copilot {

struct Metadata {
  static const int kFormatMaxLength = 32;
  char format[kFormatMaxLength];
  uint32_t db_checksum;
  OffsetPtr<char> key_trie;  // DoubleArray (query -> offset of Candidates)
  uint32_t key_trie_size;
  OffsetPtr<char> value_trie;  // StringTable
  uint32_t value_trie_size;
};

using Candidates = ::rime::Array<::rime::table::Entry>;

struct RawEntry {
  string text;
  double weight;
};

using RawData = map<string, vector<RawEntry>>;

}  // namespace copilot

class CopilotDb : public MappedFile {
 public:
  CopilotDb(const path& file_path)
      : MappedFile(file_path), key_trie_(new Darts::DoubleArray), value_trie_(new StringTable) {}

  bool Load();
  bool Save();
  bool Build(const copilot::RawData& data);
  copilot::Candidates* Lookup(const string& query);
  string GetEntryText(const ::rime::table::Entry& entry);

 private:
  int WriteCandidates(const vector<copilot::RawEntry>& candidates, const table::Entry* entry);

  copilot::Metadata* metadata_ = nullptr;
  the<Darts::DoubleArray> key_trie_;
  the<StringTable> value_trie_;
};

}  // namespace rime

#endif  // RIME_PREDICT_DB_H_
