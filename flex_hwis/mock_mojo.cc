// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flex_hwis/mock_mojo.h"

#include <string>

namespace flex_hwis {

mojom::TelemetryInfoPtr MockMojo::MockSystemInfo() {
  auto system_info = mojom::SystemInfo::New();

  auto& dmi_info = system_info->dmi_info;
  dmi_info = mojom::DmiInfo::New();
  dmi_info->sys_vendor = kSystemVersion;
  dmi_info->product_name = kSystemProductName;
  dmi_info->product_version = kSystemProductVersion;
  dmi_info->bios_version = kSystemBiosVersion;

  auto& os_info = system_info->os_info;
  os_info = mojom::OsInfo::New();
  os_info->boot_mode = mojom::BootMode::kCrosSecure;

  info_->system_result =
      mojom::SystemResult::NewSystemInfo({std::move(system_info)});
  return std::move(info_);
}

mojom::TelemetryInfoPtr MockMojo::MockCpuInfo() {
  auto cpu_info = mojom::CpuInfo::New();

  auto& physical_cpus = cpu_info->physical_cpus;
  physical_cpus = std::vector<mojom::PhysicalCpuInfoPtr>(1);

  auto& physical_cpu = physical_cpus[0];
  physical_cpu = mojom::PhysicalCpuInfo::New();
  physical_cpu->model_name = kCpuModelName;

  info_->cpu_result = mojom::CpuResult::NewCpuInfo({std::move(cpu_info)});
  return std::move(info_);
}

mojom::TelemetryInfoPtr MockMojo::MockMemoryInfo() {
  auto memory_info = mojom::MemoryInfo::New();
  memory_info->total_memory_kib = kMemoryKib;

  info_->memory_result =
      mojom::MemoryResult::NewMemoryInfo({std::move(memory_info)});
  return std::move(info_);
}

mojom::TelemetryInfoPtr MockMojo::MockGraphicsInfo() {
  auto graphics_info = mojom::GraphicsInfo::New();
  auto& gles_info = graphics_info->gles_info;
  gles_info = mojom::GLESInfo::New();
  gles_info->version = kGraphicsVersion;
  gles_info->vendor = kGraphicsVendor;
  gles_info->renderer = kGraphicsRenderer;
  gles_info->shading_version = kGraphicsShadingVer;

  auto& extensions = gles_info->extensions;
  extensions = std::vector<std::string>(1);
  extensions[0] = kGraphicsExtension;

  info_->graphics_result =
      mojom::GraphicsResult::NewGraphicsInfo({std::move(graphics_info)});
  return std::move(info_);
}

mojom::TelemetryInfoPtr MockMojo::MockInputInfo() {
  auto input_info = mojom::InputInfo::New();
  input_info->touchpad_library_name = kTouchpadLibraryName;

  info_->input_result =
      mojom::InputResult::NewInputInfo({std::move(input_info)});
  return std::move(info_);
}

mojom::TelemetryInfoPtr MockMojo::MockTpmInfo() {
  auto tpm_info = mojom::TpmInfo::New();
  auto& version = tpm_info->version;
  version = mojom::TpmVersion::New();
  version->family = kTpmFamily;
  version->spec_level = kTpmSpecLevel;
  version->manufacturer = kTpmManufacturer;

  tpm_info->did_vid = kTpmDidVid;

  auto& supported_features = tpm_info->supported_features;
  supported_features = mojom::TpmSupportedFeatures::New();
  supported_features->is_allowed = kTpmIsAllowed;

  auto& status = tpm_info->status;
  status = mojom::TpmStatus::New();
  status->owned = kTpmOwned;

  info_->tpm_result = mojom::TpmResult::NewTpmInfo({std::move(tpm_info)});
  return std::move(info_);
}

mojom::TelemetryInfoPtr MockMojo::MockTelemetryInfo() {
  info_ = MockSystemInfo();
  info_ = MockCpuInfo();
  info_ = MockMemoryInfo();
  info_ = MockPciBusInfo(mojom::BusDeviceClass::kEthernetController, false);
  info_ = MockGraphicsInfo();
  info_ = MockInputInfo();
  info_ = MockTpmInfo();
  return std::move(info_);
}
}  // namespace flex_hwis
