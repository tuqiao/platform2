/*
 * Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <fcntl.h>

#include <base/at_exit.h>
#include <base/logging.h>
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>

#include "arc/adbd/adbd.h"

namespace {

constexpr char kRuntimePath[] = "/run/arc/adbd";

}  // namespace

int main(int argc, char** argv) {
  DEFINE_string(serialnumber, "", "Serial number of the Android container");

  base::AtExitManager at_exit;

  brillo::FlagHelper::Init(argc, argv, "ADB over USB proxy.");
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogToStderrIfTty);

  const base::FilePath runtime_path(kRuntimePath);

  adbd::AdbdConfiguration config;
  if (!adbd::GetConfiguration(&config)) {
    LOG(INFO) << "Unable to find the configuration for this service. "
              << "This device does not support ADB over USB.";
    return 0;
  }

  const std::string board = adbd::GetStrippedReleaseBoard();

  const base::FilePath control_pipe_path = runtime_path.Append("ep0");
  if (!adbd::CreatePipe(control_pipe_path))
    return 1;

  char buffer[4096];

  bool configured = false;
  base::ScopedFD control_file;
  while (true) {
    LOG(INFO) << "arc-adbd ready to receive connections";
    // O_RDONLY on a FIFO waits until another endpoint has opened the file with
    // O_WRONLY or O_RDWR.
    base::ScopedFD control_pipe(
        open(control_pipe_path.value().c_str(), O_RDONLY));
    if (!control_pipe.is_valid()) {
      PLOG(ERROR) << "Failed to open FIFO at " << control_pipe_path.value();
      return 1;
    }
    LOG(INFO) << "arc-adbd connected";

    // Given that a FIFO can be opened by multiple processes, once a process has
    // opened it, we atomically replace it with a new FIFO (by using rename(2))
    // so no other process can open it. This causes that when that process
    // close(2)s the FD, we will get an EOF when we attempt to read(2) from it.
    // This also causes any other process that attempts to open the new FIFO to
    // block until we are done processing the current one.
    //
    // There is a very small chance there is a race here if multiple processes
    // get to open the FIFO between the point in time where this process opens
    // the FIFO and CreatePipe() returns. That seems unavoidable, but should not
    // present too much of a problem since exactly one process in Android has
    // the correct user to open this file in the first place (adbd).
    if (!adbd::CreatePipe(control_pipe_path))
      return 1;

    // Once adbd has opened the control pipe, we set up the adb gadget on behalf
    // of that process, if we have not already.
    if (!configured) {
      if (!adbd::SetupKernelModules(config.kernel_modules)) {
        LOG(ERROR) << "Failed to load kernel modules";
        return 1;
      }
      const std::string udc_driver_name = adbd::GetUDCDriver();
      if (udc_driver_name.empty()) {
        LOG(ERROR)
            << "Unable to find any registered UDC drivers in /sys/class/udc/. "
            << "This device does not support ADB using GadgetFS.";
        return 1;
      }
      if (!adbd::SetupConfigFS(FLAGS_serialnumber, config.usb_product_id,
                               board)) {
        LOG(ERROR) << "Failed to configure ConfigFS";
        return 1;
      }
      control_file = adbd::SetupFunctionFS(udc_driver_name);
      if (!control_file.is_valid()) {
        LOG(ERROR) << "Failed to configure FunctionFS";
        return 1;
      }

      configured = true;
    }

    // Drain the FIFO and wait until the other side closes it.
    // The data that is sent is kControlPayloadV2 (or kControlPayloadV1)
    // followed by kControlStrings. We ignore it completely since we have
    // already sent it to the underlying FunctionFS file, and also to avoid
    // parsing it to decrease the attack surface area.
    while (true) {
      ssize_t bytes_read = read(control_pipe.get(), buffer, sizeof(buffer));
      if (bytes_read < 0)
        PLOG(ERROR) << "Failed to read from FIFO";
      if (bytes_read <= 0)
        break;
    }
  }
}
