// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_SYSTEM_INPUT_H_
#define POWER_MANAGER_POWERD_SYSTEM_INPUT_H_

#include <map>
#include <string>
#include <vector>

#include <base/callback.h>
#include <base/cancelable_callback.h>
#include <base/files/file_path.h>
#include <base/macros.h>
#include <base/memory/linked_ptr.h>
#include <base/memory/scoped_ptr.h>
#include <base/message_loop/message_loop.h>
#include <base/observer_list.h>

#include "power_manager/common/power_constants.h"
#include "power_manager/powerd/system/input_interface.h"
#include "power_manager/powerd/system/udev_subsystem_observer.h"

struct input_event;  // from <linux/input.h>

namespace power_manager {

class PrefsInterface;

namespace system {

class InputObserver;
class UdevInterface;

class Input : public InputInterface,
              public base::MessageLoopForIO::Watcher,
              public UdevSubsystemObserver {
 public:
  // udev subsystem to watch for input device-related events.
  static const char kInputUdevSubsystem[];

  Input();
  virtual ~Input();

  void set_sysfs_input_path_for_testing(const base::FilePath& path) {
    sysfs_input_path_for_testing_ = path;
  }

  // Returns true on success.
  bool Init(PrefsInterface* prefs, UdevInterface* udev);

  // InputInterface implementation:
  void AddObserver(InputObserver* observer) override;
  void RemoveObserver(InputObserver* observer) override;
  LidState QueryLidState() override;
  bool IsUSBInputDeviceConnected() const override;
  int GetActiveVT() override;

  // base::MessageLoopForIO::Watcher implementation:
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override;

  // UdevSubsystemObserver implementation:
  void OnUdevEvent(const std::string& subsystem,
                   const std::string& sysname,
                   UdevAction action) override;

 private:
  class EventFileDescriptor;

  // For every "event" in /dev/input/, open a file handle, and
  // RegisterInputEvent on it if the event contains power buttons or lid.
  bool RegisterInputDevices();

  // Adds or removes events to handle lid and power button.
  bool AddEvent(const std::string& name);
  bool RemoveEvent(const std::string& name);

  // Starts watching |fd| for events if it corresponds to a power button or lid
  // switch. Takes ownership of |fd| and returns true if the descriptor is now
  // watched; the caller is responsible for closing |fd| otherwise.
  bool RegisterInputEvent(int fd, int event_num);

  // Does a non-blocking read on |fd| and copies input events to |events_out|
  // (after clearing it). Returns true if the read was successful and events
  // were present.
  bool ReadEvents(int fd, std::vector<input_event>* events_out);

  // Calls NotifyObserversAboutEvent() for each event in |queued_events_| and
  // clears the vector.
  void SendQueuedEvents();

  // Notifies observers about |event| if came from a lid switch or power button.
  void NotifyObserversAboutEvent(const input_event& event);

  // File descriptor corresponding to the lid switch. The EventFileDescriptor
  // in |registered_inputs_| handles closing this FD; it's stored separately so
  // it can be queried directly for the lid state.
  int lid_fd_;

  // TODO(derat): Calling these "events" is confusing; rename to something like
  // "devices".
  int num_power_key_events_;
  int num_lid_events_;

  // Should the lid be watched for events if present?
  bool use_lid_;

  // Most-recently-seen lid state.
  LidState lid_state_;

  // Events read from |lid_fd_| by QueryLidState() that haven't yet been sent to
  // observers.
  std::vector<input_event> queued_events_;

  // Posted by QueryLidState() to run SendQueuedEvents() to notify observers
  // about |queued_events_|.
  base::CancelableClosure send_queued_events_task_;

  // Name of the power button interface to skip monitoring.
  const char* power_button_to_skip_;

  // Used to make ioctls to /dev/console to check which VT is active.
  int console_fd_;

  UdevInterface* udev_;  // non-owned

  // Keyed by input event number.
  typedef std::map<int, linked_ptr<EventFileDescriptor> > InputMap;
  InputMap registered_inputs_;

  ObserverList<InputObserver> observers_;

  // Used by IsUSBInputDeviceConnected() instead of the default path if
  // non-empty.
  base::FilePath sysfs_input_path_for_testing_;

  DISALLOW_COPY_AND_ASSIGN(Input);
};

}  // namespace system
}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_SYSTEM_INPUT_H_
