// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_BLOCKS_PIN_WEAVER_AUTH_BLOCK_H_
#define CRYPTOHOME_AUTH_BLOCKS_PIN_WEAVER_AUTH_BLOCK_H_

#include "cryptohome/auth_blocks/auth_block.h"
#include "cryptohome/crypto.h"
#include "cryptohome/error/cryptohome_crypto_error.h"
#include "cryptohome/flatbuffer_schemas/auth_block_state.h"
#include "cryptohome/le_credential_manager.h"

namespace cryptohome {

class PinWeaverAuthBlock : public SyncAuthBlock {
 public:
  // Returns success if the AuthBlock is supported on the current hardware and
  // software environment.
  static CryptoStatus IsSupported(Crypto& crypto);

  explicit PinWeaverAuthBlock(LECredentialManager* le_manager);
  PinWeaverAuthBlock(const PinWeaverAuthBlock&) = delete;
  PinWeaverAuthBlock& operator=(const PinWeaverAuthBlock&) = delete;

  CryptoStatus Create(const AuthInput& user_input,
                      AuthBlockState* auth_block_state,
                      KeyBlobs* key_blobs) override;

  CryptoStatus Derive(const AuthInput& auth_input,
                      const AuthBlockState& state,
                      KeyBlobs* key_blobs) override;

  // Removing the underlying Pinweaver leaf node before the AuthFactor is
  // removed.
  CryptoStatus PrepareForRemoval(const AuthBlockState& state) override;

  bool IsLocked(uint64_t label);

 private:
  // Handler for Low Entropy credentials.
  LECredentialManager* le_manager_;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_BLOCKS_PIN_WEAVER_AUTH_BLOCK_H_
