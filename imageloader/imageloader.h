// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef IMAGELOADER_IMAGELOADER_H_
#define IMAGELOADER_IMAGELOADER_H_

#include <map>
#include <string>
#include <utility>

#include <base/files/file_path.h>
#include <dbus-c++/dbus.h>

#include "imageloader-glue.h"

namespace imageloader {

// This is a utility that handles mounting and unmounting of
// verified filesystem images that might include binaries intended
// to be run as read only.
class ImageLoader
    : org::chromium::ImageLoaderInterface_adaptor,
      public DBus::ObjectAdaptor {
 public:
  // Instantiate a D-Bus Helper Instance
  explicit ImageLoader(DBus::Connection* conn);

  // Register a component.
  bool RegisterComponent(const std::string& name, const std::string& version,
                         const std::string& fs_image_abs_path,
                         ::DBus::Error& err);

  // Get component version given component name.
  std::string GetComponentVersion(const std::string& name, ::DBus::Error& err);

  // Load the specified component.
  std::string LoadComponent(const std::string& name, ::DBus::Error& err);
  std::string LoadComponentUtil(const std::string& name);

  // Unload the specified component.
  bool UnloadComponent(const std::string& name, ::DBus::Error& err);
  bool UnloadComponentUtil(const std::string& name);
 private:
  // "mounts" keeps track of what has been mounted.
  // mounts = (name, (mount_point, device_path))
  std::map<std::string, std::pair<base::FilePath, base::FilePath>> mounts;
  // "reg" keeps track of registered components.
  // reg = (name, (version, fs_image_abs_path))
  std::map<std::string, std::pair<std::string, base::FilePath>> reg;
};

}  // namespace imageloader

#endif  // IMAGELOADER_IMAGELOADER_H_
