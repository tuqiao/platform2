// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_BLOCKS_FINGERPRINT_AUTH_BLOCK_H_
#define CRYPTOHOME_AUTH_BLOCKS_FINGERPRINT_AUTH_BLOCK_H_

#include "cryptohome/auth_blocks/auth_block.h"
#include "cryptohome/auth_blocks/biometrics_auth_block_service.h"
#include "cryptohome/crypto.h"
#include "cryptohome/error/cryptohome_crypto_error.h"
#include "cryptohome/flatbuffer_schemas/auth_block_state.h"
#include "cryptohome/le_credential_manager.h"

namespace cryptohome {

class FingerprintAuthBlock : public AuthBlock {
 public:
  // Returns success if the AuthBlock is supported on the current hardware and
  // software environment.
  static CryptoStatus IsSupported(Crypto& crypto);

  FingerprintAuthBlock(LECredentialManager* le_manager,
                       BiometricsAuthBlockService* service);
  FingerprintAuthBlock(const FingerprintAuthBlock&) = delete;
  FingerprintAuthBlock& operator=(const FingerprintAuthBlock&) = delete;

  void Create(const AuthInput& auth_input, CreateCallback callback) override;

  void Derive(const AuthInput& auth_input,
              const AuthBlockState& state,
              DeriveCallback callback) override;

  CryptohomeStatus PrepareForRemoval(const AuthBlockState& state) override;

 private:
  // Continue creating the KeyBlobs after receiving CreateCredential reply. This
  // is used as the callback of BiometricsAuthBlockService::CreateCredential.
  void ContinueCreate(
      CreateCallback callback,
      const ObfuscatedUsername& obfuscated_username,
      const brillo::SecureBlob& reset_secret,
      CryptohomeStatusOr<BiometricsAuthBlockService::OperationOutput> output);

  LECredentialManager* le_manager_;
  BiometricsAuthBlockService* service_;
  base::WeakPtrFactory<FingerprintAuthBlock> weak_factory_{this};
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_BLOCKS_FINGERPRINT_AUTH_BLOCK_H_