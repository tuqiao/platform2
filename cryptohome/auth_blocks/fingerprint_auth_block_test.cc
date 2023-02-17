// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_blocks/fingerprint_auth_block.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/test/task_environment.h>
#include <base/test/test_future.h>
#include <brillo/secure_blob.h>
#include <cryptohome/proto_bindings/UserDataAuth.pb.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libhwsec-foundation/error/testing_helper.h>

#include "cryptohome/auth_blocks/auth_block.h"
#include "cryptohome/auth_blocks/mock_biometrics_command_processor.h"
#include "cryptohome/error/cryptohome_crypto_error.h"
#include "cryptohome/error/cryptohome_le_cred_error.h"
#include "cryptohome/error/utilities.h"
#include "cryptohome/flatbuffer_schemas/auth_block_state.h"
#include "cryptohome/le_credential_manager.h"
#include "cryptohome/mock_le_credential_manager.h"

namespace cryptohome {
namespace {

using base::test::TestFuture;
using cryptohome::error::ContainsActionInStack;
using cryptohome::error::CryptohomeError;
using cryptohome::error::CryptohomeLECredError;
using cryptohome::error::ErrorAction;
using cryptohome::error::ErrorActionSet;
using hwsec_foundation::error::testing::IsOk;
using hwsec_foundation::error::testing::ReturnError;
using hwsec_foundation::error::testing::ReturnOk;
using hwsec_foundation::error::testing::ReturnValue;
using hwsec_foundation::status::MakeStatus;
using user_data_auth::AuthEnrollmentProgress;
using user_data_auth::AuthScanDone;

using testing::_;
using testing::AllOf;
using testing::DoAll;
using testing::Field;
using testing::SaveArg;
using testing::SetArgPointee;
using testing::SizeIs;
using testing::StrictMock;

using OperationInput = BiometricsAuthBlockService::OperationInput;
using OperationOutput = BiometricsAuthBlockService::OperationOutput;

using CreateTestFuture = TestFuture<CryptohomeStatus,
                                    std::unique_ptr<KeyBlobs>,
                                    std::unique_ptr<AuthBlockState>>;

constexpr uint8_t kFingerprintAuthChannel = 0;

constexpr uint64_t kFakeRateLimiterLabel = 100;
constexpr uint64_t kFakeCredLabel = 200;

constexpr char kFakeRecordId[] = "fake_record_id";

// TODO(b/247704971): Blob should be used for fields that doesn't contain secret
// values. Before the LE manager interface changes accordingly, transform the
// blob types explicitly.
brillo::SecureBlob BlobToSecureBlob(const brillo::Blob& blob) {
  return brillo::SecureBlob(blob.begin(), blob.end());
}

class FingerprintAuthBlockTest : public ::testing::Test {
 public:
  void SetUp() override {
    auto mock_processor =
        std::make_unique<StrictMock<MockBiometricsCommandProcessor>>();
    mock_processor_ = mock_processor.get();
    EXPECT_CALL(*mock_processor_, SetEnrollScanDoneCallback(_))
        .WillOnce(SaveArg<0>(&enroll_callback_));
    EXPECT_CALL(*mock_processor_, SetAuthScanDoneCallback(_))
        .WillOnce(SaveArg<0>(&auth_callback_));
    bio_service_ = std::make_unique<BiometricsAuthBlockService>(
        std::move(mock_processor), /*enroll_signal_sender=*/base::DoNothing(),
        /*auth_signal_sender=*/base::DoNothing());
    auth_block_ = std::make_unique<FingerprintAuthBlock>(&mock_le_manager_,
                                                         bio_service_.get());
  }

 protected:
  const error::CryptohomeError::ErrorLocationPair kErrorLocationPlaceholder =
      error::CryptohomeError::ErrorLocationPair(
          static_cast<::cryptohome::error::CryptohomeError::ErrorLocation>(1),
          "Testing1");
  const ObfuscatedUsername kFakeAccountId{"account_id"};
  const brillo::Blob kFakeAuthNonce{32, 100};

  // We can just emit empty proto message here as this test only relies on the
  // nonce.
  void EmitEnrollEvent(std::optional<brillo::Blob> nonce) {
    enroll_callback_.Run(AuthEnrollmentProgress{}, std::move(nonce));
  }

