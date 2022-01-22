// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <base/containers/span.h>
#include <gtest/gtest.h>

#include "runtime_probe/functions/edid.h"
#include "runtime_probe/system/context_mock_impl.h"
#include "runtime_probe/utils/function_test_utils.h"

namespace runtime_probe {
namespace {

constexpr unsigned char kEdidTestData[] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x38, 0x70, 0x46, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x1c, 0x01, 0x04, 0xa5, 0x22, 0x13, 0x78,
    0x02, 0x68, 0x50, 0x98, 0x5c, 0x58, 0x8e, 0x28, 0x1b, 0x50, 0x54, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1a, 0x36, 0x80, 0xa0, 0x70, 0x38,
    0x1f, 0x40, 0x30, 0x20, 0x35, 0x00, 0x58, 0xc2, 0x10, 0x00, 0x00, 0x1a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x43,
    0x45, 0x43, 0x20, 0x50, 0x41, 0x4e, 0x44, 0x41, 0x20, 0x20, 0x20, 0x20,
    0x00, 0x00, 0x00, 0xfe, 0x00, 0x4c, 0x4d, 0x31, 0x35, 0x36, 0x4c, 0x46,
    0x2d, 0x35, 0x4c, 0x30, 0x34, 0x0a, 0x00, 0x11};

class EdidFunctionTest : public BaseFunctionTest {
 public:
  EdidFunctionTest() {
    // Create card0/ but don't create card0/edid
    SetFile("sys/class/drm/card0/unused", "");
    SetFile("sys/class/drm/card0-DP-1/edid", "");
    SetFile("sys/class/drm/card0-DP-2/edid", "");
    SetFile("sys/class/drm/card0-eDP-1/edid", base::span{kEdidTestData});
  }
};

TEST_F(EdidFunctionTest, ProbeEdid) {
  base::Value probe_statement(base::Value::Type::DICTIONARY);
  auto probe_function = CreateProbeFunction<EdidFunction>(probe_statement);
  auto result = probe_function->Eval();
  auto ans = CreateProbeResultFromJson(R"JSON(
    [
      {
        "height": 1080,
        "product_id": "0046",
        "vendor": "NCP",
        "width": 1920
      }
    ]
  )JSON");
  ans[0].SetStringKey(
      "path", GetPathUnderRoot("sys/class/drm/card0-eDP-1/edid").value());
  EXPECT_EQ(result, ans);
}

}  // namespace
}  // namespace runtime_probe
