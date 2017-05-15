// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hammerd/update_fw.h"

#include <fmap.h>

#include <algorithm>
#include <iomanip>
#include <utility>

#include <base/logging.h>
#include <base/memory/free_deleter.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/stringprintf.h>
#include <base/threading/platform_thread.h>
#include <base/time/time.h>
#include <vboot/vb21_struct.h>

namespace hammerd {

const char* ToString(SectionName name) {
  switch (name) {
    case SectionName::RO:
      return "RO";
    case SectionName::RW:
      return "RW";
    default:
      return "UNKNOWN_SECTION";
  }
}

SectionName OtherSection(SectionName name) {
  switch (name) {
    case SectionName::RO:
      return SectionName::RW;
    case SectionName::RW:
      return SectionName::RO;
    default:
      return SectionName::END;
  }
}

SectionInfo::SectionInfo(SectionName name)
    : SectionInfo(name, 0, 0, "", 0, 1) {}

SectionInfo::SectionInfo(SectionName name,
                         uint32_t offset,
                         uint32_t size,
                         const char* version_str,
                         int32_t rollback,
                         int32_t key_version)
    : name(name),
      offset(offset),
      size(size),
      rollback(rollback),
      key_version(key_version) {
  if (strlen(version_str) >= sizeof(version)) {
    LOG(ERROR) << "The version name is larger than the reserved size. "
               << "Discard the extra part.";
  }
  snprintf(version, sizeof(version), version_str);
}

bool operator==(const SectionInfo& lhs, const SectionInfo& rhs) {
  return lhs.name == rhs.name && lhs.offset == rhs.offset &&
         lhs.size == rhs.size &&
         strncmp(lhs.version, rhs.version, sizeof(lhs.version)) == 0 &&
         lhs.rollback == rhs.rollback && lhs.key_version == rhs.key_version;
}

bool operator!=(const SectionInfo& lhs, const SectionInfo& rhs) {
  return !(lhs == rhs);
}

FirmwareUpdater::FirmwareUpdater()
    : FirmwareUpdater(std::shared_ptr<UsbEndpoint>(new UsbEndpoint()),
                      std::shared_ptr<FmapInterface>(new Fmap())) {}

FirmwareUpdater::FirmwareUpdater(std::shared_ptr<UsbEndpoint> uep,
                                 std::shared_ptr<FmapInterface> fmap)
    : uep_(uep), fmap_(fmap), targ_(), image_(""), sections_() {}

FirmwareUpdater::~FirmwareUpdater() {}

bool FirmwareUpdater::TryConnectUSB() {
  const unsigned int kTimeoutMs = 1000;
  const unsigned int kIntervalMs = 100;

  LOG(INFO) << "Try to connect to USB endpoint.";
  auto start_time = base::Time::Now();
  int64_t duration = 0;
  while (true) {
    bool ret = uep_->Connect();
    if (ret) {
      return true;
    }

    duration = (base::Time::Now() - start_time).InMilliseconds();
    if (duration > kTimeoutMs) {
      break;
    }
    base::PlatformThread::Sleep(
        base::TimeDelta::FromMilliseconds(kIntervalMs));
  }
  LOG(ERROR) << "Failed to connect USB endpoint.";
  return false;
}

void FirmwareUpdater::CloseUSB() {
  uep_->Close();
}

bool FirmwareUpdater::LoadImage(const std::string& image) {
  image_.clear();
  sections_.clear();
  for (int idx = 0; idx < static_cast<int>(SectionName::END); idx++) {
    sections_.push_back(SectionInfo(static_cast<SectionName>(idx)));
  }
  uint8_t* image_ptr =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(image.data()));
  size_t len = image.size();

  int64_t offset = fmap_->Find(image_ptr, len);
  if (offset < 0) {
    LOG(ERROR) << "Cannot find FMAP in image";
    return false;
  }

