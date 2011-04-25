// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CROS_DISKS_SERVER_IMPL_H__
#define CROS_DISKS_SERVER_IMPL_H__

#include "cros-disks-server.h"
#include "disk.h"

#include <string>
#include <vector>

namespace cros_disks {

class DiskManager;

// The d-bus server for the cros-disks daemon. 
//
// Example Usage:
// 
// DBus::Connection server_conn = DBus::Connection::SystemBus();
// server_conn.request_name("org.chromium.CrosDisks");
// CrosDisksServer* server = new(std::nothrow) CrosDisksServer(server_conn);
// 
// At this point the server should be attached to the main loop.
//
class CrosDisksServer : public org::chromium::CrosDisks_adaptor,
                        public DBus::IntrospectableAdaptor,
                        public DBus::ObjectAdaptor {
 public:
  CrosDisksServer(DBus::Connection& connection, DiskManager* disk_manager);
  virtual ~CrosDisksServer();

  // A method for checking if the daemon is running. Always returns true.
  virtual bool IsAlive(DBus::Error& error);  // NOLINT

  // Unmounts a device when invoked.
  virtual void FilesystemUnmount(const std::string& device_path,
      const std::vector<std::string>& mount_options,
      DBus::Error& error); // NOLINT

  // Mounts a device when invoked.
  virtual std::string FilesystemMount(const std::string& device_path,
      const std::string& filesystem_type,
      const std::vector<std::string>& mount_options,
      DBus::Error& error); // NOLINT

  // Returns a list of device files for all disk devices attached to
  // the system.
  virtual std::vector<std::string> EnumerateDeviceFiles(
      DBus::Error& error); // NOLINT

  // Returns properties of a disk device attached to the system.
  virtual DBusDisk GetDeviceProperties(const std::string& device_path,
      DBus::Error& error); // NOLINT

  // Emits appropriate DBus signals notifying device changes.
  void SignalDeviceChanges();

 private:
  DiskManager* disk_manager_;
};
} // namespace cros_disks

#endif // CROS_DISKS_SERVER_IMPL_H__
