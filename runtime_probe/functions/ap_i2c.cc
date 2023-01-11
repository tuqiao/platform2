// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "runtime_probe/functions/ap_i2c.h"

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string>
#include <utility>

#include <base/files/scoped_file.h>
#include <base/posix/eintr_wrapper.h>
#include <base/strings/stringprintf.h>

namespace runtime_probe {

namespace {

// Read data from I2C registers.
std::optional<uint8_t> i2cget(int i2c_bus, int chip_addr, uint8_t data_addr) {
  if (i2c_bus < 0 || chip_addr < 0) {
    return std::nullopt;
  }

  std::string i2c_filename = base::StringPrintf("/dev/i2c-%d", i2c_bus);
  base::ScopedFD fd(HANDLE_EINTR(open(i2c_filename.c_str(), O_RDWR)));
  if (fd.get() < 0) {
    LOG(ERROR) << "Could not open file " << i2c_filename;
    return std::nullopt;
  }
  // TODO(kevinptt): move ioctl to runtime_probe::Syscaller.
  if (ioctl(fd.get(), I2C_SLAVE, chip_addr) < 0) {
    LOG(ERROR) << "Could not set target address to "
               << base::StringPrintf("0x%02x", chip_addr);
    return std::nullopt;
  }
  i2c_smbus_data data;
  i2c_smbus_ioctl_data args{.read_write = I2C_SMBUS_READ,
                            .command = data_addr,
                            .size = I2C_SMBUS_BYTE_DATA,
                            .data = &data};
  if (ioctl(fd.get(), I2C_SMBUS, &args)) {
    LOG(ERROR) << "Could not read byte "
               << base::StringPrintf("0x%02x", data_addr) << " from "
               << base::StringPrintf("0x%02x", chip_addr) << ": "
               << strerror(errno);
    return std::nullopt;
  }
  return data.byte;
}

}  // namespace

ApI2cFunction::DataType ApI2cFunction::EvalImpl() const {
  DataType result{};

  std::optional<uint8_t> data = i2cget(i2c_bus_, chip_addr_, data_addr_);
  if (data) {
    VLOG(3) << "data: " << base::StringPrintf("0x%02x", *data);
    base::Value::Dict dict;
    dict.Set("data", *data);
    result.Append(std::move(dict));
  } else {
    VLOG(3) << "data: (null)";
  }

  return result;
}

}  // namespace runtime_probe
