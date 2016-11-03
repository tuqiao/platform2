// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Container configuration from the config.json data as specified in
// https://github.com/opencontainers/runtime-spec/tree/v1.0.0-rc2

#ifndef CONTAINER_UTILS_CONTAINER_OCI_CONFIG_H_
#define CONTAINER_UTILS_CONTAINER_OCI_CONFIG_H_

struct OciPlatform {
  std::string os;
  std::string arch;
};

struct OciProcessUser {
  uint32_t uid;
  uint32_t gid;
  std::vector<uint32_t> additionalGids;  // Optional
};

struct OciProcess {
  bool terminal;  // Optional
  OciProcessUser user;
  std::vector<std::string> args;
  std::vector<std::string> env;  // Optional
  std::string cwd;
  // Unused: capabilities, rlimits, apparmorProfile,
  //    selinuxLabel, noNewPrivileges
};

struct OciRoot {
  std::string path;
  bool readonly;  // Optional
};

struct OciMount {
  std::string destination;
  std::string type;
  std::string source;
  std::vector<std::string> options;  // Optional
};

struct OciLinuxNamespaceMapping {
  uint32_t hostID;
  uint32_t containerID;
  uint32_t size;
};

struct OciLinuxDevice {
  std::string type;
  std::string path;
  uint32_t major;  // Optional
  uint32_t minor;  // Optional
  uint32_t fileMode;  // Optional
  uint32_t uid;  // Optional
  uint32_t gid;  // Optional
};

struct OciLinux {
  std::vector<OciLinuxDevice> devices;  // Optional
  std::string cgroupsPath;  // Optional
  // Unused: resources, namespace
  std::vector<OciLinuxNamespaceMapping> uidMappings;  // Optional
  std::vector<OciLinuxNamespaceMapping> gidMappings;  // Optional
  // TODO seccomp
  // Unused: maskedPaths, readonlyPaths, rootfsPropagation, mountLabel, sysctl
};

struct OciConfig {
  std::string ociVersion;
  OciPlatform platform;
  OciRoot root;
  OciProcess process;
  std::string hostname;  // Optional
  std::vector<OciMount> mounts;  // Optional
  // json field name - linux
  OciLinux linux_config;  // Optional
  // Unused: hooks, annotations
};

#endif  // CONTAINER_UTILS_CONTAINER_OCI_CONFIG_H_