  // TODO(akahuang): validate fmap struct more than this?
  fmap* fmap = reinterpret_cast<struct fmap*>(image_ptr + offset);
  if (fmap->size != len) {
    LOG(ERROR) << "Mismatch between FMAP size and image size";
    return false;
  }

  for (auto& section : sections_) {
    const char* fmap_name;
    const char* fmap_fwid_name;
    const char* fmap_rollback_name = nullptr;
    const char* fmap_key_name = nullptr;

    if (section.name == SectionName::RO) {
      fmap_name = "EC_RO";
      fmap_fwid_name = "RO_FRID";
    } else if (section.name == SectionName::RW) {
      fmap_name = "EC_RW";
      fmap_fwid_name = "RW_FWID";
      fmap_rollback_name = "RW_RBVER";
      // Key version comes from key RO (RW signature does not
      // contain the key version.
      fmap_key_name = "KEY_RO";
    } else {
      LOG(ERROR) << "Invalid section name";
      return false;
    }

    const fmap_area* fmaparea = fmap_->FindArea(fmap, fmap_name);
    if (!fmaparea) {
      LOG(ERROR) << "Cannot find FMAP area: " << fmap_name;
      return false;
    }
    section.offset = fmaparea->offset;
    section.size = fmaparea->size;

    fmaparea = fmap_->FindArea(fmap, fmap_fwid_name);
    if (!fmaparea) {
      LOG(ERROR) << "Cannot find FMAP area: " << fmap_fwid_name;
      return false;
    }
    if (fmaparea->size != sizeof(section.version)) {
      LOG(ERROR) << "Invalid fwid size\n";
      return false;
    }
    memcpy(section.version, image_ptr + fmaparea->offset, fmaparea->size);

    if (fmap_rollback_name &&
        (fmaparea = fmap_->FindArea(fmap, fmap_rollback_name))) {
      section.rollback =
          *(reinterpret_cast<const int32_t*>(image_ptr + fmaparea->offset));
    } else {
      section.rollback = -1;
    }

    if (fmap_key_name && (fmaparea = fmap_->FindArea(fmap, fmap_key_name))) {
      auto key = reinterpret_cast<const vb21_packed_key*>(image_ptr +
                                                          fmaparea->offset);
      section.key_version = key->key_version;
    } else {
      section.key_version = -1;
    }
  }

  image_ = image;
  LOG(INFO) << "Header versions:";
  for (const auto& section : sections_) {
    LOG(INFO) << base::StringPrintf(
        "%s offset=0x%08x/0x%08x version=%s rollback=%d key_version=%d",
        ToString(section.name),
        section.offset,
        section.size,
        section.version,
        section.rollback,
        section.key_version);
  }
  return true;
}

SectionName FirmwareUpdater::CurrentSection() const {
  SectionName writable_section;
  for (const auto& section : sections_) {
    if (targ_.offset == section.offset) {
      writable_section = section.name;
    }
  }
  return OtherSection(writable_section);
}

bool FirmwareUpdater::IsNeedUpdate(SectionName section_name) const {
  if (section_name == SectionName::RW) {
    auto section = sections_[static_cast<int>(section_name)];
    // TODO(akahuang): We might still want to update even the version is
    // identical. Add a flag if we have the request in the future.
    return (strncmp(targ_.version, section.version,
                    sizeof(targ_.version)) != 0 &&
            targ_.min_rollback <= section.rollback &&
            targ_.key_version == section.key_version);
  } else {
    // TODO(akahuang): Confirm the condition of RO update.
    return false;
  }
}

bool FirmwareUpdater::IsSectionLocked(SectionName section_name) const {
  // TODO(akahuang): Implement this.
  return false;
}

bool FirmwareUpdater::UnLockSection(SectionName section_name) {
  // TODO(akahuang): Implement this.
  return false;
}

