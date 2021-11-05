// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HERMES_MODEM_MANAGER_PROXY_H_
#define HERMES_MODEM_MANAGER_PROXY_H_

#include <map>
#include <memory>
#include <string>

#include <chromeos/dbus/service_constants.h>
#include <dbus/bus.h>

#include "hermes/dbus_bindings/mm-proxies.h"
#include "hermes/hermes_common.h"

namespace hermes {

class ModemManagerProxy {
 public:
  using DBusInterfaceToProperties =
      std::map<std::string, brillo::VariantDictionary>;
  using DBusObjectsWithProperties =
      std::map<dbus::ObjectPath, DBusInterfaceToProperties>;
  explicit ModemManagerProxy(const scoped_refptr<dbus::Bus>& bus);

  // cb is executed when a new modem appears on DBus. Executed only once.
  void RegisterModemAppearedCallback(base::OnceClosure cb);
  // If MM has exported a DBus object, executes cb immediately. If not,
  // waits for MM to export a DBus object.
  void WaitForModem(base::OnceClosure cb);

 protected:
  // To be used by mocks only
  ModemManagerProxy();

 private:
  void WaitForModemStepGetObjects(base::OnceClosure cb, bool /*is_available*/);
  void OnInterfaceAdded(const dbus::ObjectPath& object_path,
                        const DBusInterfaceToProperties& properties);
  void WaitForModemStepLast(
      base::OnceClosure cb,
      const DBusObjectsWithProperties& dbus_objects_with_properties);

  scoped_refptr<dbus::Bus> bus_;
  std::unique_ptr<org::freedesktop::DBus::ObjectManagerProxy> proxy_;
  base::OnceClosure on_modem_appeared_cb_;

  base::WeakPtrFactory<ModemManagerProxy> weak_factory_;
};
}  // namespace hermes

#endif  // HERMES_MODEM_MANAGER_PROXY_H_
