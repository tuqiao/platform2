// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/events/touchpad_events_impl.h"

#include <utility>

#include <base/bind.h>
#include <base/check.h>
#include <base/logging.h>

namespace {

namespace mojom = ::ash::cros_healthd::mojom;

}  // namespace

namespace diagnostics {

TouchpadEventsImpl::TouchpadEventsImpl(Context* context)
    : receiver_(this), context_(context) {
  DCHECK(context_);

  observers_.set_disconnect_handler(base::BindRepeating(
      &TouchpadEventsImpl::StopMonitor, base::Unretained(this)));
}

void TouchpadEventsImpl::AddObserver(
    mojo::PendingRemote<mojom::EventObserver> observer) {
  auto element_id = observers_.Add(std::move(observer));
  if (observers_.size() == 1) {
    context_->executor()->MonitorTouchpad(
        receiver_.BindNewPipeAndPassRemote(),
        process_control_.BindNewPipeAndPassReceiver());
    receiver_.set_disconnect_handler(
        base::BindOnce(&TouchpadEventsImpl::CleanUp, base::Unretained(this)));
  } else {
    if (cached_connected_event_) {
      observers_.Get(element_id)
          ->OnEvent(mojom::EventInfo::NewTouchpadEventInfo(
              cached_connected_event_.Clone()));
    }
  }
}

void TouchpadEventsImpl::OnButton(mojom::TouchpadButtonEventPtr button_event) {
  auto info = mojom::TouchpadEventInfo::NewButtonEvent(button_event.Clone());
  for (auto& observer : observers_)
    observer->OnEvent(mojom::EventInfo::NewTouchpadEventInfo(info.Clone()));
}

void TouchpadEventsImpl::OnTouch(mojom::TouchpadTouchEventPtr touch_event) {
  auto info = mojom::TouchpadEventInfo::NewTouchEvent(touch_event.Clone());
  for (auto& observer : observers_)
    observer->OnEvent(mojom::EventInfo::NewTouchpadEventInfo(info.Clone()));
}

void TouchpadEventsImpl::OnConnected(
    mojom::TouchpadConnectedEventPtr connected_event) {
  cached_connected_event_ =
      mojom::TouchpadEventInfo::NewConnectedEvent(connected_event.Clone());
  for (auto& observer : observers_)
    observer->OnEvent(mojom::EventInfo::NewTouchpadEventInfo(
        cached_connected_event_.Clone()));
}

void TouchpadEventsImpl::StopMonitor(mojo::RemoteSetElementId id) {
  if (observers_.empty()) {
    process_control_.reset();
    receiver_.reset();
  }
}

void TouchpadEventsImpl::CleanUp() {
  observers_.Clear();
  cached_connected_event_.reset();
}

}  // namespace diagnostics