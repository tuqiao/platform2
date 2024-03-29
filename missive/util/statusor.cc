// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "missive/util/statusor.h"

#include <base/logging.h>
#include <base/no_destructor.h>

namespace reporting::internal {

// static
const Status& StatusOrHelper::NotInitializedStatus() {
  static base::NoDestructor<Status> status_not_initialized(error::UNKNOWN,
                                                           "Not initialized");
  return *status_not_initialized;
}

// static
const Status& StatusOrHelper::MovedOutStatus() {
  static base::NoDestructor<Status> status_moved_out(error::UNKNOWN,
                                                     "Value moved out");
  return *status_moved_out;
}

// static
void StatusOrHelper::Crash(const Status& status) {
  LOG(FATAL) << "Attempting to fetch value instead of handling error "
             << status.ToString();
}
}  // namespace reporting::internal
