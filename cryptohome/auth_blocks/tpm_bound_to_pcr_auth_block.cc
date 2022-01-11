// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_blocks/tpm_bound_to_pcr_auth_block.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/types/variant.h>
#include <base/check.h>
#include <base/logging.h>
#include <brillo/secure_blob.h>

#include "cryptohome/auth_blocks/tpm_auth_block_utils.h"
#include "cryptohome/crypto.h"
#include "cryptohome/crypto/aes.h"
#include "cryptohome/crypto/scrypt.h"
#include "cryptohome/crypto/secure_blob_util.h"
#include "cryptohome/crypto_error.h"
#include "cryptohome/cryptohome_keys_manager.h"
#include "cryptohome/cryptohome_metrics.h"
#include "cryptohome/tpm.h"
#include "cryptohome/vault_keyset.pb.h"

using hwsec::StatusChain;
using hwsec::TPMError;
using hwsec::TPMErrorBase;
using hwsec::TPMRetryAction;
using hwsec_foundation::error::WrapError;

namespace cryptohome {

TpmBoundToPcrAuthBlock::TpmBoundToPcrAuthBlock(
    Tpm* tpm, CryptohomeKeysManager* cryptohome_keys_manager)
    : SyncAuthBlock(kTpmBackedPcrBound),
      tpm_(tpm),
      cryptohome_key_loader_(
          cryptohome_keys_manager->GetKeyLoader(CryptohomeKeyType::kRSA)),
      utils_(tpm, cryptohome_key_loader_) {
  CHECK(tpm != nullptr);
  CHECK(cryptohome_key_loader_ != nullptr);

  // Create the scrypt thread.
  // TODO(yich): Create another thread in userdataauth and passing it to here.
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  scrypt_thread_ = std::make_unique<base::Thread>("scrypt_thread");
  scrypt_thread_->StartWithOptions(std::move(options));
  scrypt_task_runner_ = scrypt_thread_->task_runner();
}

CryptoError TpmBoundToPcrAuthBlock::Create(const AuthInput& user_input,
                                           AuthBlockState* auth_block_state,
                                           KeyBlobs* key_blobs) {
  const brillo::SecureBlob& vault_key = user_input.user_input.value();
  brillo::SecureBlob salt =
      CreateSecureRandomBlob(CRYPTOHOME_DEFAULT_KEY_SALT_SIZE);

  const std::string& obfuscated_username =
      user_input.obfuscated_username.value();

  // If the cryptohome key isn't loaded, try to load it.
  if (!cryptohome_key_loader_->HasCryptohomeKey())
    cryptohome_key_loader_->Init();

  // If the key still isn't loaded, fail the operation.
  if (!cryptohome_key_loader_->HasCryptohomeKey())
    return CryptoError::CE_TPM_CRYPTO;

  const auto vkk_key = CreateSecureRandomBlob(kDefaultAesKeySize);
  brillo::SecureBlob pass_blob(kDefaultPassBlobSize);
  brillo::SecureBlob vkk_iv(kAesBlockSize);
  if (!DeriveSecretsScrypt(vault_key, salt, {&pass_blob, &vkk_iv}))
    return CryptoError::CE_OTHER_CRYPTO;

  std::map<uint32_t, brillo::Blob> default_pcr_map =
      tpm_->GetPcrMap(obfuscated_username, false /* use_extended_pcr */);
  std::map<uint32_t, brillo::Blob> extended_pcr_map =
      tpm_->GetPcrMap(obfuscated_username, true /* use_extended_pcr */);

  // Encrypt the VKK using the TPM and the user's passkey. The output is two
  // encrypted blobs, sealed to PCR in |tpm_key| and |extended_tpm_key|,
  // which are stored in the serialized vault keyset.
  TpmKeyHandle cryptohome_key = cryptohome_key_loader_->GetCryptohomeKey();
  brillo::SecureBlob auth_value;

  for (int i = 0; i < kTpmDecryptMaxRetries; ++i) {
    StatusChain<TPMErrorBase> err =
        tpm_->GetAuthValue(cryptohome_key, pass_blob, &auth_value);
    if (err == nullptr) {
      break;
    }

    if (!TpmAuthBlockUtils::TPMErrorIsRetriable(err)) {
      LOG(ERROR) << "Failed to get auth value: " << err;
      return TpmAuthBlockUtils::TPMErrorToCrypto(err);
    }

    // If the error is retriable, reload the key first.
    if (!cryptohome_key_loader_->ReloadCryptohomeKey()) {
      LOG(ERROR) << "Unable to reload Cryptohome key while creating "
                    "TpmBoundToPcrAuthBlock:"
                 << err;
      // This would happen when the TPM daemons go into a strange state (e.g.
      // crased). Asking the user to reboot may resolve this issue.
      return CryptoError::CE_TPM_REBOOT;
    }
  }

  brillo::SecureBlob tpm_key;
  StatusChain<TPMErrorBase> err = tpm_->SealToPcrWithAuthorization(
      vkk_key, auth_value, default_pcr_map, &tpm_key);
  if (err != nullptr) {
    LOG(ERROR) << "Failed to wrap vkk with creds: " << err;
    return TpmAuthBlockUtils::TPMErrorToCrypto(err);
  }

  brillo::SecureBlob extended_tpm_key;
  err = tpm_->SealToPcrWithAuthorization(vkk_key, auth_value, extended_pcr_map,
                                         &extended_tpm_key);
  if (err != nullptr) {
    LOG(ERROR) << "Failed to wrap vkk with creds for extended PCR: " << err;
    return TpmAuthBlockUtils::TPMErrorToCrypto(err);
  }

  TpmBoundToPcrAuthBlockState tpm_state;

  // Allow this to fail.  It is not absolutely necessary; it allows us to
  // detect a TPM clear.  If this fails due to a transient issue, then on next
  // successful login, the vault keyset will be re-saved anyway.
  brillo::SecureBlob pub_key_hash;
  err = tpm_->GetPublicKeyHash(cryptohome_key, &pub_key_hash);
  if (err != nullptr) {
    LOG(ERROR) << "Failed to get the TPM public key hash: " << err;
  } else {
    tpm_state.tpm_public_key_hash = pub_key_hash;
  }

  tpm_state.scrypt_derived = true;
  tpm_state.tpm_key = std::move(tpm_key);
  tpm_state.extended_tpm_key = std::move(extended_tpm_key);
  tpm_state.salt = std::move(salt);

  // Pass back the vkk_key and vkk_iv so the generic secret wrapping can use it.
  key_blobs->vkk_key = vkk_key;
  // Note that one might expect the IV to be part of the AuthBlockState. But
  // since it's taken from the scrypt output, it's actually created by the auth
  // block, not used to initialize the auth block.
  key_blobs->vkk_iv = vkk_iv;
  key_blobs->chaps_iv = vkk_iv;

  *auth_block_state = AuthBlockState{.state = std::move(tpm_state)};
  return CryptoError::CE_NONE;
}

CryptoError TpmBoundToPcrAuthBlock::Derive(const AuthInput& auth_input,
                                           const AuthBlockState& state,
                                           KeyBlobs* key_out_data) {
  const TpmBoundToPcrAuthBlockState* tpm_state;
  if (!(tpm_state = absl::get_if<TpmBoundToPcrAuthBlockState>(&state.state))) {
    LOG(ERROR) << "Invalid AuthBlockState";
    return CryptoError::CE_OTHER_CRYPTO;
  }

  if (!tpm_state->scrypt_derived) {
    LOG(ERROR) << "All TpmBoundtoPcr operations should be scrypt derived.";
    return CryptoError::CE_OTHER_CRYPTO;
  }
  if (!tpm_state->salt.has_value()) {
    LOG(ERROR) << "Invalid TpmBoundToPcrAuthBlockState: missing salt";
    return CryptoError::CE_OTHER_CRYPTO;
  }
  if (!tpm_state->tpm_key.has_value()) {
    LOG(ERROR) << "Invalid TpmBoundToPcrAuthBlockState: missing tpm_key";
    return CryptoError::CE_OTHER_CRYPTO;
  }
  if (!tpm_state->extended_tpm_key.has_value()) {
    LOG(ERROR)
        << "Invalid TpmBoundToPcrAuthBlockState: missing extended_tpm_key";
    return CryptoError::CE_OTHER_CRYPTO;
  }

  brillo::SecureBlob tpm_public_key_hash =
      tpm_state->tpm_public_key_hash.value_or(brillo::SecureBlob());

  CryptoError error = utils_.CheckTPMReadiness(
      tpm_state->tpm_key.has_value(),
      tpm_state->tpm_public_key_hash.has_value(), tpm_public_key_hash);
  if (error != CryptoError::CE_NONE) {
    return error;
  }

  key_out_data->vkk_iv = brillo::SecureBlob(kAesBlockSize);
  key_out_data->vkk_key = brillo::SecureBlob(kDefaultAesKeySize);

  bool locked_to_single_user = auth_input.locked_to_single_user.value_or(false);
  brillo::SecureBlob salt = tpm_state->salt.value();
  brillo::SecureBlob tpm_key = locked_to_single_user
                                   ? tpm_state->extended_tpm_key.value()
                                   : tpm_state->tpm_key.value();
  error = DecryptTpmBoundToPcr(auth_input.user_input.value(), tpm_key, salt,
                               &key_out_data->vkk_iv.value(),
                               &key_out_data->vkk_key.value());
  if (error != CryptoError::CE_NONE) {
    if (!tpm_state->tpm_public_key_hash.has_value()) {
      return CryptoError::CE_NO_PUBLIC_KEY_HASH;
    }
    return error;
  }

  key_out_data->chaps_iv = key_out_data->vkk_iv;

  return CryptoError::CE_NONE;
}

CryptoError TpmBoundToPcrAuthBlock::DecryptTpmBoundToPcr(
    const brillo::SecureBlob& vault_key,
    const brillo::SecureBlob& tpm_key,
    const brillo::SecureBlob& salt,
    brillo::SecureBlob* vkk_iv,
    brillo::SecureBlob* vkk_key) const {
  brillo::SecureBlob pass_blob(kDefaultPassBlobSize);

  // Prepare the parameters for scrypt.
  std::vector<brillo::SecureBlob*> gen_secrets{&pass_blob, vkk_iv};
  bool derive_result = false;

  base::WaitableEvent done(base::WaitableEvent::ResetPolicy::MANUAL,
                           base::WaitableEvent::InitialState::NOT_SIGNALED);

  // Derive secrets on scrypt task runner.
  scrypt_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](const brillo::SecureBlob& passkey, const brillo::SecureBlob& salt,
             std::vector<brillo::SecureBlob*> gen_secrets, bool* result,
             base::WaitableEvent* done) {
            *result =
                DeriveSecretsScrypt(passkey, salt, std::move(gen_secrets));
            done->Signal();
          },
          vault_key, salt, gen_secrets, &derive_result, &done));

  // Preload the sealed data while deriving secrets in scrypt.
  ScopedKeyHandle preload_handle;
  StatusChain<TPMErrorBase> err;
  for (int i = 0; i < kTpmDecryptMaxRetries; ++i) {
    err = tpm_->PreloadSealedData(tpm_key, &preload_handle);
    if (err == nullptr)
      break;

    if (!TpmAuthBlockUtils::TPMErrorIsRetriable(err))
      break;
  }

  // Wait for the scrypt finished.
  done.Wait();

  if (!derive_result) {
    LOG(ERROR) << "scrypt derivation failed";
    return CryptoError::CE_OTHER_CRYPTO;
  }

  if (err != nullptr) {
    LOG(ERROR) << "Failed to preload the sealed data: " << err;
    return TpmAuthBlockUtils::TPMErrorToCrypto(err);
  }

  // On TPM1.2 devices, preloading sealed data is meaningless and
  // UnsealWithAuthorization will check the preload_handle not containing any
  // value.
  std::optional<TpmKeyHandle> handle;
  if (preload_handle.has_value()) {
    handle = preload_handle.value();
  }

  for (int i = 0; i < kTpmDecryptMaxRetries; ++i) {
    TpmKeyHandle cryptohome_key = cryptohome_key_loader_->GetCryptohomeKey();
    brillo::SecureBlob auth_value;
    err = tpm_->GetAuthValue(cryptohome_key, pass_blob, &auth_value);
    if (err == nullptr) {
      std::map<uint32_t, brillo::Blob> pcr_map(
          {{kTpmSingleUserPCR, brillo::Blob()}});
      err = tpm_->UnsealWithAuthorization(handle, tpm_key, auth_value, pcr_map,
                                          vkk_key);
      if (err == nullptr) {
        return CryptoError::CE_NONE;
      }
    }

    if (!TpmAuthBlockUtils::TPMErrorIsRetriable(err)) {
      break;
    }

    // If the error is retriable, reload the key first.
    if (!cryptohome_key_loader_->ReloadCryptohomeKey()) {
      LOG(ERROR) << "Unable to reload Cryptohome key while decrypting "
                    "TpmBoundToPcrAuthBlock:"
                 << err;
      break;
    }
  }
  if (err != nullptr) {
    LOG(ERROR) << "Failed to unwrap VKK with creds: " << err;
  }
  return TpmAuthBlockUtils::TPMErrorToCrypto(err);
}

}  // namespace cryptohome
