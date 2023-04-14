// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_FACTOR_TYPES_MANAGER_H_
#define CRYPTOHOME_AUTH_FACTOR_TYPES_MANAGER_H_

#include <memory>
#include <unordered_map>

#include "cryptohome/auth_factor/auth_factor_type.h"
#include "cryptohome/auth_factor/types/interface.h"

namespace cryptohome {

// Manager class that will construct all of the auth factor driver instances.
// This will only construct one instance of the driver for each type and so
// multiple lookups of the driver will return the same object, shared between
// all of them.
class AuthFactorDriverManager {
 public:
  AuthFactorDriverManager();

  AuthFactorDriverManager(const AuthFactorDriverManager&) = delete;
  AuthFactorDriverManager& operator=(const AuthFactorDriverManager&) = delete;

  // Return a reference to the driver for the given factor type. The references
  // returned are valid until the driver manager itself is destroyed.
  const AuthFactorDriver& GetDriver(AuthFactorType auth_factor_type) const;

 private:
  // The null driver, used when no valid driver implementation is available.
  const std::unique_ptr<AuthFactorDriver> null_driver_;

  // Store all of the real drivers.
  const std::unordered_map<AuthFactorType, std::unique_ptr<AuthFactorDriver>>
      driver_map_;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_FACTOR_TYPES_MANAGER_H_