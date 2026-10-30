// Copyright RIME Developers
// Distributed under the BSD License

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <boost/scope_exit.hpp>
#include <rime/deployer.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_db_recovery_task.h>
#include <rime/dict/user_dictionary.h>
#include <rime/service.h>

using namespace rime;

class RimeUserDbRecoveryTest : public testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directory(root_);
  }

  void TearDown() override {
    Service::instance().deployer().JoinWorkThread();
    std::filesystem::remove_all(root_);
  }

  an<LevelDb> CreateDb() {
    return New<UserDbWrapper<LevelDb>>(root_ / "test.userdb", "test");
  }

  void CorruptCurrent() {
    std::ofstream(root_ / "test.userdb" / "CURRENT") << "missing-newline";
  }

  const path root_{"user_db_recovery_test"};
};

TEST_F(RimeUserDbRecoveryTest, LockedDictionaryDoesNotScheduleRecovery) {
  auto owner = CreateDb();
  ASSERT_TRUE(owner->Open());
  ASSERT_TRUE(owner->Update("shi xin \t拾新", "c=6 d=6 t=1"));
  auto contender = CreateDb();
  UserDictionary dictionary("test", contender);
  EXPECT_FALSE(dictionary.Load());
  // Load must not even disable the dictionary or queue a recovery worker.
  EXPECT_FALSE(contender->disabled());
  auto& deployer = Service::instance().deployer();
  deployer.JoinWorkThread();
  EXPECT_FALSE(std::filesystem::exists(owner->file_path() / "lost"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "test.userdb.old"));
  ASSERT_TRUE(owner->Update("shi yan \t试验", "c=7 d=7 t=2"));
  ASSERT_TRUE(owner->Close());
  ASSERT_TRUE(contender->Open());
  string value;
  EXPECT_TRUE(contender->Fetch("shi xin \t拾新", &value));
  EXPECT_EQ("c=6 d=6 t=1", value);
  EXPECT_TRUE(contender->Fetch("shi yan \t试验", &value));
  EXPECT_EQ("c=7 d=7 t=2", value);
}

TEST_F(RimeUserDbRecoveryTest, IoErrorDoesNotScheduleRecovery) {
  // A regular file in place of the parent directory forces an I/O error
  // without depending on permissions (the suite can run as root).
  std::ofstream(root_ / "not-a-directory") << "keep";
  auto db = New<LevelDb>(root_ / "not-a-directory" / "test.userdb", "test");
  UserDictionary dictionary("test", db);
  EXPECT_FALSE(dictionary.Load());
  EXPECT_FALSE(db->IsRecoveryNeeded());
  EXPECT_FALSE(db->disabled());
  Service::instance().deployer().JoinWorkThread();
  EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "not-a-directory"));
}

TEST_F(RimeUserDbRecoveryTest, RepairsConfirmedCorruptionAndReleasesLock) {
  auto db = CreateDb();
  ASSERT_TRUE(db->Open());
  ASSERT_TRUE(db->Update("shi xin \t拾新", "c=6 d=6 t=1"));
  ASSERT_TRUE(db->Close());
  CorruptCurrent();
  ASSERT_FALSE(db->Open());
  ASSERT_TRUE(db->IsRecoveryNeeded());
  UserDbRecoveryTask task(db);
  ASSERT_TRUE(task.Run(&Service::instance().deployer()));
  EXPECT_FALSE(db->disabled());
  ASSERT_TRUE(db->Open());
  EXPECT_FALSE(db->IsRecoveryNeeded());
  string value;
  EXPECT_TRUE(db->Fetch("shi xin \t拾新", &value));
  EXPECT_EQ("c=6 d=6 t=1", value);
}

TEST_F(RimeUserDbRecoveryTest, CorruptionSchedulesAutomaticRecovery) {
  auto db = CreateDb();
  ASSERT_TRUE(db->Open());
  ASSERT_TRUE(db->Update("shi xin \t拾新", "c=6 d=6 t=1"));
  ASSERT_TRUE(db->Close());
  CorruptCurrent();
  UserDictionary dictionary("test", db);
  EXPECT_FALSE(dictionary.Load());
  Service::instance().deployer().JoinWorkThread();
  ASSERT_TRUE(dictionary.Load());
  string value;
  EXPECT_TRUE(db->Fetch("shi xin \t拾新", &value));
  EXPECT_EQ("c=6 d=6 t=1", value);
}

