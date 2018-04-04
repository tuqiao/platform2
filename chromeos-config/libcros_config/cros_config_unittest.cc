// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/*
 * Tests for the CrosConfig library, which provides access to the Chrome OS
 * master configuration.
 */

#include <stdlib.h>

#include <base/files/file_path.h>
#include <base/logging.h>
#include <chromeos-config/libcros_config/cros_config.h>
#include <gtest/gtest.h>

#ifndef USE_JSON
#define TEST_FILE "test.dtb"
#else
#define TEST_FILE "test.json"
#endif

class CrosConfigTest : public testing::Test {
 protected:
  void InitConfig(const std::string name = "Another",
                  int sku_id = -1,
                  std::string whitelabel_name = "") {
    base::FilePath filepath(TEST_FILE);
    ASSERT_TRUE(
        cros_config_.InitForTest(filepath, name, sku_id, whitelabel_name));
  }

  brillo::CrosConfig cros_config_;
};

TEST_F(CrosConfigTest, CheckMissingFile) {
  base::FilePath filepath("invalid-file");
  ASSERT_FALSE(cros_config_.InitForTest(filepath, "Another", -1, ""));
}

TEST_F(CrosConfigTest, CheckUnknownModel) {
  base::FilePath filepath(TEST_FILE);
  ASSERT_FALSE(cros_config_.InitForTest(filepath, "no-model", -1, ""));
}

TEST_F(CrosConfigTest, Check111NoInit) {
  std::string val;
  ASSERT_FALSE(cros_config_.GetString("/", "wallpaper", &val));
}

TEST_F(CrosConfigTest, CheckWrongPath) {
  InitConfig();
  std::string val;
  ASSERT_FALSE(cros_config_.GetString("/wibble", "wallpaper", &val));
}

TEST_F(CrosConfigTest, CheckBadString) {
  InitConfig();
  std::string val;
  ASSERT_FALSE(cros_config_.GetString("/", "string-list", &val));
}

TEST_F(CrosConfigTest, CheckGoodStringRoot) {
  InitConfig();
  std::string val;
  ASSERT_TRUE(cros_config_.GetString("/", "wallpaper", &val));
  ASSERT_EQ("default", val);
}

TEST_F(CrosConfigTest, CheckGoodStringNonRoot) {
  InitConfig();
  std::string val;
  ASSERT_TRUE(cros_config_.GetString("/touch", "present", &val));
  ASSERT_EQ("probe", val);
}

TEST_F(CrosConfigTest, CheckEmptyPathError) {
  InitConfig();
  std::string val;
  ASSERT_FALSE(cros_config_.GetString("", "wallpaper", &val));
  ASSERT_EQ("", val);
}

TEST_F(CrosConfigTest, CheckPathWithoutSlashError) {
  InitConfig();
  std::string val;
  ASSERT_FALSE(cros_config_.GetString("noslash", "wallpaper", &val));
  ASSERT_EQ("", val);
}

TEST_F(CrosConfigTest, CheckAbsPath) {
  InitConfig("Another");
  std::string val;

  ASSERT_TRUE(cros_config_.GetAbsPath("/audio/main", "cras-config-dir", &val));
  ASSERT_EQ("/etc/cras/another", val);
}


#ifndef USE_JSON
TEST_F(CrosConfigTest, CheckBadFile) {
  base::FilePath filepath("test.dts");
  ASSERT_FALSE(cros_config_.InitForTest(filepath, "Another", -1, ""));
}

TEST_F(CrosConfigTest, CheckBadStruct) {
  base::FilePath filepath("test_bad_struct.dtb");
  ASSERT_FALSE(cros_config_.InitForTest(filepath, "not_another", -1, ""));
}

TEST_F(CrosConfigTest, CheckSubmodel) {
  InitConfig("Some", 0, "");
  std::string val;
  ASSERT_TRUE(cros_config_.GetString("/touch", "present", &val));
  ASSERT_EQ("yes", val);

  InitConfig("Some", 1, "");
  ASSERT_TRUE(cros_config_.GetString("/touch", "present", &val));
  ASSERT_EQ("no", val);

  std::vector<std::string> log_msgs;
  ASSERT_FALSE(cros_config_.GetString("/touch", "presents", &val, &log_msgs));
  ASSERT_EQ(2, log_msgs.size());
  ASSERT_EQ(
      "Cannot get path /touch property presents: full path "
      "/chromeos/models/some/touch: FDT_ERR_NOTFOUND",
      log_msgs[0]);
  ASSERT_EQ(
      "Cannot get path /touch property presents: full path "
      "/chromeos/models/some/submodels/notouch/touch: FDT_ERR_NOTFOUND",
      log_msgs[1]);
}

TEST_F(CrosConfigTest, CheckFollowPhandle) {
  InitConfig("Another");
  std::string val;
  ASSERT_TRUE(cros_config_.GetString("/audio/main", "card", &val));
  ASSERT_EQ("a-card", val);
}

TEST_F(CrosConfigTest, CheckWhiteLabel) {
  // Check values defined by whitelabel1.
  InitConfig("Some", 8, "whitelabel1");
  std::string val;
  ASSERT_TRUE(cros_config_.GetString("/", "wallpaper", &val));
  ASSERT_EQ("wallpaper-wl1", val);
  ASSERT_TRUE(cros_config_.GetString("/firmware", "key-id", &val));
  ASSERT_EQ("WHITELABEL1", val);
  ASSERT_TRUE(cros_config_.GetString("/", "brand-code", &val));
  ASSERT_EQ("WLBA", val);

  // Check values defined by whitelabel2.
  InitConfig("Some", 9, "whitelabel2");
  ASSERT_TRUE(cros_config_.GetString("/", "wallpaper", &val));
  ASSERT_EQ("wallpaper-wl2", val);
  ASSERT_TRUE(cros_config_.GetString("/firmware", "key-id", &val));
  ASSERT_EQ("WHITELABEL2", val);
  ASSERT_TRUE(cros_config_.GetString("/", "brand-code", &val));
  ASSERT_EQ("WLBB", val);
}
#endif /* !USE_JSON */

int main(int argc, char** argv) {
  int status = system("exec ./chromeos-config-test-setup.sh");
  if (status != 0)
    return EXIT_FAILURE;

  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_FILE;
  settings.log_file = "log.test";
  settings.lock_log = logging::DONT_LOCK_LOG_FILE;
  settings.delete_old = logging::DELETE_OLD_LOG_FILE;
  logging::InitLogging(settings);
  logging::SetMinLogLevel(-3);

  testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