bool FirmwareUpdater::TransferImage(SectionName section_name) {
  if (!SendFirstPDU()) {
    LOG(ERROR) << "Failed to send the first PDU.";
    return false;
  }
  // Determine if the section need to update.
  bool ret = false;
  const uint8_t* image_ptr = reinterpret_cast<const uint8_t*>(image_.data());
  auto section = sections_[static_cast<int>(section_name)];
  LOG(INFO) << "Section to be updated: " << section.name;
  if (section.offset + section.size > image_.size()) {
    LOG(ERROR) << "image length (" << image_.size()
               << ") is smaller than transfer requirements: " << section.offset
               << " + " << section.size;
    return false;
  }
  ret =
      TransferSection(image_ptr + section.offset, section.offset, section.size);

  // Move USB receiver state machine to idle state so that vendor commands can
  // be processed later, if any.
  SendDone();
  return ret;
}

bool FirmwareUpdater::SendSubcommand(UpdateExtraCommand subcommand) {
  LOG(INFO) << "Send Sub-command: " << subcommand;
  SendDone();

  uint8_t response = -1;
  uint16_t subcommand_value = static_cast<uint16_t>(subcommand);
  size_t usb_msg_size = (sizeof(UpdateFrameHeader) + sizeof(subcommand_value));
  std::unique_ptr<UpdateFrameHeader, base::FreeDeleter> ufh(
      static_cast<UpdateFrameHeader*>(malloc(usb_msg_size)));
  if (ufh == nullptr) {
    LOG(ERROR) << "Failed to allocate " << usb_msg_size << " bytes";
    return false;
  }
  ufh->block_size = htobe32(usb_msg_size);
  ufh->block_base = htobe32(kUpdateExtraCmd);
  ufh->block_digest = 0;
  uint16_t* frame_ptr = reinterpret_cast<uint16_t*>(ufh.get() + 1);
  *frame_ptr = htobe16(subcommand_value);

  bool ret;
  if (subcommand == UpdateExtraCommand::kImmediateReset) {
    // When sending reset command, we won't get the response. Therefore just
    // check the Send action is successful.
    int received = uep_->Send(ufh.get(), usb_msg_size, false);
    ret = (received == usb_msg_size);
  } else {
    int received = uep_->Transfer(
        ufh.get(), usb_msg_size, &response, sizeof(response), false);
    ret = (received == sizeof(response));
  }
  if (ret) {
    LOG(INFO) << "Sent sub-command: " << std::hex << subcommand << std::dec
              << ", response: " << base::HexEncode(&response, sizeof(response));
  }
  return ret;
}

bool FirmwareUpdater::SendFirstPDU() {
  LOG(INFO) << "Send the first PDU: zero data header.";
  UpdateFrameHeader ufh;
  memset(&ufh, 0, sizeof(ufh));
  ufh.block_size = htobe32(sizeof(ufh));
  if (uep_->Send(&ufh, sizeof(ufh)) != sizeof(ufh)) {
    LOG(ERROR) << "Send first update frame header failed.";
    return false;
  }

  // We got something. Check for errors in response.
  FirstResponsePDU rpdu;
  size_t rxed_size = uep_->Receive(&rpdu, sizeof(rpdu), true);
  const size_t kMinimumResponseSize = 8;
  if (rxed_size < kMinimumResponseSize) {
    LOG(ERROR) << "Unexpected response size: " << rxed_size
               << ". Response content: "
               << base::HexEncode(reinterpret_cast<uint8_t*>(&rpdu), rxed_size);
    return false;
  }

  // Convert endian of the response.
  uint32_t return_value = be32toh(rpdu.return_value);
  targ_.header_type = be16toh(rpdu.header_type);
  targ_.protocol_version = be16toh(rpdu.protocol_version);
  targ_.maximum_pdu_size = be32toh(rpdu.maximum_pdu_size);
  targ_.flash_protection = be32toh(rpdu.flash_protection);
  targ_.offset = be32toh(rpdu.offset);
  memcpy(targ_.version, rpdu.version, sizeof(rpdu.version));
  targ_.min_rollback = be32toh(rpdu.min_rollback);
  targ_.key_version = be32toh(rpdu.key_version);

  LOG(INFO) << "target running protocol version " << targ_.protocol_version
            << " (type " << targ_.header_type << ")";
  if (targ_.protocol_version != 6) {
    LOG(ERROR) << "Unsupported protocol version " << targ_.protocol_version;
    return false;
  }
  if (targ_.header_type !=
      static_cast<int>(FirstResponsePDUHeaderType::kCommon)) {
    LOG(ERROR) << "Unsupported header type " << targ_.header_type;
    return false;
  }
  if (return_value) {
    LOG(ERROR) << "Target reporting error " << return_value;
    return false;
  }

  LOG(INFO) << "Response of the first PDU:";
  LOG(INFO) << base::StringPrintf(
      "Maximum PDU size: %d, Flash protection: %04x, Version: %s, "
      "Key version: %d, Minimum rollback: %d, Writeable at offset: 0x%x",
      targ_.maximum_pdu_size,
      targ_.flash_protection,
      targ_.version,
      targ_.key_version,
      targ_.min_rollback,
      targ_.offset);
  LOG(INFO) << "SendFirstPDU finished successfully.";
  return true;
}

