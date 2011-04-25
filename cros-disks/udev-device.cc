// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <base/string_number_conversions.h>
#include <base/string_util.h>
#include <fcntl.h>
#include <libudev.h>
#include <sys/statvfs.h>
#include <fstream>

#include "udev-device.h"

namespace cros_disks {

UdevDevice::UdevDevice(struct udev_device *dev)
    : dev_(dev) {

  CHECK(dev_) << "Invalid udev device";
  udev_device_ref(dev_);
}

UdevDevice::~UdevDevice() {
  udev_device_unref(dev_);
}

bool UdevDevice::IsValueBooleanTrue(const char *value) const {
  return value && strcmp(value, "1") == 0;
}

std::string UdevDevice::GetAttribute(const char *key) const {
  const char *value = udev_device_get_sysattr_value(dev_, key);
  return (value) ? value : "";
}

bool UdevDevice::IsAttributeTrue(const char *key) const {
  const char *value = udev_device_get_sysattr_value(dev_, key);
  return IsValueBooleanTrue(value);
}

bool UdevDevice::HasAttribute(const char *key) const {
  const char *value = udev_device_get_sysattr_value(dev_, key);
  return value != NULL;
}

std::string UdevDevice::GetProperty(const char *key) const {
  const char *value = udev_device_get_property_value(dev_, key);
  return (value) ? value : "";
}

bool UdevDevice::IsPropertyTrue(const char *key) const {
  const char *value = udev_device_get_property_value(dev_, key);
  return IsValueBooleanTrue(value);
}

bool UdevDevice::HasProperty(const char *key) const {
  const char *value = udev_device_get_property_value(dev_, key);
  return value != NULL;
}

void UdevDevice::GetSizeInfo(uint64 *total_size, uint64 *remaining_size) const {
  static const int kSectorSize = 512;
  uint64 total = 0, remaining = 0;

  // If the device is mounted, obtain the total and remaining size in bytes
  // using statvfs.
  std::vector<std::string> mount_paths = GetMountPaths();
  if (!mount_paths.empty()) {
    struct statvfs stat;
    if (statvfs(mount_paths[0].c_str(), &stat) == 0) {
      total = stat.f_blocks * stat.f_frsize;
      remaining = stat.f_bfree * stat.f_frsize;
    }
  }

  // If the UDISKS_PARTITION_SIZE property is set, use it as the total size
  // instead. If the UDISKS_PARTITION_SIZE property is not set but sysfs
  // provides a size value, which is the actual size in bytes divided by 512,
  // use that as the total size instead.
  const char *partition_size = udev_device_get_property_value(dev_,
      "UDISKS_PARTITION_SIZE");
  int64 size = 0;
  if (partition_size) {
    base::StringToInt64(partition_size, &size);
    total = size;
  } else {
    const char *size_attr = udev_device_get_sysattr_value(dev_, "size");
    if (size_attr) {
      base::StringToInt64(size_attr, &size);
      total = size * kSectorSize;
    }
  }

  if (total_size)
    *total_size = total;
  if (remaining_size)
    *remaining_size = remaining;
}

bool UdevDevice::IsMediaAvailable() const {
  bool is_media_available = true;
  if (IsAttributeTrue("removable")) {
    if (IsPropertyTrue("ID_CDROM")) {
      is_media_available = IsPropertyTrue("ID_CDROM_MEDIA");
    } else {
      const char *dev_file = udev_device_get_devnode(dev_);
      if (dev_file) {
        int fd = open(dev_file, O_RDONLY);
        if (fd < 0) {
          is_media_available = false;
        } else {
          close(fd);
        }
      }
    }
  }
  return is_media_available;
}

std::vector<std::string> UdevDevice::GetMountPaths() const {
  const char *device_path = udev_device_get_devnode(dev_);
  if (device_path) {
    return GetMountPaths(device_path);
  }
  return std::vector<std::string>();
}

std::vector<std::string> UdevDevice::GetMountPaths(
    const std::string& device_path) {
  std::ifstream fs("/proc/mounts");
  if (fs.is_open()) {
    return ParseMountPaths(device_path, fs);
  }
  LOG(ERROR) << "Unable to parse /proc/mounts";
  return std::vector<std::string>();
}

std::vector<std::string> UdevDevice::ParseMountPaths(
    const std::string& device_path, std::istream& stream) {
  std::vector<std::string> mount_paths;
  std::string line;
  while (std::getline(stream, line)) {
    std::vector<std::string> tokens;
    SplitString(line, ' ', &tokens);
    if (tokens.size() >= 2) {
      if (tokens[0] == device_path)
        mount_paths.push_back(tokens[1]);
    }
  }
  return mount_paths;
}

Disk UdevDevice::ToDisk() const {
  Disk disk;

  disk.set_is_read_only(IsAttributeTrue("ro"));
  disk.set_is_drive(HasAttribute("range"));
  disk.set_is_rotational(HasProperty("ID_ATA_ROTATION_RATE_RPM"));
  disk.set_is_optical_disk(IsPropertyTrue("ID_CDROM"));
  disk.set_is_hidden(IsPropertyTrue("UDISKS_PRESENTATION_HIDE"));
  disk.set_is_media_available(IsMediaAvailable());
  disk.set_drive_model(GetProperty("ID_MODEL"));
  disk.set_label(GetProperty("ID_FS_LABEL"));
  const char *sys_path = udev_device_get_syspath(dev_);
  if (sys_path)
    disk.set_native_path(sys_path);

  const char *dev_file = udev_device_get_devnode(dev_);
  if (dev_file)
    disk.set_device_file(dev_file);

  std::vector<std::string> mount_paths = GetMountPaths();
  disk.set_is_mounted(!mount_paths.empty());
  disk.set_mount_paths(mount_paths);

  uint64 total_size, remaining_size;
  GetSizeInfo(&total_size, &remaining_size);
  disk.set_device_capacity(total_size);
  disk.set_bytes_remaining(remaining_size);

  return disk;
}

}  // namespace cros_disks
