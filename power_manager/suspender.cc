// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/suspender.h"

#include "power_manager/util.h"
#include "base/logging.h"
#include "chromeos/dbus/service_constants.h"

namespace power_manager {

Suspender::Suspender(ScreenLocker* locker)
    : locker_(locker), suspend_requested_(false) {}

void Suspender::RequestSuspend() {
  if (util::LoggedIn()) {
    suspend_requested_ = true;
    locker_->LockScreen();
    g_timeout_add(3000, CheckSuspendTimeout, this);
  } else {
    LOG(INFO) << "Not logged in. Suspend Request -> Shutting down.";
    util::SendSignalToPowerM(util::kShutdownSignal);

  }
}

void Suspender::CheckSuspend() {
  if (suspend_requested_) {
    suspend_requested_ = false;
    Suspend();
  }
}

void Suspender::CancelSuspend() {
  if (suspend_requested_)
    LOG(INFO) << "Suspend canceled mid flight.";
  suspend_requested_ = false;
}

gboolean Suspender::CheckSuspendTimeout(gpointer data) {
  Suspender* suspender = static_cast<Suspender*>(data);
  if (suspender->suspend_requested_) {
    LOG(ERROR) << "Screen locker timed out";
    suspender->CheckSuspend();
  }
  // We don't want this callback to be repeated, so we return false.
  return false;
}

void Suspender::Suspend() {
  util::SendSignalToPowerM(util::kSuspendSignal);
}

}  // namespace power_manager
