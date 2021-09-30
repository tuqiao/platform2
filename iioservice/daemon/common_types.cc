// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "iioservice/daemon/common_types.h"

#include <utility>

#include <base/files/file_util.h>
#include <base/strings/stringprintf.h>
#include <libmems/common_types.h>

namespace iioservice {

namespace {

bool IioDeviceOnDut(libmems::IioDevice* const iio_device) {
  auto path_opt = GetAbsoluteSysPath(iio_device);
  if (!path_opt.has_value())
    return false;

  base::FilePath path = path_opt.value();
  while (!path.empty() && path.DirName() != path) {
    base::FilePath sym_driver;
    if (base::ReadSymbolicLink(path.Append("driver"), &sym_driver) &&
        sym_driver.value().find("ish-hid") != std::string::npos) {
      return true;
    }

    path = path.DirName();
  }

  return false;
}

}  // namespace

base::Optional<base::FilePath> GetAbsoluteSysPath(
    libmems::IioDevice* const iio_device) {
  base::FilePath iio_path(iio_device->GetPath());
  base::FilePath sys_path;
  if (base::ReadSymbolicLink(iio_path, &sys_path)) {
    if (sys_path.IsAbsolute()) {
      return sys_path;
    } else {
      base::FilePath result = iio_path.DirName();
      result = result.Append(sys_path);

      return base::MakeAbsoluteFilePath(result);
    }
  }

  return base::nullopt;
}

DeviceData::DeviceData(libmems::IioDevice* const iio_device,
                       std::set<cros::mojom::DeviceType> types)
    : iio_device(iio_device),
      types(std::move(types)),
      on_dut(IioDeviceOnDut(iio_device)) {}

ClientData::ClientData(const mojo::ReceiverId id, DeviceData* device_data)
    : id(id), device_data(device_data) {}

bool ClientData::IsActive() const {
  return frequency >= libmems::kFrequencyEpsilon &&
         !enabled_chn_indices.empty();
}

std::vector<std::string> GetGravityChannels() {
  std::vector<std::string> channel_ids;
  for (char axis : kChannelAxes) {
    channel_ids.push_back(
        base::StringPrintf(kChannelFormat, cros::mojom::kGravityChannel, axis));
  }
  channel_ids.push_back(cros::mojom::kTimestampChannel);

  return channel_ids;
}

std::string GetSamplingFrequencyAvailable(double min_frequency,
                                          double max_frequency) {
  return base::StringPrintf(kSamplingFrequencyAvailableFormat, min_frequency,
                            max_frequency);
}

}  // namespace iioservice
