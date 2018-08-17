// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VM_TOOLS_NOTIFICATIOND_DBUS_SERVICE_H_
#define VM_TOOLS_NOTIFICATIOND_DBUS_SERVICE_H_

#include <memory>

#include <dbus/bus.h>
#include <dbus/exported_object.h>
#include <dbus/message.h>
#include <gtest/gtest_prod.h>

#include "vm_tools/notificationd/dbus_interface.h"

namespace vm_tools {
namespace notificationd {

// Handles D-BUS connecton for org.freedesktop.Notifications as a server
// according to the Desktop Notifications Specification here:
// https://developer.gnome.org/notification-spec/
class DBusService {
 public:
  ~DBusService() = default;

  // Creates and returns a DBusService. The |interface| is required to outlive
  // this DBusService. Returns nullptr if the the service failed to be
  // initialized for any reason.
  static std::unique_ptr<DBusService> Create(DBusInterface* interface);

 private:
  explicit DBusService(DBusInterface* interface);

  bool RegisterMethods();

  // Initializes D-BUS connection for org.freedesktop.Notifications. Returns
  // true on success.
  bool Init();

  std::unique_ptr<dbus::Response> CallGetCapabilities(
      dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> CallNotify(dbus::MethodCall* method_call);
  std::unique_ptr<dbus::Response> CallGetServerInformation(
      dbus::MethodCall* method_call);

  DBusInterface* const interface_;

  scoped_refptr<dbus::Bus> bus_;
  dbus::ExportedObject* exported_object_ = nullptr;  // Owned by |bus_|.

  FRIEND_TEST(DBusServiceTest, GetCapabilities);
  FRIEND_TEST(DBusServiceTest, Notify);
  FRIEND_TEST(DBusServiceTest, GetServerInformation);

  DISALLOW_COPY_AND_ASSIGN(DBusService);
};

}  // namespace notificationd
}  // namespace vm_tools

#endif  // VM_TOOLS_NOTIFICATIOND_DBUS_SERVICE_H_
