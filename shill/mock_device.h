// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_MOCK_DEVICE_H_
#define SHILL_MOCK_DEVICE_H_

#include <string>
#include <vector>

#include <base/memory/ref_counted.h>
#include <gmock/gmock.h>

#include "shill/device.h"
#include "shill/geolocation_info.h"

namespace shill {

class MockDevice : public Device {
 public:
  MockDevice(Manager* manager,
             const std::string& link_name,
             const std::string& address,
             int interface_index);
  MockDevice(const MockDevice&) = delete;
  MockDevice& operator=(const MockDevice&) = delete;

  ~MockDevice() override;

  MOCK_METHOD(void, Initialize, (), (override));
  MOCK_METHOD(void, Start, (EnabledStateChangedCallback), (override));
  MOCK_METHOD(void, Stop, (EnabledStateChangedCallback), (override));
  MOCK_METHOD(void, SetEnabled, (bool), (override));
  MOCK_METHOD(void,
              SetEnabledChecked,
              (bool, bool, ResultOnceCallback),
              (override));
  MOCK_METHOD(void, Scan, (Error*, const std::string&), (override));
  MOCK_METHOD(bool, Load, (const StoreInterface*), (override));
  MOCK_METHOD(bool, Save, (StoreInterface*), (override));
  MOCK_METHOD(bool, UpdatePortalDetector, (bool), (override));
  MOCK_METHOD(bool,
              IsConnectedToService,
              (const ServiceRefPtr&),
              (const, override));
  MOCK_METHOD(Technology, technology, (), (const, override));
  MOCK_METHOD(void, OnBeforeSuspend, (ResultOnceCallback), (override));
  MOCK_METHOD(void, OnDarkResume, (ResultOnceCallback), (override));
  MOCK_METHOD(void, OnAfterResume, (), (override));
  MOCK_METHOD(void,
              UpdateGeolocationObjects,
              (std::vector<GeolocationInfo>*),
              (const, override));
  MOCK_METHOD(void,
              OnNeighborReachabilityEvent,
              (int,
               const IPAddress&,
               patchpanel::NeighborReachabilityEventSignal::Role,
               patchpanel::NeighborReachabilityEventSignal::EventType),
              (override));
};

}  // namespace shill

#endif  // SHILL_MOCK_DEVICE_H_
