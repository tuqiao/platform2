// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BLUETOOTH_NEWBLUED_NEWBLUE_DAEMON_H_
#define BLUETOOTH_NEWBLUED_NEWBLUE_DAEMON_H_

#include <memory>

#include <base/memory/ref_counted.h>
#include <base/memory/weak_ptr.h>
#include <brillo/dbus/exported_object_manager.h>
#include <dbus/bus.h>

#include "bluetooth/common/bluetooth_daemon.h"
#include "bluetooth/common/exported_object_manager_wrapper.h"
#include "bluetooth/newblued/newblue.h"

namespace bluetooth {

class NewblueDaemon : public BluetoothDaemon {
 public:
  explicit NewblueDaemon(std::unique_ptr<Newblue> newblue);
  ~NewblueDaemon() override = default;

  // BluetoothDaemon override:
  bool Init(scoped_refptr<dbus::Bus> bus, DBusDaemon* dbus_daemon) override;

  // Frees up all resources. Currently only needed in test.
  void Shutdown();

  // Called when NewBlue is ready to be brought up.
  void OnHciReadyForUp();

 private:
  // Registers GetAll/Get/Set method handlers.
  void SetupPropertyMethodHandlers(
      brillo::dbus_utils::DBusInterface* prop_interface,
      brillo::dbus_utils::ExportedPropertySet* property_set);

  // Exports org.bluez.Adapter1 interface on object /org/bluez/hci0.
  // The properties of this object will be ignored by btdispatch, but the object
  // still has to be exposed to be able to receive org.bluez.Adapter1 method
  // calls, e.g. StartDiscovery(), StopDiscovery().
  void ExportAdapterInterface();

  scoped_refptr<dbus::Bus> bus_;

  std::unique_ptr<ExportedObjectManagerWrapper>
      exported_object_manager_wrapper_;

  std::unique_ptr<Newblue> newblue_;

  DBusDaemon* dbus_daemon_;

  // Must come last so that weak pointers will be invalidated before other
  // members are destroyed.
  base::WeakPtrFactory<NewblueDaemon> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(NewblueDaemon);
};

}  // namespace bluetooth

#endif  // BLUETOOTH_NEWBLUED_NEWBLUE_DAEMON_H_
