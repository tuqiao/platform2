// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RUNTIME_PROBE_FUNCTIONS_AP_I2C_H_
#define RUNTIME_PROBE_FUNCTIONS_AP_I2C_H_

#include "runtime_probe/probe_function.h"

namespace runtime_probe {

// Read data from an I2C register on AP (application processor).
// This probe function takes the following arguments:
//   i2c_bus: The port of the I2C connected to EC.
//   chip_addr: The I2C address
//   data_addr: The register offset.
class ApI2cFunction : public PrivilegedProbeFunction {
  using PrivilegedProbeFunction::PrivilegedProbeFunction;

 public:
  NAME_PROBE_FUNCTION("ap_i2c");

  template <typename T>
  static auto FromKwargsValue(const base::Value& dict_value) {
    PARSE_BEGIN();
    PARSE_ARGUMENT(i2c_bus);
    PARSE_ARGUMENT(chip_addr);
    PARSE_ARGUMENT(data_addr);
    PARSE_END();
  }

 private:
  DataType EvalImpl() const override;

  int i2c_bus_;
  int chip_addr_;
  int data_addr_;
};

}  // namespace runtime_probe

#endif  // RUNTIME_PROBE_FUNCTIONS_AP_I2C_H_