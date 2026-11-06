#include <gtest/gtest.h>
#include <rime/algo/syllabifier.h>
#include <rime/translation.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <rime/dict/hypothesis_policy.h>
#include <rime/dict/initial_cache.h>
namespace rime {
#include <rime/gear/hypothesis_translation.inc>
}
static rime::SyllableGraph Budget(size_t work=32768) {
  rime::SyllableGraph b; b.shixin_typo_bounded=true; b.shixin_typo_work_left=work;
  b.shixin_typo_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  return b;
}
TEST(HypothesisPolicy, ModelValidation) {
  auto dir=(std::filesystem::temp_directory_path()/ ("shixin-typo-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
  ASSERT_TRUE(std::filesystem::create_directory(dir));
  auto path=std::string(dir)+"/model";
  shixin_typo::HypothesisPolicy policy;
  for(const auto& text:std::vector<std::string>{"", "a\t\t0\n", "a\tnan\t0\n", "a\t1\t0\n", "b\t-1\t0\na\t-1\t0\n", std::string(100,'a')+"\n", "a\t-1\t9\n", std::string("a\t-1\t0\0hidden\n", 14)}) {
    { std::ofstream out(path); out<<text; }
    EXPECT_FALSE(policy.Load(path));
  }
  { std::ofstream out(path); out<<"hao\t-1\t0\nni\t-1\t0\n"; }
  ASSERT_TRUE(policy.Load(path));
  { std::ofstream out(path, std::ios::binary); out<<"hao\t-1\t0\r\nni\t-1\t0\r\n"; }
  ASSERT_TRUE(policy.Load(path));
  for(auto raw:{"nihoa","nihso"}) {
    auto budget=Budget(); auto plan=policy.Plan(raw,budget);
    ASSERT_TRUE(plan.complete); ASSERT_EQ(plan.count,1u);
    EXPECT_EQ(plan.hypotheses[0].spelling(),"nihao");
    EXPECT_FALSE(plan.abbreviated);
    auto exhausted=Budget(1); EXPECT_FALSE(policy.Plan(raw,exhausted).complete);
    EXPECT_TRUE(exhausted.shixin_typo_exhausted);
    auto expired=Budget(); expired.shixin_typo_deadline=std::chrono::steady_clock::now();
    EXPECT_FALSE(policy.Plan(raw,expired).complete);
  }
  std::filesystem::remove_all(dir);
}
TEST(HypothesisPolicy, RealModelAndInputClasses) {
  shixin_typo::HypothesisPolicy policy;
  ASSERT_TRUE(policy.Load("typo.forms.tsv"));
  auto measured=Budget();
  auto measured_plan=policy.Plan("nihso",measured);
  EXPECT_TRUE(measured_plan.complete);
  for(auto raw:{"nihso","niaho","ni'hso","xgr","zhongguo","zhonggr"}) {
    auto budget=Budget(); auto plan=policy.Plan(raw,budget);
    EXPECT_TRUE(plan.complete); EXPECT_LE(plan.count,plan.abbreviated?16u:8u);
    EXPECT_GT(plan.count,0u);
    if(std::getenv("SHIXIN_SPIKE_PRINT_CORRECTIONS")) {
      std::cout<<raw<<" plan:";
      for(size_t i=0;i<plan.count;++i) std::cout<<" "<<plan.hypotheses[i].spelling();
      std::cout<<"\n";
    }
  }
  for(auto raw:{"", "ab", "ni''hao", "'nihao", "nihao'", "NIHAO", "abcdefghijklmnopqrstuvwxyzabcdefg"}) {
    auto budget=Budget(); EXPECT_FALSE(policy.Plan(raw,budget).complete);
  }
}
TEST(HypothesisPolicy, ExactInitialBucketHasNoNeighborExpansion) {
  shixin_typo::InitialCache cache;
  const auto bucket=shixin_typo::InitialCode("zgr");
  cache.offsets.resize(shixin_typo::InitialCache::codes+1);
  cache.entries.resize(12);
  for(size_t i=0;i<12;++i) cache.entries[i].offset=uint32_t(i+1);
  for(size_t i=bucket+1;i<cache.offsets.size();++i) cache.offsets[i]=12;
  shixin_typo::InitialCacheSearch search;
  auto r=search.Exact("zgr",cache,32768,std::chrono::steady_clock::time_point::max());
  EXPECT_TRUE(r.complete); EXPECT_EQ(r.count,8u); EXPECT_EQ(r.probes,8u);
  EXPECT_EQ(r.entries[0].offset,1u); EXPECT_EQ(r.entries[7].offset,8u);
  auto limited=search.Exact("zgr",cache,1,std::chrono::steady_clock::time_point::max());
  EXPECT_FALSE(limited.complete); EXPECT_EQ(limited.probes,1u);
  EXPECT_FALSE(search.Exact("zgr",cache,32768,std::chrono::steady_clock::now()).complete);
  EXPECT_FALSE(search.Exact("Zgr",cache,32768,std::chrono::steady_clock::time_point::max()).complete);
}
TEST(HypothesisPool, PerSpellingCapFairnessAndDiscard) {
  rime::ShixinTypoHypothesisTranslation pool;
  for(size_t h=0;h<16;++h) for(size_t i=0;i<12;++i)
    pool.Add(h,rime::New<rime::SimpleCandidate>("phrase",0,5,std::to_string(h)+":"+std::to_string(i)));
  auto budget=Budget(); pool.Finish();
  pool.Finish(); // Publication is idempotent; cannot overflow the fixed array.
  pool.Add(16,nullptr); pool.Discard(16);
  size_t count=0;
  while(!pool.exhausted()) {
    EXPECT_EQ(pool.Peek()->text(),std::to_string(count%16)+":"+std::to_string(count/16));
    ++count; pool.Next();
  }
  EXPECT_EQ(count,160u);
  rime::ShixinTypoHypothesisTranslation drop;
  drop.Add(0,rime::New<rime::SimpleCandidate>("phrase",0,5,"a")); drop.Discard(0);
  drop.Finish(); EXPECT_TRUE(drop.exhausted());

}

TEST(HypothesisPolicy, BoundedMalformedModelAndInputStress) {
  auto dir=(std::filesystem::temp_directory_path()/ ("shixin-typo-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
  ASSERT_TRUE(std::filesystem::create_directory(dir));
  auto path=std::string(dir)+"/model";
  shixin_typo::HypothesisPolicy policy;
  uint32_t state=0x783921;
  auto next=[&]() { state^=state<<13; state^=state>>17; state^=state<<5; return state; };
  for(size_t run=0;run<256;++run) {
    std::string content(next()%256,'x');
    for(auto& c:content) c=char(next()%128);
    { std::ofstream out(path,std::ios::binary); out.write(content.data(),content.size()); }
    EXPECT_FALSE(policy.Load(path));
    auto budget=Budget(512); EXPECT_FALSE(policy.Plan("nihso",budget).complete);
  }
  { std::ofstream out(path); out<<"a\t-1\t0\nhao\t-1\t0\nni\t-1\t0\ns\t-1\t1\n"; }
  ASSERT_TRUE(policy.Load(path));
  for(size_t run=0;run<1000;++run) {
    std::array<char,40> input{}; auto length=next()%input.size();
    for(size_t i=0;i<length;++i) input[i]="ahinos'"[next()%7];
    auto budget=Budget(512); auto plan=policy.Plan({input.data(),length},budget);
    EXPECT_LE(plan.count,16u); EXPECT_LE(budget.shixin_typo_work_left,512u);
    if(budget.shixin_typo_exhausted) EXPECT_FALSE(plan.complete);
  }
  std::filesystem::remove_all(dir);
}