TEST_F(RimeUserDbRecoveryTest, QueuedRecoveryCannotTouchNewOwner) {
  auto owner = CreateDb();
  ASSERT_TRUE(owner->Open());
  ASSERT_TRUE(owner->Update("shi xin \t拾新", "c=6 d=6 t=1"));
  ASSERT_TRUE(owner->Close());
  const path current = owner->file_path() / "CURRENT";
  string manifest;
  std::getline(std::ifstream(current), manifest);
  CorruptCurrent();
  auto contender = CreateDb();
  ASSERT_FALSE(contender->Open());
  ASSERT_TRUE(contender->IsRecoveryNeeded());
  // Another process repairs/reopens the dictionary before the queued task.
  std::ofstream(current) << manifest << '\n';
  ASSERT_TRUE(owner->Open());
  UserDbRecoveryTask task(contender);
  EXPECT_FALSE(task.Run(&Service::instance().deployer()));
  EXPECT_FALSE(contender->disabled());
  EXPECT_FALSE(std::filesystem::exists(owner->file_path() / "lost"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "test.userdb.old"));
  ASSERT_TRUE(owner->Update("shi yan \t试验", "c=7 d=7 t=2"));
  ASSERT_TRUE(owner->Close());
  ASSERT_TRUE(contender->Open());
  string value;
  EXPECT_TRUE(contender->Fetch("shi xin \t拾新", &value));
  EXPECT_TRUE(contender->Fetch("shi yan \t试验", &value));
}

TEST_F(RimeUserDbRecoveryTest, RecoveryGuardBlocksOpenUntilReleased) {
  auto db = CreateDb();
  ASSERT_TRUE(db->Open());
  ASSERT_TRUE(db->Close());
  string manifest;
  std::getline(std::ifstream(db->file_path() / "CURRENT"), manifest);
  CorruptCurrent();
  ASSERT_FALSE(db->Open());
  auto guard = db->AcquireRecoveryLock();
  ASSERT_TRUE(bool(guard));
  std::ofstream(db->file_path() / "CURRENT") << manifest << '\n';
  auto other = CreateDb();
  EXPECT_FALSE(other->Open());
  EXPECT_FALSE(other->IsRecoveryNeeded());
  guard.reset();
  EXPECT_TRUE(other->Open());
}

// Exercise the existing snapshot fallback deterministically: actual RepairDB
// often succeeds even when it has no records left to salvage.
class FailedRepairDb : public UserDbWrapper<LevelDb> {
 public:
  using UserDbWrapper<LevelDb>::UserDbWrapper;

  bool Recover() override {
    LevelDb contender(file_path(), name());
    EXPECT_FALSE(contender.Open());  // repair still holds the lock
    return false;
  }
};

TEST_F(RimeUserDbRecoveryTest, FailedRepairStillRecreatesFromSnapshot) {
  auto db = New<FailedRepairDb>(root_ / "test.userdb", "test");
  ASSERT_TRUE(db->Open());
  ASSERT_TRUE(db->Update("shi xin \t拾新", "c=6 d=6 t=1"));
  auto& deployer = Service::instance().deployer();
  // Redirect only the snapshot directory; never use a real user's files.
  const path previous_sync = deployer.sync_dir;
  BOOST_SCOPE_EXIT((&deployer) (&previous_sync)) {
    deployer.sync_dir = previous_sync;
  }
  BOOST_SCOPE_EXIT_END
  deployer.sync_dir = root_ / "sync";
  std::filesystem::create_directories(deployer.user_data_sync_dir());
  ASSERT_TRUE(db->Backup(deployer.user_data_sync_dir() / "test.userdb.txt"));
  ASSERT_TRUE(db->Close());
  CorruptCurrent();
  ASSERT_FALSE(db->Open());
  UserDbRecoveryTask task(db);
  const bool recreated = task.Run(&deployer);
  EXPECT_FALSE(db->disabled());
#ifdef _WIN32
  // Windows may reject renaming a directory containing the locked file.
  // Preserve the original and snapshot for manual recovery in that case.
  if (!recreated) {
    EXPECT_FALSE(db->loaded());
    EXPECT_TRUE(std::filesystem::exists(db->file_path() / "CURRENT"));
    EXPECT_FALSE(std::filesystem::exists(root_ / "test.userdb.old"));
    EXPECT_TRUE(std::filesystem::exists(deployer.user_data_sync_dir() /
                                        "test.userdb.txt"));
    return;
  }
#endif
  ASSERT_TRUE(recreated);
  ASSERT_TRUE(db->loaded());
  EXPECT_TRUE(std::filesystem::exists(root_ / "test.userdb.old"));
  string value;
  EXPECT_TRUE(db->Fetch("shi xin \t拾新", &value));
  EXPECT_EQ("c=6 d=6 t=1", value);
}

TEST_F(RimeUserDbRecoveryTest, FailedArchivePreservesBothDatabases) {
  auto db = New<FailedRepairDb>(root_ / "test.userdb", "test");
  ASSERT_TRUE(db->Open());
  ASSERT_TRUE(db->Close());
  CorruptCurrent();
  ASSERT_FALSE(db->Open());
  const path archive = root_ / "test.userdb.old";
  std::filesystem::create_directory(archive);
  std::ofstream(archive / "keep") << "older backup";
  UserDbRecoveryTask task(db);
  EXPECT_FALSE(task.Run(&Service::instance().deployer()));
  EXPECT_FALSE(db->disabled());
  EXPECT_TRUE(std::filesystem::exists(db->file_path() / "CURRENT"));
  EXPECT_TRUE(std::filesystem::exists(archive / "keep"));
  // The failure must release the recovery lock as well.
  auto guard = db->AcquireRecoveryLock();
  EXPECT_TRUE(bool(guard));
}
