// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "diagnostics/cros_healthd/delegate/utils/edid.h"

namespace diagnostics {

namespace {

class EdidTestDelegate : public ::testing::Test {
 protected:
  EdidTestDelegate() = default;

  // The EDID raw data below was obtained by |modetest -c| and is the real data
  // from a DUT with an external monitor.
  const std::vector<uint8_t> edp_blob_data = {
      0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x06, 0xAF, 0x3D, 0x32,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x1C, 0x01, 0x04, 0xA5, 0x1F, 0x11, 0x78,
      0x03, 0x3E, 0x85, 0x91, 0x56, 0x59, 0x91, 0x28, 0x1F, 0x50, 0x54, 0x00,
      0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x77, 0x3F, 0x80, 0x3C, 0x71, 0x38,
      0x82, 0x40, 0x10, 0x10, 0x3E, 0x00, 0x35, 0xAE, 0x10, 0x00, 0x00, 0x18,
      0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x41,
      0x55, 0x4F, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
      0x00, 0x00, 0x00, 0xFE, 0x00, 0x42, 0x31, 0x34, 0x30, 0x48, 0x41, 0x4B,
      0x30, 0x33, 0x2E, 0x32, 0x20, 0x0A, 0x00, 0xB6};
  const std::vector<uint8_t> dp_blob_data = {
      0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x10, 0xAC, 0x31, 0x42,
      0x4C, 0x54, 0x48, 0x45, 0x03, 0x20, 0x01, 0x04, 0xB5, 0x3C, 0x22, 0x78,
      0x3E, 0xEE, 0x95, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54, 0xA5,
      0x4B, 0x00, 0x71, 0x4F, 0x81, 0x80, 0xA9, 0xC0, 0xA9, 0x40, 0xD1, 0xC0,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x56, 0x5E, 0x00, 0xA0, 0xA0, 0xA0,
      0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1A,
      0x00, 0x00, 0x00, 0xFF, 0x00, 0x43, 0x47, 0x59, 0x43, 0x34, 0x48, 0x33,
      0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x44,
      0x45, 0x4C, 0x4C, 0x20, 0x55, 0x32, 0x37, 0x32, 0x32, 0x44, 0x45, 0x0A,
      0x00, 0x00, 0x00, 0xFD, 0x00, 0x31, 0x4C, 0x1E, 0x5A, 0x19, 0x01, 0x0A,
      0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x2E, 0x02, 0x03, 0x19, 0xF1,
      0x4C, 0x90, 0x04, 0x03, 0x02, 0x01, 0x11, 0x12, 0x13, 0x1F, 0x20, 0x21,
      0x22, 0x23, 0x09, 0x7F, 0x07, 0x83, 0x01, 0x00, 0x00, 0x02, 0x3A, 0x80,
      0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0x55, 0x50, 0x21,
      0x00, 0x00, 0x1E, 0x7E, 0x39, 0x00, 0xA0, 0x80, 0x38, 0x1F, 0x40, 0x30,
      0x20, 0x3A, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1A, 0x01, 0x1D, 0x00,
      0x72, 0x51, 0xD0, 0x1E, 0x20, 0x6E, 0x28, 0x55, 0x00, 0x55, 0x50, 0x21,
      0x00, 0x00, 0x1E, 0xBF, 0x16, 0x00, 0xA0, 0x80, 0x38, 0x13, 0x40, 0x30,
      0x20, 0x3A, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x06};
};

TEST_F(EdidTestDelegate, ParseEdpEdid) {
  auto info = Edid::From(edp_blob_data);
  EXPECT_TRUE(info.has_value());
  EXPECT_EQ(info.value().manufacturer, "AUO");
  EXPECT_EQ(info.value().model_id, 0x323D);
  EXPECT_FALSE(info.value().serial_number.has_value());
  EXPECT_EQ(info.value().manufacture_week.value(), 20);
  EXPECT_EQ(info.value().manufacture_year.value(), 2018);
  EXPECT_EQ(info.value().edid_version, "1.4");
  EXPECT_EQ(info.value().is_digital_input, true);
  EXPECT_FALSE(info.value().display_name.has_value());
}

TEST_F(EdidTestDelegate, ParseDpEdid) {
  auto info = Edid::From(dp_blob_data);
  EXPECT_TRUE(info.has_value());
  EXPECT_EQ(info.value().manufacturer, "DEL");
  EXPECT_EQ(info.value().model_id, 0x4231);
  EXPECT_EQ(info.value().serial_number.value(), 1162368076);
  EXPECT_EQ(info.value().manufacture_week.value(), 3);
  EXPECT_EQ(info.value().manufacture_year.value(), 2022);
  EXPECT_EQ(info.value().edid_version, "1.4");
  EXPECT_EQ(info.value().is_digital_input, true);
  EXPECT_EQ(info.value().display_name.value(), "DELL U2722DE");
}

}  // namespace

}  // namespace diagnostics