  void EmitAuthEvent(brillo::Blob nonce) {
    auth_callback_.Run(AuthScanDone{}, std::move(nonce));
  }

  void StartEnrollSession(std::unique_ptr<PreparedAuthFactorToken>& ret_token) {
    EXPECT_CALL(*mock_processor_, StartEnrollSession(_))
        .WillOnce([](auto&& callback) { std::move(callback).Run(true); });

    TestFuture<CryptohomeStatusOr<std::unique_ptr<PreparedAuthFactorToken>>>
        result;
    bio_service_->StartEnrollSession(AuthFactorType::kFingerprint,
                                     kFakeAccountId, result.GetCallback());
    ASSERT_TRUE(result.IsReady());
    CryptohomeStatusOr<std::unique_ptr<PreparedAuthFactorToken>> token =
        result.Take();
    ASSERT_TRUE(token.ok());
    EmitEnrollEvent(kFakeAuthNonce);
    ret_token = *std::move(token);

    EXPECT_CALL(*mock_processor_, EndEnrollSession);
  }

  base::test::TaskEnvironment task_environment_;

  StrictMock<MockLECredentialManager> mock_le_manager_;
  StrictMock<MockBiometricsCommandProcessor>* mock_processor_;
  base::RepeatingCallback<void(AuthEnrollmentProgress,
                               std::optional<brillo::Blob>)>
      enroll_callback_;
  base::RepeatingCallback<void(AuthScanDone, brillo::Blob)> auth_callback_;
  std::unique_ptr<BiometricsAuthBlockService> bio_service_;
  std::unique_ptr<FingerprintAuthBlock> auth_block_;
};

TEST_F(FingerprintAuthBlockTest, CreateSuccess) {
  const brillo::SecureBlob kFakeResetSecret(32, 1);
  const brillo::SecureBlob kFakeAuthSecret(32, 3), kFakeAuthPin(32, 4);
  const brillo::Blob kFakeGscNonce(32, 1), kFakeLabelSeed(32, 2);
  const brillo::Blob kFakeGscIv(16, 1);
  const AuthInput kFakeAuthInput{.obfuscated_username = kFakeAccountId,
                                 .reset_secret = kFakeResetSecret,
                                 .rate_limiter_label = kFakeRateLimiterLabel};

  std::unique_ptr<PreparedAuthFactorToken> token;
  StartEnrollSession(token);
  ASSERT_NE(token, nullptr);

  EXPECT_CALL(
      mock_le_manager_,
      StartBiometricsAuth(kFingerprintAuthChannel, kFakeRateLimiterLabel,
                          BlobToSecureBlob(kFakeAuthNonce)))
      .WillOnce(ReturnValue(LECredentialManager::StartBiometricsAuthReply{
          .server_nonce = BlobToSecureBlob(kFakeGscNonce),
          .iv = BlobToSecureBlob(kFakeGscIv),
          .encrypted_he_secret = BlobToSecureBlob(kFakeLabelSeed)}));
  EXPECT_CALL(
      *mock_processor_,
      CreateCredential(
          kFakeAccountId,
          AllOf(Field(&OperationInput::nonce, kFakeGscNonce),
                Field(&OperationInput::encrypted_label_seed, kFakeLabelSeed),
                Field(&OperationInput::iv, kFakeGscIv)),
          _))
      .WillOnce([&](auto&&, auto&&, auto&& callback) {
        std::move(callback).Run(OperationOutput{
            .record_id = kFakeRecordId,
            .auth_secret = kFakeAuthSecret,
            .auth_pin = kFakeAuthPin,
        });
      });
  EXPECT_CALL(mock_le_manager_,
              InsertCredential(_, kFakeAuthPin, _, kFakeResetSecret, _, _, _))
      .WillOnce(DoAll(SetArgPointee<6>(kFakeCredLabel),
                      ReturnOk<CryptohomeLECredError>()));

  CreateTestFuture result;
  auth_block_->Create(kFakeAuthInput, result.GetCallback());

  ASSERT_TRUE(result.IsReady());
  auto [status, key_blobs, auth_state] = result.Take();
  EXPECT_THAT(status, IsOk());
  ASSERT_NE(key_blobs, nullptr);
  ASSERT_TRUE(key_blobs->vkk_key.has_value());
  EXPECT_THAT(key_blobs->vkk_key.value(), SizeIs(32));
  ASSERT_TRUE(key_blobs->reset_secret.has_value());
  EXPECT_EQ(key_blobs->reset_secret.value(), kFakeResetSecret);
  EXPECT_FALSE(key_blobs->rate_limiter_label.has_value());
  ASSERT_NE(auth_state, nullptr);
  ASSERT_TRUE(
      std::holds_alternative<FingerprintAuthBlockState>(auth_state->state));
  auto& state = std::get<FingerprintAuthBlockState>(auth_state->state);
  const std::string record_id = kFakeRecordId;
  EXPECT_EQ(state.template_id, record_id);
  EXPECT_EQ(state.gsc_secret_label, kFakeCredLabel);
}

TEST_F(FingerprintAuthBlockTest, CreateNoUsername) {
  CreateTestFuture result;
  auth_block_->Create(AuthInput{}, result.GetCallback());

  ASSERT_TRUE(result.IsReady());
  auto [status, key_blobs, auth_state] = result.Take();
  EXPECT_TRUE(
      ContainsActionInStack(status, ErrorAction::kDevCheckUnexpectedState));
  EXPECT_EQ(key_blobs, nullptr);
  EXPECT_EQ(auth_state, nullptr);
}

TEST_F(FingerprintAuthBlockTest, CreateNoSession) {
  const brillo::SecureBlob kFakeResetSecret(32, 1);
  const AuthInput kFakeAuthInput{.obfuscated_username = kFakeAccountId,
                                 .reset_secret = kFakeResetSecret,
                                 .rate_limiter_label = kFakeRateLimiterLabel};

  CreateTestFuture result;
  auth_block_->Create(kFakeAuthInput, result.GetCallback());

  ASSERT_TRUE(result.IsReady());
  auto [status, key_blobs, auth_state] = result.Take();
  EXPECT_TRUE(
      ContainsActionInStack(status, ErrorAction::kDevCheckUnexpectedState));
  EXPECT_EQ(key_blobs, nullptr);
  EXPECT_EQ(auth_state, nullptr);
}

TEST_F(FingerprintAuthBlockTest, CreateStartBioAuthFailed) {
  const brillo::SecureBlob kFakeResetSecret(32, 1);
  const AuthInput kFakeAuthInput{.obfuscated_username = kFakeAccountId,
                                 .reset_secret = kFakeResetSecret,
                                 .rate_limiter_label = kFakeRateLimiterLabel};

  std::unique_ptr<PreparedAuthFactorToken> token;
  StartEnrollSession(token);
  ASSERT_NE(token, nullptr);

  EXPECT_CALL(mock_le_manager_, StartBiometricsAuth)
      .WillOnce([this](auto&&, auto&&, auto&&) {
        return MakeStatus<CryptohomeLECredError>(
            kErrorLocationPlaceholder,
            ErrorActionSet({ErrorAction::kLeLockedOut}),
            LECredError::LE_CRED_ERROR_TOO_MANY_ATTEMPTS);
      });

  CreateTestFuture result;
  auth_block_->Create(kFakeAuthInput, result.GetCallback());

  ASSERT_TRUE(result.IsReady());
  auto [status, key_blobs, auth_state] = result.Take();
  EXPECT_TRUE(ContainsActionInStack(status, ErrorAction::kLeLockedOut));
  EXPECT_EQ(key_blobs, nullptr);
  EXPECT_EQ(auth_state, nullptr);
}

TEST_F(FingerprintAuthBlockTest, CreateCreateCredentialFailed) {
  const brillo::SecureBlob kFakeResetSecret(32, 1);
  const brillo::Blob kFakeGscNonce(32, 1), kFakeLabelSeed(32, 2);
  const brillo::Blob kFakeGscIv(16, 1);
  const AuthInput kFakeAuthInput{.obfuscated_username = kFakeAccountId,
                                 .reset_secret = kFakeResetSecret,
                                 .rate_limiter_label = kFakeRateLimiterLabel};

  std::unique_ptr<PreparedAuthFactorToken> token;
  StartEnrollSession(token);
  ASSERT_NE(token, nullptr);

  EXPECT_CALL(
      mock_le_manager_,
      StartBiometricsAuth(kFingerprintAuthChannel, kFakeRateLimiterLabel,
                          BlobToSecureBlob(kFakeAuthNonce)))
      .WillOnce(ReturnValue(LECredentialManager::StartBiometricsAuthReply{
          .server_nonce = BlobToSecureBlob(kFakeGscNonce),
          .iv = BlobToSecureBlob(kFakeGscIv),
          .encrypted_he_secret = BlobToSecureBlob(kFakeLabelSeed)}));
  EXPECT_CALL(*mock_processor_, CreateCredential(_, _, _))
      .WillOnce([&](auto&&, auto&&, auto&& callback) {
        std::move(callback).Run(MakeStatus<CryptohomeError>(
            kErrorLocationPlaceholder,
            ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
            user_data_auth::CRYPTOHOME_ERROR_NOT_IMPLEMENTED));
      });

  CreateTestFuture result;
  auth_block_->Create(kFakeAuthInput, result.GetCallback());

  ASSERT_TRUE(result.IsReady());
  auto [status, key_blobs, auth_state] = result.Take();
  EXPECT_TRUE(
      ContainsActionInStack(status, ErrorAction::kDevCheckUnexpectedState));
  EXPECT_EQ(key_blobs, nullptr);
  EXPECT_EQ(auth_state, nullptr);
}

TEST_F(FingerprintAuthBlockTest, CreateInsertCredentialFailed) {
  const brillo::SecureBlob kFakeResetSecret(32, 1);
  const brillo::SecureBlob kFakeAuthSecret(32, 3), kFakeAuthPin(32, 4);
  const brillo::Blob kFakeGscNonce(32, 1), kFakeLabelSeed(32, 2);
  const brillo::Blob kFakeGscIv(16, 1);
  const AuthInput kFakeAuthInput{.obfuscated_username = kFakeAccountId,
                                 .reset_secret = kFakeResetSecret,
                                 .rate_limiter_label = kFakeRateLimiterLabel};

  std::unique_ptr<PreparedAuthFactorToken> token;
  StartEnrollSession(token);
  ASSERT_NE(token, nullptr);

  EXPECT_CALL(
      mock_le_manager_,
      StartBiometricsAuth(kFingerprintAuthChannel, kFakeRateLimiterLabel,
                          BlobToSecureBlob(kFakeAuthNonce)))
      .WillOnce(ReturnValue(LECredentialManager::StartBiometricsAuthReply{
          .server_nonce = BlobToSecureBlob(kFakeGscNonce),
          .iv = BlobToSecureBlob(kFakeGscIv),
          .encrypted_he_secret = BlobToSecureBlob(kFakeLabelSeed)}));
  EXPECT_CALL(
      *mock_processor_,
      CreateCredential(
          kFakeAccountId,
          AllOf(Field(&OperationInput::nonce, kFakeGscNonce),
                Field(&OperationInput::encrypted_label_seed, kFakeLabelSeed),
                Field(&OperationInput::iv, kFakeGscIv)),
          _))
      .WillOnce([&](auto&&, auto&&, auto&& callback) {
        std::move(callback).Run(OperationOutput{
            .record_id = kFakeRecordId,
            .auth_secret = kFakeAuthSecret,
            .auth_pin = kFakeAuthPin,
        });
      });
  EXPECT_CALL(mock_le_manager_, InsertCredential)
      .WillOnce([this](auto&&, auto&&, auto&&, auto&&, auto&&, auto&&, auto&&) {
        return MakeStatus<CryptohomeLECredError>(
            kErrorLocationPlaceholder,
            ErrorActionSet({ErrorAction::kDevCheckUnexpectedState}),
            LECredError::LE_CRED_ERROR_HASH_TREE);
      });

  CreateTestFuture result;
  auth_block_->Create(kFakeAuthInput, result.GetCallback());

  ASSERT_TRUE(result.IsReady());
  auto [status, key_blobs, auth_state] = result.Take();
  EXPECT_TRUE(
      ContainsActionInStack(status, ErrorAction::kDevCheckUnexpectedState));
  EXPECT_EQ(key_blobs, nullptr);
  EXPECT_EQ(auth_state, nullptr);
}

}  // namespace
}  // namespace cryptohome
