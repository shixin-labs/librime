//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-02 GONG Chen <chen.sst@gmail.com>
//
#include <cfloat>
#include <cstring>
#include <algorithm>
#include <queue>
#include <utility>
#include <rime/common.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/table.h>
#include "subtree_bounds.h"
#include "subtree_bounds_io.h"
#include "initial_cache.h"
#include "mirror_cache.h"

namespace rime {

const char kTableFormatLatest[] = "Rime::Table/4.0";
const int kTableFormatLowestCompatible = 4.0;

const char kTableFormatPrefix[] = "Rime::Table/";
const size_t kTableFormatPrefixLen = sizeof(kTableFormatPrefix) - 1;

TableAccessor::TableAccessor(const Code& index_code,
                             const List<table::Entry>* list,
                             double credibility,
                             double quality_len)
    : index_code_(index_code),
      entries_(list->at.get()),
      size_(list->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

TableAccessor::TableAccessor(const Code& index_code,
                             const Array<table::Entry>* array,
                             double credibility,
                             double quality_len)
    : index_code_(index_code),
      entries_(array->at),
      size_(array->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

TableAccessor::TableAccessor(const Code& index_code,
                             const table::TailIndex* code_map,
                             double credibility,
                             double quality_len)
    : index_code_(index_code),
      long_entries_(code_map->at),
      size_(code_map->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

bool TableAccessor::exhausted() const {
  if (entries_ || long_entries_) {
    return !(size_ - cursor_);
  }
  return true;
}

size_t TableAccessor::remaining() const {
  if (entries_ || long_entries_) {
    return size_ - cursor_;
  }
  return 0;
}

const table::Entry* TableAccessor::entry() const {
  if (exhausted())
    return NULL;
  if (entries_)
    return &entries_[cursor_];
  else
    return &long_entries_[cursor_].entry;
}

const table::Code* TableAccessor::extra_code() const {
  if (!long_entries_ || cursor_ >= size_)
    return NULL;
  return &long_entries_[cursor_].extra_code;
}

Code TableAccessor::code() const {
  auto extra = extra_code();
  if (!extra) {
    return index_code();
  }
  Code code(index_code());
  for (auto p = extra->begin(); p != extra->end(); ++p) {
    code.push_back(*p);
  }
  return code;
}

bool TableAccessor::Next() {
  if (exhausted())
    return false;
  ++cursor_;
  return !exhausted();
}

bool TableQuery::Advance(SyllableId syllable_id,
                         double credibility,
                         double quality_len,
                         size_t last_pos) {
  if (!Walk(syllable_id)) {
    return false;
  }
  credibility_[level_] = credibility_sum() + credibility;
  quality_len_[level_] = quality_len_sum() + quality_len;
  last_pos_[level_] = last_pos;
  ++level_;
  index_code_.push_back(syllable_id);
  return true;
}

bool TableQuery::Backdate() {
  if (level_ == 0)
    return false;
  --level_;
  if (index_code_.size() > level_) {
    index_code_.pop_back();
  }
  return true;
}

void TableQuery::Reset() {
  level_ = 0;
  index_code_.clear();
}

inline static bool node_less(const table::TrunkIndexNode& a,
                             const table::TrunkIndexNode& b) {
  return a.key < b.key;
}

static table::TrunkIndexNode* find_node(table::TrunkIndexNode* first,
                                        table::TrunkIndexNode* last,
                                        const SyllableId& key) {
  table::TrunkIndexNode target;
  target.key = key;
  auto it = std::lower_bound(first, last, target, node_less);
  return it == last || key < it->key ? last : it;
}

bool TableQuery::Walk(SyllableId syllable_id) {
  if (level_ == 0) {
    if (!lv1_index_ || syllable_id < 0 ||
        syllable_id >= static_cast<SyllableId>(lv1_index_->size))
      return false;
    auto node = &lv1_index_->at[syllable_id];
    if (!node->next_level)
      return false;
    lv2_index_ = &node->next_level->trunk();
  } else if (level_ == 1) {
    if (!lv2_index_)
      return false;
    auto node = find_node(lv2_index_->begin(), lv2_index_->end(), syllable_id);
    if (node == lv2_index_->end())
      return false;
    if (!node->next_level)
      return false;
    lv3_index_ = &node->next_level->trunk();
  } else if (level_ == 2) {
    if (!lv3_index_)
      return false;
    auto node = find_node(lv3_index_->begin(), lv3_index_->end(), syllable_id);
    if (node == lv3_index_->end())
      return false;
    if (!node->next_level)
      return false;
    lv4_index_ = &node->next_level->tail();
  } else {
    return false;
  }
  return true;
}

inline static Code add_syllable(Code code, SyllableId syllable_id) {
  code.push_back(syllable_id);
  return code;
}

TableAccessor TableQuery::Access(SyllableId syllable_id,
                                 double credibility,
                                 double quality_len) const {
  credibility += credibility_sum();
  quality_len += quality_len_sum();
  if (level_ == 0) {
    if (!lv1_index_ || syllable_id < 0 ||
        syllable_id >= static_cast<SyllableId>(lv1_index_->size))
      return TableAccessor();
    auto node = &lv1_index_->at[syllable_id];
    return TableAccessor(add_syllable(index_code_, syllable_id), &node->entries,
                         credibility, quality_len);
  } else if (level_ == 1 || level_ == 2) {
    auto index = (level_ == 1) ? lv2_index_ : lv3_index_;
    if (!index)
      return TableAccessor();
    auto node = find_node(index->begin(), index->end(), syllable_id);
    if (node == index->end())
      return TableAccessor();
    return TableAccessor(add_syllable(index_code_, syllable_id), &node->entries,
                         credibility, quality_len);
  } else if (level_ == 3) {
    if (!lv4_index_)
      return TableAccessor();
    return TableAccessor(index_code_, lv4_index_, credibility, quality_len);
  }
  return TableAccessor();
}

// string Table::GetString_v1(const table::StringType& x) {
//  return x.str().c_str();
// }

// bool Table::AddString_v1(const string& src, table::StringType* dest,
//                          double /*weight*/) {
//   return CopyString(src, &dest->str());
// }

string Table::GetString(const table::StringType& x) {
  return string_table_->GetString(x.str_id());
}

bool Table::AddString(const string& src,
                      table::StringType* dest,
                      double weight) {
  string_table_builder_->Add(src, weight, &dest->str_id());
  return true;
}

bool Table::OnBuildStart() {
  string_table_builder_.reset(new StringTableBuilder);
  return true;
}

bool Table::OnBuildFinish() {
  string_table_builder_->Build();
  // saving string table image
  size_t image_size = string_table_builder_->BinarySize();
  char* image = Allocate<char>(image_size);
  if (!image) {
    LOG(ERROR) << "Error creating string table image.";
    return false;
  }
  string_table_builder_->Dump(image, image_size);
  metadata_->string_table = image;
  metadata_->string_table_size = image_size;
  return true;
}

bool Table::OnLoad() {
  string_table_.reset(new StringTable(metadata_->string_table.get(),
                                      metadata_->string_table_size));
  return true;
}

struct ShixinTypoInitialCache {
  shixin_typo::InitialCache cache;
  std::vector<char> initials;
  shixin_typo::InitialCacheSearch search;
  shixin_typo::MirrorCache mirror;
  bool mirror_required = false;
  bool mirror_initialized = false;
  string mirror_path;
};
bool Table::PrepareShixinTypoInitialCache(const string& sidecar_path) {
  if(shixin_typo_initial_cache_) return true;
  if(!IsOpen() || !index_) return false;
  auto prepared=std::make_unique<ShixinTypoInitialCache>();
  path source=sidecar_path.empty()?path(file_path().u8string()+".initial3"):path(sidecar_path);
  const shixin_typo::BoundIdentity identity{dict_file_checksum(),index_->size,metadata_->num_entries,file_size()};
  prepared->initials.resize(index_->size);
  for(size_t a=0;a<index_->size;++a) {
    auto spelling=GetSyllableById(a);
    prepared->initials[a]=spelling.empty()?0:spelling[0];
  }
  if(!shixin_typo::LoadInitialCache(source,identity,prepared->initials,&prepared->cache)) return false;
  shixin_typo_initial_cache_=std::move(prepared);return true;
}

bool Table::PrepareShixinTypoMirrorCache(const string& mirror_path) {
  if (!shixin_typo_initial_cache_) return false;
  if (shixin_typo_initial_cache_->mirror_initialized)
    return shixin_typo_initial_cache_->mirror_path == mirror_path && !shixin_typo_initial_cache_->mirror.native.empty();
  shixin_typo_initial_cache_->mirror_required = true;
  shixin_typo_initial_cache_->mirror_initialized = true;
  shixin_typo_initial_cache_->mirror_path = mirror_path;
  const shixin_typo::BoundIdentity identity{dict_file_checksum(), index_->size, metadata_->num_entries, file_size()};
  return shixin_typo::LoadMirror(path(mirror_path), identity,
      shixin_typo_initial_cache_->cache, &shixin_typo_initial_cache_->mirror);
}

// Offline experiment: caller supplies a locally built, trusted native Table.
// This is not a validator for arbitrary untrusted table files.
bool Table::PrepareShixinTypoCanonicalTable(const string& file) {
  if (shixin_typo_canonical_initialized_)
    return file == shixin_typo_canonical_path_ && bool(shixin_typo_canonical_table_);
  shixin_typo_canonical_initialized_ = true;
  shixin_typo_canonical_path_ = file;
  the<Table> prepared(new Table(path(file)));
  if (!prepared->Load() || !metadata_ ||
      prepared->metadata()->num_syllables != metadata_->num_syllables)
    return false;
  for (size_t i = 0; i < metadata_->num_syllables; ++i)
    if (prepared->GetSyllableById(i) != GetSyllableById(i))
      return false;
  if (!prepared->WarmShixinTypoPages()) return false;
  shixin_typo_canonical_table_ = std::move(prepared);
  return true;
}

// Explicit schema-initialization experiment, never called from Query().
bool Table::WarmShixinTypoPages() {
  if (!IsOpen()) return false;
  volatile unsigned char touched = 0;
  for (size_t i = 0; i < file_size(); i += 4096)
    touched ^= static_cast<unsigned char>(address()[i]);
  (void)touched;
  return true;
}

bool Table::WarmShixinTypoStrings() {
  if (!IsOpen() || !metadata_ || !metadata_->string_table) return false;
  const char* strings = metadata_->string_table.get();
  const size_t bytes = metadata_->string_table_size;
  const uintptr_t base = reinterpret_cast<uintptr_t>(address());
  const uintptr_t start = reinterpret_cast<uintptr_t>(strings);
  if (start < base || start - base > file_size() ||
      bytes > file_size() - (start - base)) return false;
  volatile unsigned char touched = 0;
  for (size_t i = 0; i < bytes; i += 4096)
    touched ^= static_cast<unsigned char>(strings[i]);
  if (bytes) touched ^= static_cast<unsigned char>(strings[bytes - 1]);
  (void)touched;
  return true;
}

bool Table::WarmShixinTypoIndexes(bool include_short_entries) {
  if (!IsOpen() || !index_) return false;
  volatile unsigned char touched = 0;
  auto touch = [&](const void* pointer, size_t header, size_t count, size_t item) {
    if (!header && !count) return true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(address());
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < base || start - base > file_size()) return false;
    const size_t available = file_size() - (start - base);
    if (header > available || count > (available - header) / item) return false;
    const size_t bytes = header + count * item;
    const char* data = static_cast<const char*>(pointer);
    for (size_t i = 0; i < bytes; i += 4096)
      touched ^= static_cast<unsigned char>(data[i]);
    if (bytes) touched ^= static_cast<unsigned char>(data[bytes-1]);
    return true;
  };
  if (!touch(index_, offsetof(table::Index, at), index_->size,
             sizeof(table::HeadIndexNode))) return false;
  for (const auto& a : *index_) {
    if (include_short_entries && !touch(a.entries.at.get(), 0, a.entries.size, sizeof(table::Entry))) return false;
    if (!a.next_level) continue;
    const auto& two = a.next_level->trunk();
    if (!touch(&two, offsetof(table::TrunkIndex, at), two.size,
               sizeof(table::TrunkIndexNode))) return false;
    for (const auto& b : two) {
      if (include_short_entries && !touch(b.entries.at.get(), 0, b.entries.size, sizeof(table::Entry))) return false;
      if (!b.next_level) continue;
      const auto& three = b.next_level->trunk();
      if (!touch(&three, offsetof(table::TrunkIndex, at), three.size,
                 sizeof(table::TrunkIndexNode))) return false;
      if (include_short_entries)
        for (const auto& c : three)
          if (!touch(c.entries.at.get(), 0, c.entries.size, sizeof(table::Entry))) return false;
    }
  }
  (void)touched;
  return true;
}

Table::Table(const path& file_path) : MappedFile(file_path) {}

Table::~Table() {}

bool Table::Load() {
  shixin_typo_canonical_table_.reset();
  shixin_typo_canonical_path_.clear();
  shixin_typo_canonical_initialized_ = false;
  shixin_typo_initial_cache_.reset();
  LOG(INFO) << "loading table file: " << file_path();

  if (IsOpen())
    Close();

  if (!OpenReadOnly()) {
    LOG(ERROR) << "Error opening table file '" << file_path() << "'.";
    return false;
  }

  metadata_ = Find<table::Metadata>(0);
  if (!metadata_) {
    LOG(ERROR) << "metadata not found.";
    Close();
    return false;
  }
  if (strncmp(metadata_->format, kTableFormatPrefix, kTableFormatPrefixLen)) {
    LOG(ERROR) << "invalid metadata.";
    Close();
    return false;
  }
  double format_version = atof(&metadata_->format[kTableFormatPrefixLen]);
  if (format_version < kTableFormatLowestCompatible - DBL_EPSILON) {
    LOG(ERROR) << "table format version " << format_version
               << " is no longer supported. please upgrade to version "
               << kTableFormatLatest;
    return false;
  }

  syllabary_ = metadata_->syllabary.get();
  if (!syllabary_) {
    LOG(ERROR) << "syllabary not found.";
    Close();
    return false;
  }
  index_ = metadata_->index.get();
  if (!index_) {
    LOG(ERROR) << "table index not found.";
    Close();
    return false;
  }

  return OnLoad();
}

bool Table::Save() {
  LOG(INFO) << "saving table file: " << file_path();

  if (!index_) {
    LOG(ERROR) << "the table has not been constructed!";
    return false;
  }

  return ShrinkToFit();
}

uint32_t Table::dict_file_checksum() const {
  return metadata_ ? metadata_->dict_file_checksum : 0;
}

bool Table::Build(const Syllabary& syllabary,
                  const Vocabulary& vocabulary,
                  size_t num_entries,
                  uint32_t dict_file_checksum) {
  const size_t kReservedSize = 4096;
  size_t num_syllables = syllabary.size();
  size_t estimated_file_size =
      kReservedSize + 32 * num_syllables + 64 * num_entries;
  LOG(INFO) << "building table.";
  LOG(INFO) << "num syllables: " << num_syllables;
  LOG(INFO) << "num entries: " << num_entries;
  LOG(INFO) << "estimated file size: " << estimated_file_size;
  if (!Create(estimated_file_size)) {
    LOG(ERROR) << "Error creating table file '" << file_path() << "'.";
    return false;
  }

  LOG(INFO) << "creating metadata.";
  metadata_ = Allocate<table::Metadata>();
  if (!metadata_) {
    LOG(ERROR) << "Error creating metadata in file '" << file_path() << "'.";
    return false;
  }
  metadata_->dict_file_checksum = dict_file_checksum;
  metadata_->num_syllables = num_syllables;
  metadata_->num_entries = num_entries;

  if (!OnBuildStart()) {
    return false;
  }

  LOG(INFO) << "creating syllabary.";
  syllabary_ = CreateArray<table::StringType>(num_syllables);
  if (!syllabary_) {
    LOG(ERROR) << "Error creating syllabary.";
    return false;
  } else {
    size_t i = 0;
    for (const string& syllable : syllabary) {
      AddString(syllable, &syllabary_->at[i++], 0.0);
    }
  }
  metadata_->syllabary = syllabary_;

  LOG(INFO) << "creating table index.";
  index_ = BuildIndex(vocabulary, num_syllables);
  if (!index_) {
    LOG(ERROR) << "Error creating table index.";
    return false;
  }
  metadata_->index = index_;

  if (!OnBuildFinish()) {
    return false;
  }

  // at last, complete the metadata
  std::strncpy(metadata_->format, kTableFormatLatest,
               table::Metadata::kFormatMaxLength);
  return true;
}

table::Index* Table::BuildIndex(const Vocabulary& vocabulary,
                                size_t num_syllables) {
  return reinterpret_cast<table::Index*>(
      BuildHeadIndex(vocabulary, num_syllables));
}

table::HeadIndex* Table::BuildHeadIndex(const Vocabulary& vocabulary,
                                        size_t num_syllables) {
  auto index = CreateArray<table::HeadIndexNode>(num_syllables);
  if (!index) {
    return NULL;
  }
  for (const auto& v : vocabulary) {
    int syllable_id = v.first;
    auto& node(index->at[syllable_id]);
    const auto& entries(v.second.entries);
    if (!BuildEntryList(entries, &node.entries)) {
      return NULL;
    }
    if (v.second.next_level) {
      Code code;
      code.push_back(syllable_id);
      auto next_level_index = BuildTrunkIndex(code, *v.second.next_level);
      if (!next_level_index) {
        return NULL;
      }
      node.next_level = reinterpret_cast<table::PhraseIndex*>(next_level_index);
    }
  }
  return index;
}

table::TrunkIndex* Table::BuildTrunkIndex(const Code& prefix,
                                          const Vocabulary& vocabulary) {
  auto index = CreateArray<table::TrunkIndexNode>(vocabulary.size());
  if (!index) {
    return NULL;
  }
  size_t count = 0;
  for (const auto& v : vocabulary) {
    int syllable_id = v.first;
    auto& node(index->at[count++]);
    node.key = syllable_id;
    const auto& entries(v.second.entries);
    if (!BuildEntryList(entries, &node.entries)) {
      return NULL;
    }
    if (v.second.next_level) {
      Code code(prefix);
      code.push_back(syllable_id);
      if (code.size() < Code::kIndexCodeMaxLength) {
        auto next_level_index = BuildTrunkIndex(code, *v.second.next_level);
        if (!next_level_index) {
          return NULL;
        }
        node.next_level =
            reinterpret_cast<table::PhraseIndex*>(next_level_index);
      } else {
        auto tail_index = BuildTailIndex(code, *v.second.next_level);
        if (!tail_index) {
          return NULL;
        }
        node.next_level = reinterpret_cast<table::PhraseIndex*>(tail_index);
      }
    }
  }
  return index;
}

table::TailIndex* Table::BuildTailIndex(const Code& prefix,
                                        const Vocabulary& vocabulary) {
  if (vocabulary.find(-1) == vocabulary.end()) {
    return NULL;
  }
  const auto& page(vocabulary.find(-1)->second);
  DLOG(INFO) << "page size: " << page.entries.size();
  auto index = CreateArray<table::LongEntry>(page.entries.size());
  if (!index) {
    return NULL;
  }
  size_t count = 0;
  for (const auto& src : page.entries) {
    DLOG(INFO) << "count: " << count;
    DLOG(INFO) << "entry: " << src->text;
    auto& dest(index->at[count++]);
    size_t extra_code_length = src->code.size() - Code::kIndexCodeMaxLength;
    DLOG(INFO) << "extra code length: " << extra_code_length;
    dest.extra_code.size = extra_code_length;
    dest.extra_code.at = Allocate<SyllableId>(extra_code_length);
    if (!dest.extra_code.at) {
      LOG(ERROR) << "Error creating code sequence; file size: " << file_size();
      return NULL;
    }
    std::copy(src->code.begin() + Code::kIndexCodeMaxLength, src->code.end(),
              dest.extra_code.begin());
    BuildEntry(*src, &dest.entry);
  }
  return index;
}

Array<table::Entry>* Table::BuildEntryArray(const ShortDictEntryList& entries) {
  auto array = CreateArray<table::Entry>(entries.size());
  if (!array) {
    return NULL;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!BuildEntry(*entries[i], &array->at[i])) {
      return NULL;
    }
  }
  return array;
}

bool Table::BuildEntryList(const ShortDictEntryList& src,
                           List<table::Entry>* dest) {
  if (!dest)
    return false;
  dest->size = src.size();
  dest->at = Allocate<table::Entry>(src.size());
  if (!dest->at) {
    LOG(ERROR) << "Error creating table entries; file size: " << file_size();
    return false;
  }
  size_t i = 0;
  for (auto d = src.begin(); d != src.end(); ++d, ++i) {
    if (!BuildEntry(**d, &dest->at[i]))
      return false;
  }
  return true;
}

bool Table::BuildEntry(const ShortDictEntry& dict_entry, table::Entry* entry) {
  if (!entry)
    return false;
  if (!AddString(dict_entry.text, &entry->text, dict_entry.weight)) {
    LOG(ERROR) << "Error creating table entry '" << dict_entry.text
               << "'; file size: " << file_size();
    return false;
  }
  entry->weight = static_cast<table::Weight>(dict_entry.weight);
  return true;
}

bool Table::GetSyllabary(Syllabary* result) {
  if (!result || !syllabary_)
    return false;
  for (size_t i = 0; i < syllabary_->size; ++i) {
    result->insert(GetSyllableById(static_cast<SyllableId>(i)));
  }
  return true;
}
string Table::GetSyllableById(SyllableId syllable_id) {
  if (!syllabary_ || syllable_id < 0 ||
      syllable_id >= static_cast<SyllableId>(syllabary_->size))
    return string();
  return GetString(syllabary_->at[syllable_id]);
}

TableAccessor Table::QueryWords(SyllableId syllable_id) {
  TableQuery query(index_);
  return query.Access(syllable_id);
}

TableAccessor Table::QueryPhrases(const Code& code) {
  if (code.empty())
    return TableAccessor();
  TableQuery query(index_);
  for (size_t i = 0; i < Code::kIndexCodeMaxLength; ++i) {
    if (code.size() == i + 1)
      return query.Access(code[i]);
    if (!query.Advance(code[i]))
      return TableAccessor();
  }
  return query.Access(-1);
}

// log(0.05) ≈ -3.0
const double kPenaltyForAmbiguousSyllable = -2.995732274;

bool Table::Query(const SyllableGraph& syll_graph,
                  size_t start_pos,
                  TableQueryResult* result,
                  bool (*visitor)(void*, size_t, TableAccessor&),
                  void* visitor_context) {
  if (!result || !index_ || start_pos >= syll_graph.interpreted_length)
    return false;
  result->clear();
  if(syll_graph.shixin_typo_three_initials) {
    // Bounds are prepared during schema initialization, never lazily on a key.
    if(!shixin_typo_initial_cache_ || start_pos!=0 || !syll_graph.shixin_typo_graph_complete) { syll_graph.FailShixinTypo(ShixinTypoStopReason::Other);return false; }
    const auto found=syll_graph.shixin_typo_initial_exact ? shixin_typo_initial_cache_->search.Exact(
        std::string_view(syll_graph.shixin_typo_raw_initials.data(),3),
        shixin_typo_initial_cache_->cache,
        std::min(syll_graph.shixin_typo_work_left,syll_graph.shixin_typo_probe_cap),syll_graph.shixin_typo_deadline)
        : shixin_typo_initial_cache_->search.Search(
        std::string_view(syll_graph.shixin_typo_raw_initials.data(),3),
        shixin_typo_initial_cache_->cache,
        std::min(syll_graph.shixin_typo_work_left,syll_graph.shixin_typo_probe_cap),syll_graph.shixin_typo_deadline);
    syll_graph.shixin_typo_work_left-=std::min(syll_graph.shixin_typo_work_left,found.probes);
    if(!found.complete) {
      syll_graph.FailShixinTypo(ShixinTypoStopReason::Other);return false;
    }
    for(size_t i=0;i<found.count;++i) {
      if(!syll_graph.CheckShixinTypoDeadline() || !syll_graph.SpendShixinTypoNode()) { result->clear(); return false; }
      const auto& candidate=found.entries[i];
      size_t corrections=0;double credibility=0,quality=0;bool valid=true;
      for(size_t pos=0;pos<3;++pos) {
        if(!syll_graph.SpendShixinTypoWork()) { result->clear(); return false; }
        auto begin=syll_graph.edges.find(pos);
        if(begin==syll_graph.edges.end()) {valid=false;break;}
        auto end=begin->second.find(pos+1);
        if(end==begin->second.end()) {valid=false;break;}
        auto spelling=end->second.find(candidate.code[pos]);
        if(spelling==end->second.end()) {valid=false;break;}
        corrections+=spelling->second.is_correction;
        credibility+=spelling->second.credibility;
        quality+=spelling->second.type==kNormalSpelling ? 1 : 0;
      }
      if(!valid || corrections!=1) continue;
      Code code;code.assign(candidate.code.begin(),candidate.code.end());
      const table::Entry* entry = nullptr;
      std::string_view cached_text;
      if (shixin_typo_initial_cache_->mirror_required) {
        const auto& mirror = shixin_typo_initial_cache_->mirror;
        const size_t index = mirror.Find(candidate, shixin_typo_initial_cache_->cache,
                                        shixin_typo_initial_cache_->initials);
        if (index >= mirror.native.size()) { syll_graph.FailShixinTypo(ShixinTypoStopReason::Other);result->clear();return false; }
        entry = &mirror.native[index];
        cached_text = mirror.Text(index);
      } else {
        // The offset-only experiment still verifies native membership per query.
        auto native=QueryPhrases(code);
        if(!syll_graph.CheckShixinTypoDeadline()) { result->clear(); return false; }
        entry=Find<table::Entry>(candidate.offset);
        bool member=false;
        for(;!native.exhausted();native.Next()) {
          if(!syll_graph.SpendShixinTypoWork()) { result->clear(); return false; }
          if(native.entry()==entry) {member=true;break;}
        }
        if(!member || entry->weight!=candidate.weight) {syll_graph.FailShixinTypo(ShixinTypoStopReason::Other);result->clear();return false;}
      }
      TableAccessor accessor(code,entry,credibility,quality,cached_text);
      if(visitor) { if(!visitor(visitor_context,3,accessor)) { result->clear(); return false; } }
      else (*result)[3].push_back(accessor);
      if(!syll_graph.CheckShixinTypoDeadline()) { result->clear(); return false; }
    }
    if(!syll_graph.CheckShixinTypoDeadline()) { result->clear(); return false; }
    return visitor || !result->empty();
  }
  std::deque<std::tuple<size_t, TableQuery, size_t>> q;
  TableQuery initial_state(index_);
  q.push_back({start_pos, initial_state, 0});
  while (!q.empty()) {
    if (!syll_graph.SpendShixinTypoNode()) { result->clear(); return false; }
    // Corrected lookup completes a phrase branch before expanding every
    // sibling prefix. Ordinary lookup retains its existing breadth-first order.
    auto& pending = syll_graph.shixin_typo_bounded ? q.back() : q.front();
    size_t current_pos = std::get<0>(pending);
    TableQuery query(std::move(std::get<1>(pending)));
    size_t corrections = std::get<2>(pending);
    if (syll_graph.shixin_typo_bounded) q.pop_back(); else q.pop_front();
    auto index = syll_graph.indices.find(current_pos);
    if (index == syll_graph.indices.end()) {
      continue;
    }
    if (query.level() == Code::kIndexCodeMaxLength) {
      TableAccessor accessor(query.Access(-1));
      if (!accessor.exhausted()) {
        if (visitor) { if (!visitor(visitor_context,current_pos,accessor)) return false; }
        else (*result)[current_pos].push_back(accessor);
      }
      continue;
    }
    // Intersect the smaller native dictionary child set with the graph.
    // Initial abbreviations can otherwise probe every expanded syllable at
    // every sparse dictionary node, mostly finding no matching child.
    auto child_index = syll_graph.shixin_typo_bounded ? query.ShixinTypoChildren() : nullptr;
    bool sparse = child_index && child_index->size < index->second.size();
    size_t child = 0;
    auto spelling_it = index->second.begin();
    while (sparse ? child < child_index->size : spelling_it != index->second.end()) {
      auto current = spelling_it;
      if (sparse) {
        if (!syll_graph.SpendShixinTypoWork()) { result->clear(); return false; }
        current = index->second.find(child_index->at[child++].key);
        if (current == index->second.end()) continue;
      } else {
        ++spelling_it;
      }
      const auto& spellings = *current;
      SyllableId syll_id = spellings.first;
      for (auto props : spellings.second) {
        if (!syll_graph.SpendShixinTypoWork()) { result->clear(); return false; }
        if (syll_graph.shixin_typo_bounded && corrections && props->is_correction) continue;
        size_t end_pos = props->end_pos;

        double penalty = 0.0;
        if (false) {
          size_t last_pos = query.last_pos();
          if (props->ambiguous_source_positions.count(last_pos)) {
            penalty = kPenaltyForAmbiguousSyllable;
            DLOG(INFO) << "conditional penalty applied: ambiguous path ["
                       << last_pos << ", " << end_pos << ")";
          }
        }
        double next_credibility = props->credibility + penalty;

        // 全碼匹配長度積分
        bool is_normal_spelling = props->type == kNormalSpelling;
        double delta_quality_len =
            (is_normal_spelling ? 1.0 : 0.0) * (end_pos - current_pos);
        TableAccessor accessor =
            query.Access(syll_id, next_credibility, delta_quality_len);
        if (!accessor.exhausted()) {
          if (visitor) { if (!visitor(visitor_context,end_pos,accessor)) return false; }
          else (*result)[end_pos].push_back(accessor);
        }
        if (end_pos < syll_graph.interpreted_length &&
            query.Advance(syll_id, next_credibility, delta_quality_len,
                          current_pos)) {
          q.push_back({end_pos, query, corrections + props->is_correction});
          query.Backdate();
        }
      }
    }
  }
  return visitor || !result->empty();
}

string Table::GetEntryText(const table::Entry& entry) {
  return GetString(entry.text);
}

}  // namespace rime
