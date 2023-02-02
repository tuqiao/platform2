// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RUNTIME_PROBE_SSFC_PROBE_CONFIG_LOADER_H_
#define RUNTIME_PROBE_SSFC_PROBE_CONFIG_LOADER_H_

#include "runtime_probe/probe_config_loader.h"

#include <vector>

#include <base/files/file_path.h>

namespace runtime_probe {

inline constexpr char kSsfcProbeConfigName[] = "probe_config_ssfc.json";

// SsfcProbeConfigLoader loads probe configs for second source components for
// SSFC.
class SsfcProbeConfigLoader : public ProbeConfigLoader {
 public:
  SsfcProbeConfigLoader() = default;

  // Load probe config from AVL config paths. The function will return
  // std::nullopt when loading fails.
  std::optional<ProbeConfigData> Load() const override;

 private:
  // Return default paths for SSFC probe configs.  When cros_debug is disabled,
  // the default paths will be:
  //     * `/etc/runtime_probe/<model_name>/probe_config_ssfc.json`
  // When cros_debug is enabled, the config paths under the stateful partition
  // will also be included:
  //     * `/usr/local/etc/runtime_probe/<model_name>/probe_config_ssfc.json`
  //     * `/etc/runtime_probe/<model_name>/probe_config_ssfc.json`
  std::vector<base::FilePath> GetPaths() const;
};

}  // namespace runtime_probe

#endif  // RUNTIME_PROBE_SSFC_PROBE_CONFIG_LOADER_H_
