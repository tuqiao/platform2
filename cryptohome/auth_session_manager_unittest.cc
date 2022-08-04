// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_session_manager.h"

#include <memory>
#include <string>
#include <utility>

#include <base/test/task_environment.h>
#include <base/unguessable_token.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libhwsec/frontend/cryptohome/mock_frontend.h>
#include <libhwsec/frontend/pinweaver/mock_frontend.h>

#include "cryptohome/auth_blocks/mock_auth_block_utility.h"
#include "cryptohome/auth_factor/auth_factor_manager.h"
#include "cryptohome/mock_cryptohome_keys_manager.h"
#include "cryptohome/mock_keyset_management.h"
#include "cryptohome/mock_platform.h"
#include "cryptohome/mock_tpm.h"
#include "cryptohome/user_secret_stash_storage.h"

using base::test::TaskEnvironment;
using ::testing::_;
using ::testing::ByMove;
using ::testing::Eq;
using ::testing::IsNull;
using testing::NiceMock;
using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Return;

namespace cryptohome {

class AuthSessionManagerTest : public ::testing::Test {
 public:
  AuthSessionManagerTest() = default;
  ~AuthSessionManagerTest() override = default;
  AuthSessionManagerTest(const AuthSessionManagerTest&) = delete;
  AuthSessionManagerTest& operator=(AuthSessionManagerTest&) = delete;

 protected:
  NiceMock<hwsec::MockCryptohomeFrontend> hwsec_;
  NiceMock<hwsec::MockPinWeaverFrontend> pinweaver_;
  NiceMock<MockPlatform> platform_;
  NiceMock<MockCryptohomeKeysManager> cryptohome_keys_manager_;
  AuthFactorManager auth_factor_manager_{&platform_};
  UserSecretStashStorage user_secret_stash_storage_{&platform_};
  Crypto crypto_{&hwsec_, &pinweaver_, &cryptohome_keys_manager_, nullptr};
};

TEST_F(AuthSessionManagerTest, CreateFindRemove) {
  TaskEnvironment task_environment(
      TaskEnvironment::ThreadPoolExecutionMode::QUEUED);

  NiceMock<MockKeysetManagement> keyset_management;
  NiceMock<MockPlatform> platform;
  AuthFactorManager auth_factor_manager(&platform);
  UserSecretStashStorage user_secret_stash_storage(&platform);
  NiceMock<MockAuthBlockUtility> auth_block_utility;
  AuthSessionManager auth_session_manager(
      &crypto_, &platform_, &keyset_management, &auth_block_utility,
      &auth_factor_manager, &user_secret_stash_storage);

  AuthSession* auth_session =
      auth_session_manager.CreateAuthSession("foo@example.com", 0);
  ASSERT_THAT(auth_session, NotNull());
  base::UnguessableToken token = auth_session->token();
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), Eq(auth_session));
  EXPECT_TRUE(auth_session_manager.RemoveAuthSession(token));
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), IsNull());

  // Repeat with serialized_token overload.
  auth_session = auth_session_manager.CreateAuthSession("foo@example.com", 0);
  ASSERT_THAT(auth_session, NotNull());
  std::string serialized_token = auth_session->serialized_token();
  ASSERT_THAT(auth_session_manager.FindAuthSession(serialized_token),
              Eq(auth_session));
  EXPECT_TRUE(auth_session_manager.RemoveAuthSession(serialized_token));
  ASSERT_THAT(auth_session_manager.FindAuthSession(serialized_token), IsNull());
}

TEST_F(AuthSessionManagerTest, CreateExpire) {
  TaskEnvironment task_environment(
      TaskEnvironment::TimeSource::MOCK_TIME,
      TaskEnvironment::ThreadPoolExecutionMode::QUEUED);

  NiceMock<MockKeysetManagement> keyset_management;
  NiceMock<MockPlatform> platform;
  AuthFactorManager auth_factor_manager(&platform);
  UserSecretStashStorage user_secret_stash_storage(&platform);
  NiceMock<MockAuthBlockUtility> auth_block_utility;
  AuthSessionManager auth_session_manager(
      &crypto_, &platform_, &keyset_management, &auth_block_utility,
      &auth_factor_manager, &user_secret_stash_storage);

  AuthSession* auth_session =
      auth_session_manager.CreateAuthSession("foo@example.com", 0);
  ASSERT_THAT(auth_session, NotNull());
  base::UnguessableToken token = auth_session->token();
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), Eq(auth_session));
  auth_session->SetAuthSessionAsAuthenticated();
  task_environment.FastForwardUntilNoTasksRemain();
  ASSERT_THAT(auth_session_manager.FindAuthSession(token), IsNull());
}

TEST_F(AuthSessionManagerTest, RemoveNonExisting) {
  // Setup:
  NiceMock<MockKeysetManagement> keyset_management;
  NiceMock<MockPlatform> platform;
  AuthFactorManager auth_factor_manager(&platform);
  UserSecretStashStorage user_secret_stash_storage(&platform);
  NiceMock<MockAuthBlockUtility> auth_block_utility;
  AuthSessionManager auth_session_manager(
      &crypto_, &platform_, &keyset_management, &auth_block_utility,
      &auth_factor_manager, &user_secret_stash_storage);

  // Test:
  EXPECT_FALSE(
      auth_session_manager.RemoveAuthSession(base::UnguessableToken()));
  EXPECT_FALSE(auth_session_manager.RemoveAuthSession("non-existing-token"));
}

}  // namespace cryptohome
