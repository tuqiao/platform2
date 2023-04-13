// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "missive/util/dynamic_flag.h"

#include <atomic>
#include <string>

#include <base/logging.h>

namespace reporting {

DynamicFlag::DynamicFlag(base::StringPiece name, bool is_enabled)
    : name_(name), is_enabled_(is_enabled) {
  LOG(WARNING) << "Flag `" << name_ << "` is initially "
               << (is_enabled ? "enabled" : "disabled");
}

DynamicFlag::~DynamicFlag() = default;

bool DynamicFlag::is_enabled() const {
  return is_enabled_.load();
}

void DynamicFlag::OnEnableUpdate(bool is_enabled) {
  const bool was_enabled = is_enabled_.exchange(is_enabled);
  LOG_IF(WARNING, was_enabled != is_enabled)
      << "Flag `" << name_ << "` flipped to "
      << (is_enabled ? "enabled" : "disabled");
}
}  // namespace reporting