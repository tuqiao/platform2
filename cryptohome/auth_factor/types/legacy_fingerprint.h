// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_FACTOR_TYPES_LEGACY_FINGERPRINT_H_
#define CRYPTOHOME_AUTH_FACTOR_TYPES_LEGACY_FINGERPRINT_H_

#include <optional>
#include <string>
#include <variant>

#include "cryptohome/auth_factor/auth_factor_label_arity.h"
#include "cryptohome/auth_factor/auth_factor_metadata.h"
#include "cryptohome/auth_factor/auth_factor_type.h"
#include "cryptohome/auth_factor/types/common.h"
#include "cryptohome/auth_factor/types/interface.h"

namespace cryptohome {

class LegacyFingerprintAuthFactorDriver final
    : public TypedAuthFactorDriver<std::monostate> {
 public:
  LegacyFingerprintAuthFactorDriver();

 private:
  bool NeedsResetSecret() const override;
  bool NeedsRateLimiter() const override;
  AuthFactorLabelArity GetAuthFactorLabelArity() const override;

  std::optional<user_data_auth::AuthFactor> TypedConvertToProto(
      const CommonAuthFactorMetadata& common,
      const std::monostate& typed_metadata) const override;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_FACTOR_TYPES_LEGACY_FINGERPRINT_H_