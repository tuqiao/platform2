// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cecservice/cec_device.h"

#include <fcntl.h>
#include <linux/cec-funcs.h>
#include <poll.h>
#include <string.h>

#include <utility>

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>

namespace cecservice {

namespace {
struct cec_msg CreateMessage(uint16_t source_address,
                             uint16_t destination_address) {
  struct cec_msg message;
  if (source_address == CEC_LOG_ADDR_INVALID) {
    cec_msg_init(&message, CEC_LOG_ADDR_UNREGISTERED, destination_address);
  } else {
    cec_msg_init(&message, source_address, destination_address);
  }
  return message;
}

TvPowerStatus GetPowerStatus(const struct cec_msg& msg) {
  uint8_t power_status;
  cec_ops_report_power_status(&msg, &power_status);
  switch (power_status) {
    case CEC_OP_POWER_STATUS_ON:
      return kTvPowerStatusOn;
    case CEC_OP_POWER_STATUS_STANDBY:
      return kTvPowerStatusStandBy;
    case CEC_OP_POWER_STATUS_TO_ON:
      return kTvPowerStatusToOn;
    case CEC_OP_POWER_STATUS_TO_STANDBY:
      return kTvPowerStatusToStandBy;
    default:
      LOG(WARNING) << "Unknown power status:" << power_status << " received.";
      return kTvPowerStatusUnknown;
  }
}
}  // namespace

CecDeviceImpl::CecDeviceImpl(std::unique_ptr<CecFd> fd) : fd_(std::move(fd)) {}

CecDeviceImpl::~CecDeviceImpl() = default;

bool CecDeviceImpl::Init() {
  if (!fd_->SetEventCallback(
          base::Bind(&CecDeviceImpl::OnFdEvent, weak_factory_.GetWeakPtr()))) {
    DisableDevice();
    return false;
  }
  return true;
}

void CecDeviceImpl::GetTvPowerStatus(GetTvPowerStatusCallback callback) {
  LOG(INFO) << "Getting power status.";
  if (!fd_) {
    LOG(WARNING) << "Device is disabled due to errors, unable to query.";
    std::move(callback).Run(kTvPowerStatusError);
    return;
  }

  if (GetState() == State::kStart) {
    LOG(INFO) << "Not configured, not querying TV power state.";
    std::move(callback).Run(kTvPowerStatusAdapterNotConfigured);
    return;
  }

  struct cec_msg message = CreateMessage(logical_address_, CEC_LOG_ADDR_TV);
  cec_msg_give_device_power_status(&message, 1);
  message_queue_.push_back(message);

  requests_.push_back({std::move(callback), 0});

  RequestWriteWatch();
}

void CecDeviceImpl::SetStandBy() {
  if (!fd_) {
    LOG(WARNING) << "Device in disabled due to previous errors, ignoring "
                    "standby request.";
    return;
  }

  if (GetState() == State::kStart) {
    LOG(INFO) << "Ignoring standby request, we are not connected.";
    return;
  }

  active_source_ = false;

  struct cec_msg message = CreateMessage(logical_address_, CEC_LOG_ADDR_TV);
  cec_msg_standby(&message);
  message_queue_.push_back(message);

  RequestWriteWatch();
}

void CecDeviceImpl::SetWakeUp() {
  if (!fd_) {
    LOG(WARNING) << "Device in disabled due to previous errors, ignoring wake "
                    "up request.";
    return;
  }

  struct cec_msg image_view_on_message =
      CreateMessage(logical_address_, CEC_LOG_ADDR_TV);
  cec_msg_image_view_on(&image_view_on_message);

  switch (GetState()) {
    case State::kReady: {
      message_queue_.push_back(image_view_on_message);

      struct cec_msg active_source_message =
          CreateMessage(logical_address_, CEC_LOG_ADDR_BROADCAST);
      cec_msg_active_source(&active_source_message, physical_address_);
      message_queue_.push_back(active_source_message);
    } break;
    case State::kStart:
      if (SendMessage(&image_view_on_message) == CecFd::TransmitResult::kOk) {
        pending_active_source_broadcast_ = true;
      } else {
        LOG(WARNING) << "Failed to send image view on message while in start "
                        "state, we are not able to wake up this TV.";
        return;
      }
      break;
    case State::kNoLogicalAddress:
      message_queue_.push_back(image_view_on_message);
      pending_active_source_broadcast_ = true;
      break;
  }
  active_source_ = true;

  RequestWriteWatch();
}

void CecDeviceImpl::RequestWriteWatch() {
  if (message_queue_.empty()) {
    return;
  }

  if (!fd_->WriteWatch()) {
    LOG(ERROR) << "Failed to request write watch on fd, disabling device.";
    DisableDevice();
  }
}

CecDeviceImpl::State CecDeviceImpl::GetState() const {
  if (physical_address_ == CEC_PHYS_ADDR_INVALID) {
    return State::kStart;
  }
  if (logical_address_ == CEC_LOG_ADDR_INVALID) {
    return State::kNoLogicalAddress;
  }
  return State::kReady;
}

CecDeviceImpl::State CecDeviceImpl::UpdateState(
    const struct cec_event_state_change& event) {
  physical_address_ = event.phys_addr;
  logical_address_ = CEC_LOG_ADDR_INVALID;

  if (event.log_addr_mask) {
    logical_address_ = ffs(event.log_addr_mask) - 1;
  }

  VLOG(1) << base::StringPrintf(
      "State update, physical address: 0x%x logical address: 0x%x",
      static_cast<uint32_t>(physical_address_),
      static_cast<uint32_t>(logical_address_));

  return GetState();
}

bool CecDeviceImpl::ProcessMessagesLostEvent(
    const struct cec_event_lost_msgs& event) {
  LOG(WARNING) << "Received event lost message, lost " << event.lost_msgs
               << " messages";
  return true;
}

bool CecDeviceImpl::ProcessStateChangeEvent(
    const struct cec_event_state_change& event) {
  switch (UpdateState(event)) {
    case State::kNoLogicalAddress:
      return SetLogicalAddress();
    case State::kStart:
      RespondToAllPendingQueries(kTvPowerStatusAdapterNotConfigured);
      message_queue_.clear();
      return true;
    case State::kReady:
      if (pending_active_source_broadcast_) {
        struct cec_msg message =
            CreateMessage(logical_address_, CEC_LOG_ADDR_BROADCAST);
        cec_msg_active_source(&message, physical_address_);
        message_queue_.push_back(message);

        pending_active_source_broadcast_ = false;
      }
      return true;
  }
}

bool CecDeviceImpl::ProcessEvents() {
  struct cec_event event;

  if (!fd_->ReceiveEvent(&event)) {
    return false;
  }

  switch (event.event) {
    case CEC_EVENT_LOST_MSGS:
      return ProcessMessagesLostEvent(event.lost_msgs);
      break;
    case CEC_EVENT_STATE_CHANGE:
      return ProcessStateChangeEvent(event.state_change);
    default:
      LOG(WARNING) << base::StringPrintf("Unexpected cec event type: 0x%x",
                                         event.event);
      return true;
  }
}

bool CecDeviceImpl::ProcessRead() {
  struct cec_msg msg;
  if (!fd_->ReceiveMessage(&msg)) {
    return false;
  }

  if (msg.sequence) {
    ProcessSentMessage(msg);
  } else {
    ProcessIncomingMessage(&msg);
  }
  return true;
}

bool CecDeviceImpl::ProcessWrite() {
  if (message_queue_.empty()) {
    return true;
  }

  struct cec_msg message = message_queue_.front();
  const CecFd::TransmitResult ret = SendMessage(&message);
  if (ret == CecFd::TransmitResult::kWouldBlock) {
    return true;
  }

  if (cec_msg_opcode(&message) == CEC_MSG_GIVE_DEVICE_POWER_STATUS) {
    auto iterator = std::find_if(requests_.begin(), requests_.end(),
                                 [](const RequestInFlight& request) {
                                   return request.sequence_id == 0;
                                 });
    CHECK(iterator != requests_.end());

    if (ret == CecFd::TransmitResult::kOk) {
      iterator->sequence_id = message.sequence;
    } else {
      std::move(iterator->callback).Run(kTvPowerStatusError);
      requests_.erase(iterator);
    }
  }

  message_queue_.pop_front();
  return ret != CecFd::TransmitResult::kError;
}

bool CecDeviceImpl::ProcessPowerStatusResponse(const struct cec_msg& msg) {
  auto iterator = std::find_if(requests_.begin(), requests_.end(),
                               [&](const RequestInFlight& request) {
                                 return request.sequence_id == msg.sequence;
                               });
  if (iterator == requests_.end()) {
    return false;
  }

  TvPowerStatus status;
  if (cec_msg_status_is_ok(&msg)) {
    status = GetPowerStatus(msg);
  } else if (msg.tx_status & CEC_TX_STATUS_NACK) {
    status = kTvPowerStatusNoTv;
  } else {
    status = kTvPowerStatusError;
  }

  std::move(iterator->callback).Run(status);
  requests_.erase(iterator);

  return true;
}

void CecDeviceImpl::ProcessSentMessage(const struct cec_msg& msg) {
  if (ProcessPowerStatusResponse(msg)) {
    return;
  }

  if (cec_msg_status_is_ok(&msg)) {
    VLOG(1) << base::StringPrintf("Successfully sent message, opcode: 0x%x",
                                  cec_msg_opcode(&msg));
  } else {
    LOG(WARNING) << base::StringPrintf(
        "Failed to send message, opcode: 0x%x tx_status: 0x%x",
        cec_msg_opcode(&msg), static_cast<uint32_t>(msg.tx_status));
  }
}

void CecDeviceImpl::ProcessIncomingMessage(struct cec_msg* msg) {
  struct cec_msg reply;
  switch (cec_msg_opcode(msg)) {
    case CEC_MSG_REQUEST_ACTIVE_SOURCE:
      VLOG(1) << "Received active source request.";
      if (active_source_) {
        VLOG(1) << "We are active source, will respond.";
        cec_msg_init(&reply, logical_address_, CEC_LOG_ADDR_BROADCAST);
        cec_msg_active_source(&reply, physical_address_);
        message_queue_.push_back(std::move(reply));
      }
      break;
    case CEC_MSG_ACTIVE_SOURCE:
      VLOG(1) << "Received active source message.";
      if (active_source_) {
        VLOG(1) << "We ceased to be active source.";
        active_source_ = false;
      }
      break;
    case CEC_MSG_GIVE_DEVICE_POWER_STATUS:
      VLOG(1) << "Received give power status message.";
      cec_msg_init(&reply, logical_address_, cec_msg_initiator(msg));
      cec_msg_report_power_status(&reply, CEC_OP_POWER_STATUS_ON);
      message_queue_.push_back(reply);
      break;
    default:
      VLOG(1) << base::StringPrintf("Received message, opcode: 0x%x",
                                    cec_msg_opcode(msg));
      if (!cec_msg_is_broadcast(msg)) {
        VLOG(1) << "Responding with feature abort.";
        cec_msg_reply_feature_abort(msg, CEC_OP_ABORT_UNRECOGNIZED_OP);
        message_queue_.push_back(std::move(*msg));
      } else {
        VLOG(1) << "Ignoring broadcast message.";
      }
      break;
  }
}

CecFd::TransmitResult CecDeviceImpl::SendMessage(struct cec_msg* msg) {
  VLOG(1) << base::StringPrintf("Transmitting message, opcode:0x%x",
                                cec_msg_opcode(msg));
  return fd_->TransmitMessage(msg);
}

bool CecDeviceImpl::SetLogicalAddress() {
  struct cec_log_addrs addresses = {};

  if (!fd_->GetLogicalAddresses(&addresses)) {
    return false;
  }

  // The address has already been set, so we will reuse it.
  if (addresses.num_log_addrs) {
    return true;
  }

  memset(&addresses, 0, sizeof(addresses));
  addresses.cec_version = CEC_OP_CEC_VERSION_1_4;
  addresses.vendor_id = CEC_VENDOR_ID_NONE;
  addresses.osd_name[0] = 0;
  addresses.num_log_addrs = 1;
  addresses.log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
  addresses.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
  addresses.all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
  addresses.flags = CEC_LOG_ADDRS_FL_ALLOW_UNREG_FALLBACK;

  return fd_->SetLogicalAddresses(&addresses);
}

void CecDeviceImpl::OnFdEvent(CecFd::EventType event) {
  if (!fd_) {
    return;
  }

  bool ret;
  switch (event) {
    case CecFd::EventType::kPriorityRead:
      ret = ProcessEvents();
      break;
    case CecFd::EventType::kRead:
      ret = ProcessRead();
      break;
    case CecFd::EventType::kWrite:
      ret = ProcessWrite();
      break;
  }

  if (!ret) {
    DisableDevice();
    return;
  }

  RequestWriteWatch();
}

void CecDeviceImpl::RespondToAllPendingQueries(TvPowerStatus response) {
  std::list<RequestInFlight> requests;
  requests.swap(requests_);

  for (auto& request : requests) {
    std::move(request.callback).Run(response);
  }
}

void CecDeviceImpl::DisableDevice() {
  fd_.reset();

  RespondToAllPendingQueries(kTvPowerStatusError);
}

CecDeviceFactoryImpl::CecDeviceFactoryImpl(const CecFdOpener* cec_fd_opener)
    : cec_fd_opener_(cec_fd_opener) {}

CecDeviceFactoryImpl::~CecDeviceFactoryImpl() = default;

std::unique_ptr<CecDevice> CecDeviceFactoryImpl::Create(
    const base::FilePath& path) const {
  std::unique_ptr<CecFd> fd = cec_fd_opener_->Open(path, O_NONBLOCK);
  if (!fd) {
    return nullptr;
  }

  struct cec_caps caps;
  if (!fd->GetCapabilities(&caps)) {
    return nullptr;
  }

  LOG(INFO) << base::StringPrintf("CEC adapter driver:%s name:%s caps:0x%x",
                                  caps.driver, caps.name, caps.capabilities);

  // At the moment the only adapters supported are the ones that:
  // - handle configuration of physical address on their own (i.e. don't have
  // CEC_CAP_PHYS_ADDR flag set)
  // - allow us to configure logical addrresses (i.e. have CEC_CAP_LOG_ADDRS
  // set)
  if ((caps.capabilities & CEC_CAP_PHYS_ADDR) ||
      !(caps.capabilities & CEC_CAP_LOG_ADDRS)) {
    LOG(WARNING) << "Device does not have required capabilities to function "
                    "with this service";
    return nullptr;
  }

  uint32_t mode = CEC_MODE_EXCL_INITIATOR;
  if (!fd->SetMode(mode)) {
    LOG(ERROR) << "Failed to set an exclusive initiator mode on the device";
    return nullptr;
  }

  auto device = std::make_unique<CecDeviceImpl>(std::move(fd));
  if (!device->Init()) {
    return nullptr;
  }

  return device;
}

}  // namespace cecservice
