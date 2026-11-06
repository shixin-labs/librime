// Offline builder entry point. No session or user dictionary is opened.
#include <rime/dict/table.h>
#include <rime/dict/mirror_cache.h>
#include <unordered_map>

SHIXIN_TYPO_EXPORT int32_t shixin_typo_build_resources_v1(
    const char* table_file, const char* initial_file, const char* mirror_file) {
  if (!table_file || !initial_file || !mirror_file) return -1;
  try {
    rime::Table table{rime::path(std::string(table_file))};
    if (!table.Load()) return -2;
    auto* meta=table.metadata();
    auto* index=meta->index.get();
    if(!index || index->size>65535 || table.file_size()>UINT32_MAX) return -3;
    shixin_typo::BoundIdentity identity{table.dict_file_checksum(),index->size,meta->num_entries,table.file_size()};
    std::vector<char> initials(index->size);
    for(size_t i=0;i<initials.size();++i) {
      auto spelling=table.GetSyllableById(i);
      initials[i]=spelling.empty()?0:spelling[0];
    }
    std::vector<std::vector<shixin_typo::CachedReading>> buckets(shixin_typo::InitialCache::codes);
    for(size_t a=0;a<index->size;++a) {
      if(initials[a]<'a'||initials[a]>'z') continue;
      auto second=index->at[a].next_level.get(); if(!second) continue;
      const auto& children=second->trunk();
      for(size_t b=0;b<children.size;++b) {
        const auto bi=children.at[b].key;
        if(bi>=initials.size()||initials[bi]<'a'||initials[bi]>'z') continue;
        auto third=children.at[b].next_level.get(); if(!third) continue;
        const auto& leaves=third->trunk();
        for(size_t c=0;c<leaves.size;++c) {
          const auto ci=leaves.at[c].key;
          if(ci>=initials.size()||initials[ci]<'a'||initials[ci]>'z') continue;
          const std::array<char,3> code{initials[a],initials[bi],initials[ci]};
          auto& bucket=buckets[shixin_typo::InitialCode({code.data(),3})];
          const auto& entries=leaves.at[c].entries;
          for(size_t i=0;i<entries.size;++i) {
            const auto& entry=entries.at[i];
            if(!std::isfinite(entry.weight)) return -4;
            shixin_typo::CachedReading value{uint32_t(reinterpret_cast<const char*>(&entry)-table.Find<char>(0)),{uint16_t(a),uint16_t(bi),uint16_t(ci)},0,entry.weight};
            auto where=std::lower_bound(bucket.begin(),bucket.end(),value,shixin_typo::BetterReading);
            if(where!=bucket.end() || bucket.size()<8) {
              bucket.insert(where,value); if(bucket.size()>8) bucket.pop_back();
            }
          }
        }
      }
    }
    shixin_typo::InitialCache cache; cache.offsets.push_back(0);
    for(const auto& bucket:buckets) {
      cache.entries.insert(cache.entries.end(),bucket.begin(),bucket.end());
      cache.offsets.push_back(uint32_t(cache.entries.size()));
    }
    shixin_typo::MirrorCache mirror;
    mirror.native.resize(cache.entries.size()); mirror.spans.resize(cache.entries.size());
    std::unordered_map<std::string,uint32_t> pool;
    for(size_t i=0;i<cache.entries.size();++i) {
      const auto* entry=table.Find<rime::table::Entry>(cache.entries[i].offset);
      auto text=table.GetEntryText(*entry);
      if(!shixin_typo::ValidMirrorUTF8(text)) return -5;
      auto item=pool.emplace(text,uint32_t(mirror.text.size()));
      if(item.second) { mirror.text.insert(mirror.text.end(),text.begin(),text.end()); mirror.text.push_back(0); }
      mirror.spans[i]={item.first->second,uint32_t(text.size())}; mirror.native[i]=*entry;
    }
    if(!shixin_typo::SaveInitialCache(rime::path(initial_file),identity,cache)) return -6;
    if(!shixin_typo::SaveMirror(rime::path(mirror_file),identity,cache,mirror)) return -7;
    return 0;
  } catch(...) { return -8; }
}
