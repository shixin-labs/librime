//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <rime/algo/utilities.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dictionary.h>

namespace {

using namespace rime;
namespace fs = std::filesystem;

class RimeDictCompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TearDown();
    WriteDictionary("中\tzhong\t10\n");
    dict_.reset(new Dictionary(
        "prism_compile_test", {}, {New<Table>(table_)}, New<Prism>(prism_)));
    ASSERT_TRUE(CompileSchema(schema_));
  }

  void TearDown() override {
    dict_.reset();
    for (const auto& file : {source_, schema_file_, table_, prism_, reverse_}) {
      fs::remove(file);
    }
  }

  void WriteDictionary(const string& entries) {
    std::ofstream out(source_);
    out << "---\nname: prism_compile_test\nversion: '1'\n"
           "sort: by_weight\nuse_preset_vocabulary: false\n...\n"
        << entries;
    ASSERT_TRUE(out.good());
  }

  bool CompileSchema(const string& schema, int options = 0) {
    {
      std::ofstream out(schema_file_);
      out << schema;
      if (!out.good())
        return false;
    }
    DictCompiler compiler(dict_.get());
    compiler.set_options(options);
    return compiler.Compile(schema_file_);
  }

  fs::file_time_type MarkPrism() {
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    fs::last_write_time(prism_, old);
    return fs::last_write_time(prism_);
  }

  bool HasKey(const string& key) {
    Prism prism(prism_);
    EXPECT_TRUE(prism.Load());
    return prism.HasKey(key);
  }

  const path source_{"prism_compile_test.dict.yaml"};
  const path schema_file_{"prism_compile_test.schema.yaml"};
  const path table_{"prism_compile_test.table.bin"};
  const path prism_{"prism_compile_test.prism.bin"};
  const path reverse_{"prism_compile_test.reverse.bin"};
  const string schema_ =
      "menu: {page_size: 5}\n"
      "speller:\n  alphabet: abcdefghijklmnopqrstuvwxyz\n"
      "  delimiter: \"'\"\n  algebra: [derive/^zh/z/]\n";
  the<Dictionary> dict_;
};

TEST_F(RimeDictCompilerTest, ReusesPrismForUnrelatedSettings) {
  const auto old = MarkPrism();
  ASSERT_TRUE(CompileSchema(schema_ +
      "translator: {enable_sentence: false}\n"
      "switches: [{name: emoji, reset: 0}]\n"
      "__build_info: {timestamps: {default: 12345}}\n"));
  EXPECT_EQ(old, fs::last_write_time(prism_));
  EXPECT_TRUE(HasKey("zong"));
  const auto page = schema_.find("page_size: 5");
  auto changed = schema_;
  changed.replace(page, string("page_size: 5").size(), "page_size: 9");
  ASSERT_TRUE(CompileSchema(changed));
  EXPECT_EQ(old, fs::last_write_time(prism_));
}

TEST_F(RimeDictCompilerTest, CanonicalizesSpeller) {
  const auto old = MarkPrism();
  ASSERT_TRUE(CompileSchema(
      "# Same spelling rules, different YAML formatting and key order.\n"
      "speller: {algebra: ['derive/^zh/z/'], delimiter: \"'\", "
      "alphabet: abcdefghijklmnopqrstuvwxyz}\nmenu: {page_size: 5}\n"));
  EXPECT_EQ(old, fs::last_write_time(prism_));
}

TEST_F(RimeDictCompilerTest, RebuildsForSpellingChangesAndRemoval) {
  const auto old = MarkPrism();
  auto changed = schema_;
  changed.replace(changed.find("derive/^zh/z/"), string("derive/^zh/z/").size(),
                  "derive/^zh/j/");
  ASSERT_TRUE(CompileSchema(changed));
  EXPECT_NE(old, fs::last_write_time(prism_));
  EXPECT_TRUE(HasKey("jong"));
  EXPECT_FALSE(HasKey("zong"));
  const auto derived = MarkPrism();
  ASSERT_TRUE(CompileSchema("menu: {page_size: 5}\n"));
  EXPECT_NE(derived, fs::last_write_time(prism_));
  EXPECT_TRUE(HasKey("zhong"));
  EXPECT_FALSE(HasKey("jong"));
}

TEST_F(RimeDictCompilerTest, RebuildsForDictionaryChanges) {
  const auto old = MarkPrism();
  WriteDictionary("中\tzhong\t10\n新\txin\t5\n");
  ASSERT_TRUE(CompileSchema(schema_));
  EXPECT_NE(old, fs::last_write_time(prism_));
  EXPECT_TRUE(HasKey("xin"));
}

TEST_F(RimeDictCompilerTest, PreservesNullAlgebraEntries) {
  const auto old = MarkPrism();
  auto changed = schema_;
  changed.replace(changed.find("derive/^zh/z/]"), string("derive/^zh/z/]").size(),
                  "derive/^zh/z/, null]");
  ASSERT_TRUE(CompileSchema(changed));
  EXPECT_NE(old, fs::last_write_time(prism_));
  // Projection rejects a null formula and falls back to the base syllabary.
  EXPECT_TRUE(HasKey("zhong"));
  EXPECT_FALSE(HasKey("zong"));
}

TEST_F(RimeDictCompilerTest, ExplicitRebuildStillWorks) {
  const auto old = MarkPrism();
  ASSERT_TRUE(CompileSchema(schema_, DictCompiler::kRebuildPrism));
  EXPECT_NE(old, fs::last_write_time(prism_));
}

TEST_F(RimeDictCompilerTest, MigratesWholeSchemaChecksum) {
  // Simulate a prism written by unpatched librime. It should be rebuilt
  // once, with the actual spelling algebra, then reused on the next deploy.
  {
    Prism legacy(prism_);
    ASSERT_TRUE(legacy.Build({"zhong"}, nullptr, Checksum(source_),
                             Checksum(schema_file_)));
    ASSERT_TRUE(legacy.Save());
  }
  const auto legacy = MarkPrism();
  ASSERT_TRUE(CompileSchema(schema_));
  EXPECT_NE(legacy, fs::last_write_time(prism_));
  EXPECT_TRUE(HasKey("zong"));
  const auto migrated = MarkPrism();
  ASSERT_TRUE(CompileSchema(schema_));
  EXPECT_EQ(migrated, fs::last_write_time(prism_));
}

TEST_F(RimeDictCompilerTest, RejectsInvalidSchemaWithExistingPrism) {
  const auto old = MarkPrism();
  EXPECT_FALSE(CompileSchema("speller: [invalid yaml\n"));
  EXPECT_EQ(old, fs::last_write_time(prism_));
}

}  // namespace
