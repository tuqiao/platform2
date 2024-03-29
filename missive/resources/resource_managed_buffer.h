// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MISSIVE_RESOURCES_RESOURCE_MANAGED_BUFFER_H_
#define MISSIVE_RESOURCES_RESOURCE_MANAGED_BUFFER_H_

#include <memory>

#include <base/memory/scoped_refptr.h>

#include "missive/resources/resource_manager.h"
#include "missive/util/status.h"

namespace reporting {

// Helper class for memory buffer allocation, with memory availability
// controlled by resource manager. Memory is not initialized.
// Not thread-safe, must be only used sequentially.
class ResourceManagedBuffer {
 public:
  explicit ResourceManagedBuffer(
      scoped_refptr<ResourceManager> memory_resource);

  ResourceManagedBuffer(ResourceManagedBuffer& other) = delete;
  ResourceManagedBuffer& operator=(ResourceManagedBuffer& other) = delete;

  ~ResourceManagedBuffer();

  Status Allocate(size_t size);

  void Clear();

  char* at(size_t pos);
  size_t size() const;
  bool empty() const;

 private:
  std::unique_ptr<char[]> buffer_;
  size_t size_ = 0;

  const scoped_refptr<ResourceManager> memory_resource_;
};

}  // namespace reporting

#endif  // MISSIVE_RESOURCES_RESOURCE_MANAGED_BUFFER_H_
