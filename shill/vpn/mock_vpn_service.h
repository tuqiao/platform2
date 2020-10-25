// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_VPN_MOCK_VPN_SERVICE_H_
#define SHILL_VPN_MOCK_VPN_SERVICE_H_

#include <memory>
#include <string>

#include <gmock/gmock.h>

#include "shill/vpn/vpn_service.h"

namespace shill {

class MockVPNService : public VPNService {
 public:
  MockVPNService(Manager* manager, std::unique_ptr<VPNDriver> driver);
  ~MockVPNService() override;

  MOCK_METHOD(void, SetState, (ConnectState), (override));
  MOCK_METHOD(void, SetFailure, (ConnectFailure), (override));
  MOCK_METHOD(void, InitDriverPropertyStore, (), (override));
  MOCK_METHOD(void,
              OnDriverEvent,
              (DriverEvent, ConnectFailure, const std::string&),
              (override));

  VPNService::DriverEventCallback GetCallback() {
    return base::BindRepeating(&MockVPNService::OnDriverEvent,
                               weak_factory_.GetWeakPtr());
  }

 private:
  base::WeakPtrFactory<MockVPNService> weak_factory_{this};
  DISALLOW_COPY_AND_ASSIGN(MockVPNService);
};

}  // namespace shill

#endif  // SHILL_VPN_MOCK_VPN_SERVICE_H_
