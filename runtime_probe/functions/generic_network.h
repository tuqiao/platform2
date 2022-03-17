// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RUNTIME_PROBE_FUNCTIONS_GENERIC_NETWORK_H_
#define RUNTIME_PROBE_FUNCTIONS_GENERIC_NETWORK_H_

#include <memory>
#include <optional>
#include <string>

#include "runtime_probe/function_templates/network.h"

namespace runtime_probe {

class GenericNetworkFunction : public NetworkFunction {
  using NetworkFunction::NetworkFunction;

 public:
  NAME_PROBE_FUNCTION("generic_network");

 protected:
  std::optional<std::string> GetNetworkType() const override;
};

}  // namespace runtime_probe

#endif  // RUNTIME_PROBE_FUNCTIONS_GENERIC_NETWORK_H_
