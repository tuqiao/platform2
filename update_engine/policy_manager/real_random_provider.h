// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_PLATFORM_UPDATE_ENGINE_POLICY_MANAGER_REAL_RANDOM_PROVIDER_H_
#define CHROMEOS_PLATFORM_UPDATE_ENGINE_POLICY_MANAGER_REAL_RANDOM_PROVIDER_H_

#include <base/memory/scoped_ptr.h>

#include "update_engine/policy_manager/random_provider.h"

namespace chromeos_policy_manager {

// RandomProvider implementation class.
class RealRandomProvider : public RandomProvider {
 public:
  RealRandomProvider() {}

  virtual Variable<uint64_t>* var_seed() override { return var_seed_.get(); }

  // Initializes the provider and returns whether it succeeded.
  bool Init();

 private:
  // The seed() scoped variable.
  scoped_ptr<Variable<uint64_t>> var_seed_;

  DISALLOW_COPY_AND_ASSIGN(RealRandomProvider);
};

}  // namespace chromeos_policy_manager

#endif  // CHROMEOS_PLATFORM_UPDATE_ENGINE_POLICY_MANAGER_REAL_RANDOM_PROVIDER_H_
