// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/mock_wake_on_wifi.h"

#include <gmock/gmock.h>

namespace shill {

MockWakeOnWiFi::MockWakeOnWiFi(NetlinkManager *netlink_manager,
                               EventDispatcher *dispatcher, Manager *manager)
    : WakeOnWiFi(netlink_manager, dispatcher, manager) {}

MockWakeOnWiFi::~MockWakeOnWiFi() {}

}  // namespace shill
