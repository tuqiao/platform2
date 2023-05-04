// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_factor/types/cryptohome_recovery.h"

#include "cryptohome/auth_blocks/cryptohome_recovery_auth_block.h"
#include "cryptohome/auth_factor/auth_factor_label_arity.h"
#include "cryptohome/auth_factor/auth_factor_metadata.h"
#include "cryptohome/auth_factor/auth_factor_storage_type.h"
#include "cryptohome/auth_factor/auth_factor_type.h"

namespace cryptohome {

bool CryptohomeRecoveryAuthFactorDriver::IsSupported(
    const std::set<AuthFactorStorageType>& configured_storage_types,
    const std::set<AuthFactorType>& configured_factors) const {
  if (configured_factors.count(AuthFactorType::kKiosk) > 0) {
    return false;
  }
  return configured_storage_types.size() == 1 &&
         configured_storage_types.count(
             AuthFactorStorageType::kUserSecretStash) &&
         CryptohomeRecoveryAuthBlock::IsSupported(*crypto_).ok();
}

bool CryptohomeRecoveryAuthFactorDriver::NeedsResetSecret() const {
  return false;
}

bool CryptohomeRecoveryAuthFactorDriver::NeedsRateLimiter() const {
  return false;
}

AuthFactorLabelArity
CryptohomeRecoveryAuthFactorDriver::GetAuthFactorLabelArity() const {
  return AuthFactorLabelArity::kSingle;
}

std::optional<user_data_auth::AuthFactor>
CryptohomeRecoveryAuthFactorDriver::TypedConvertToProto(
    const CommonAuthFactorMetadata& common,
    const CryptohomeRecoveryAuthFactorMetadata& typed_metadata) const {
  user_data_auth::AuthFactor proto;
  proto.set_type(user_data_auth::AUTH_FACTOR_TYPE_CRYPTOHOME_RECOVERY);
  // TODO(b/232896212): There's no metadata for recovery auth factor
  // currently.
  proto.mutable_cryptohome_recovery_metadata();
  return proto;
}

}  // namespace cryptohome