void FirmwareUpdater::SendDone() {
  // Send stop request, ignoring reply.
  uint32_t out = htobe32(kUpdateDoneCmd);
  uint8_t unused_received;
  uep_->Transfer(&out, sizeof(out), &unused_received, 1, false);
}

bool FirmwareUpdater::TransferSection(const uint8_t* data_ptr,
                                      uint32_t section_addr,
                                      size_t data_len) {
  // Actually, we can skip trailing chunks of 0xff, as the entire
  // section space must be erased before the update is attempted.
  // TODO(akahuang): skip blocks within the image.
  while (data_len && (data_ptr[data_len - 1] == 0xff))
    data_len--;

  LOG(INFO) << "Sending 0x" << std::hex << data_len << " bytes to 0x"
            << section_addr << std::dec;
  while (data_len > 0) {
    // prepare the header to prepend to the block.
    size_t payload_size = std::min<size_t>(data_len, targ_.maximum_pdu_size);
    UpdateFrameHeader ufh;
    ufh.block_size = htobe32(payload_size + sizeof(UpdateFrameHeader));
    ufh.block_base = htobe32(section_addr);
    ufh.block_digest = 0;
    LOG(INFO) << "Update frame header: " << std::hex << "0x" << ufh.block_size
              << " "
              << "0x" << ufh.block_base << " "
              << "0x" << ufh.block_digest << std::dec;
    if (!TransferBlock(&ufh, data_ptr, payload_size)) {
      LOG(ERROR) << "Failed to transfer block, " << data_len << " to go";
      return false;
    }
    data_len -= payload_size;
    data_ptr += payload_size;
    section_addr += payload_size;
  }
  return true;
}

bool FirmwareUpdater::TransferBlock(UpdateFrameHeader* ufh,
                                    const uint8_t* transfer_data_ptr,
                                    size_t payload_size) {
  // First send the header.
  LOG(INFO) << "Send the block header: "
            << base::HexEncode(reinterpret_cast<uint8_t*>(ufh), sizeof(*ufh));
  uep_->Send(ufh, sizeof(*ufh));

  // Now send the block, chunk by chunk.
  size_t transfer_size = 0;
  while (transfer_size < payload_size) {
    int chunk_size =
        std::min<size_t>(uep_->GetChunkLength(), payload_size - transfer_size);
    uep_->Send(transfer_data_ptr, chunk_size);
    transfer_data_ptr += chunk_size;
    transfer_size += chunk_size;
    DLOG(INFO) << "Send block data " << transfer_size << "/" << payload_size;
  }

  // Now get the reply.
  uint32_t reply;
  if (uep_->Receive(&reply, sizeof(reply), true) == -1) {
    return false;
  }
  reply = *(reinterpret_cast<uint8_t*>(&reply));
  if (reply) {
    LOG(ERROR) << "Error: status " << reply;
    return false;
  }
  return true;
}

}  // namespace hammerd
