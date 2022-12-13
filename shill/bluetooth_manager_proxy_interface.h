// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_BLUETOOTH_MANAGER_PROXY_INTERFACE_H_
#define SHILL_BLUETOOTH_MANAGER_PROXY_INTERFACE_H_

#include <cstdint>
#include <vector>

namespace shill {

class BluetoothManagerProxyInterface {
 public:
  virtual ~BluetoothManagerProxyInterface() = default;

  virtual bool GetFlossEnabled(bool* enabled) const = 0;

  struct BTAdapterWithEnabled {
    int32_t hci_interface;
    bool enabled;
  };

  virtual bool GetAvailableAdapters(
      std::vector<BTAdapterWithEnabled>* adapters) const = 0;
};

}  // namespace shill

#endif  // SHILL_BLUETOOTH_MANAGER_PROXY_INTERFACE_H_
