// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RUNTIME_PROBE_FUNCTIONS_MMC_STORAGE_H_
#define RUNTIME_PROBE_FUNCTIONS_MMC_STORAGE_H_

#include <optional>
#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/values.h>

#include "runtime_probe/function_templates/storage.h"

namespace runtime_probe {

class MmcStorageFunction final : public StorageFunction {
  using StorageFunction::StorageFunction;

 public:
  NAME_PROBE_FUNCTION("mmc_storage");

 protected:
  std::optional<base::Value> ProbeFromSysfs(
      const base::FilePath& node_path) const override;
  std::optional<base::Value> ProbeFromStorageTool(
      const base::FilePath& node_path) const override;
};

}  // namespace runtime_probe

#endif  // RUNTIME_PROBE_FUNCTIONS_MMC_STORAGE_H_
