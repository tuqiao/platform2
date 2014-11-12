// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UPDATE_ENGINE_PREFS_H_
#define UPDATE_ENGINE_PREFS_H_

#include <string>

#include <base/files/file_path.h>
#include "gtest/gtest_prod.h"  // for FRIEND_TEST
#include "update_engine/prefs_interface.h"

namespace chromeos_update_engine {

// Implements a preference store by storing the value associated with
// a key in a separate file named after the key under a preference
// store directory.

class Prefs : public PrefsInterface {
 public:
  Prefs() {}

  // Initializes the store by associating this object with |prefs_dir|
  // as the preference store directory. Returns true on success, false
  // otherwise.
  bool Init(const base::FilePath& prefs_dir);

  // PrefsInterface methods.
  bool GetString(const std::string& key, std::string* value) override;
  bool SetString(const std::string& key, const std::string& value) override;
  bool GetInt64(const std::string& key, int64_t* value) override;
  bool SetInt64(const std::string& key, const int64_t value) override;
  bool GetBoolean(const std::string& key, bool* value) override;
  bool SetBoolean(const std::string& key, const bool value) override;

  bool Exists(const std::string& key) override;
  bool Delete(const std::string& key) override;

 private:
  FRIEND_TEST(PrefsTest, GetFileNameForKey);
  FRIEND_TEST(PrefsTest, GetFileNameForKeyBadCharacter);
  FRIEND_TEST(PrefsTest, GetFileNameForKeyEmpty);

  // Sets |filename| to the full path to the file containing the data
  // associated with |key|. Returns true on success, false otherwise.
  bool GetFileNameForKey(const std::string& key, base::FilePath* filename);

  // Preference store directory.
  base::FilePath prefs_dir_;

  DISALLOW_COPY_AND_ASSIGN(Prefs);
};

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_PREFS_H_
