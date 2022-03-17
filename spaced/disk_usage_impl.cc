// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "spaced/disk_usage_impl.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/posix/eintr_wrapper.h>
#include <base/strings/string_util.h>
#include <rootdev/rootdev.h>

namespace spaced {
DiskUsageUtilImpl::DiskUsageUtilImpl()
    : lvm_(std::make_unique<brillo::LogicalVolumeManager>()) {
  auto thinpool = GetThinpool();
  if (thinpool) {
    thinpool_ = std::make_unique<brillo::Thinpool>(*thinpool);
  }
}

int DiskUsageUtilImpl::StatVFS(const base::FilePath& path, struct statvfs* st) {
  return HANDLE_EINTR(statvfs(path.value().c_str(), st));
}

std::optional<base::FilePath> DiskUsageUtilImpl::GetRootDevice() {
  // Get the root device.
  char root_device[PATH_MAX];
  int ret = rootdev(root_device, sizeof(root_device),
                    true,   // Do full resolution.
                    true);  // Remove partition number.
  if (ret != 0) {
    LOG(WARNING) << "rootdev failed with error code " << ret;
    return std::nullopt;
  }

  return base::FilePath(root_device);
}

std::optional<brillo::Thinpool> DiskUsageUtilImpl::GetThinpool() {
  std::optional<base::FilePath> root_device = GetRootDevice();

  if (!root_device) {
    LOG(WARNING) << "Failed to get root device";
    return std::nullopt;
  }

  // For some storage devices (eg. eMMC), the path ends in a digit
  // (eg. /dev/mmcblk0). Use 'p' as the partition separator while generating
  // the partition's block device path. For other types of paths (/dev/sda), we
  // directly append the partition number.
  std::string stateful_dev(root_device->value());
  if (base::IsAsciiDigit(stateful_dev[stateful_dev.size() - 1]))
    stateful_dev += 'p';
  stateful_dev += '1';

  // Attempt to check if the stateful partition is setup with a valid physical
  // volume.
  base::FilePath physical_volume(stateful_dev);

  std::optional<brillo::PhysicalVolume> pv =
      lvm_->GetPhysicalVolume(physical_volume);
  if (!pv || !pv->IsValid())
    return std::nullopt;

  std::optional<brillo::VolumeGroup> vg = lvm_->GetVolumeGroup(*pv);
  if (!vg || !vg->IsValid())
    return std::nullopt;

  return lvm_->GetThinpool(*vg, "thinpool");
}

int64_t DiskUsageUtilImpl::GetFreeDiskSpace(const base::FilePath& path) {
  // Use statvfs() to get the free space for the given path.
  struct statvfs stat;

  if (StatVFS(path, &stat) != 0) {
    LOG(ERROR) << "Failed to run statvfs() on " << path;
    return -1;
  }

  int64_t free_disk_space = static_cast<int64_t>(stat.f_bavail) * stat.f_frsize;

  int64_t thinpool_free_space;
  if (thinpool_ && thinpool_->IsValid() &&
      thinpool_->GetFreeSpace(&thinpool_free_space)) {
    free_disk_space = std::min(free_disk_space, thinpool_free_space);
  }

  return free_disk_space;
}

int64_t DiskUsageUtilImpl::GetTotalDiskSpace(const base::FilePath& path) {
  // Use statvfs() to get the total space for the given path.
  struct statvfs stat;

  if (StatVFS(path, &stat) != 0) {
    LOG(ERROR) << "Failed to run statvfs() on " << path;
    return -1;
  }

  int64_t total_disk_space =
      static_cast<int64_t>(stat.f_blocks) * stat.f_frsize;

  int64_t thinpool_total_space;
  if (thinpool_ && thinpool_->IsValid() &&
      thinpool_->GetTotalSpace(&thinpool_total_space)) {
    total_disk_space = std::min(total_disk_space, thinpool_total_space);
  }

  return total_disk_space;
}

int64_t DiskUsageUtilImpl::GetBlockDeviceSize(const base::FilePath& device) {
  base::ScopedFD fd(HANDLE_EINTR(
      open(device.value().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)));
  if (!fd.is_valid()) {
    PLOG(ERROR) << "open " << device.value();
    return -1;
  }

  int64_t size;
  if (ioctl(fd.get(), BLKGETSIZE64, &size)) {
    PLOG(ERROR) << "ioctl(BLKGETSIZE): " << device.value();
    return -1;
  }
  return size;
}

int64_t DiskUsageUtilImpl::GetRootDeviceSize() {
  std::optional<base::FilePath> root_device = GetRootDevice();

  if (!root_device) {
    LOG(WARNING) << "Failed to get root device";
    return -1;
  }

  return GetBlockDeviceSize(*root_device);
}

}  // namespace spaced
