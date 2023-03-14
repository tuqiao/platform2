// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBHWSEC_BACKEND_STORAGE_H_
#define LIBHWSEC_BACKEND_STORAGE_H_

#include <cstdint>

#include <brillo/secure_blob.h>

#include "libhwsec/status.h"
#include "libhwsec/structures/space.h"

namespace hwsec {

// Storage provide the functions for writeable space.
class Storage {
 public:
  enum class ReadyState {
    // Ready to use, and both read and write are allowed.
    kReadableAndWritable,
    // Ready to use, but write locked.
    kReadable,
    // Not ready to use, should be prepared before using it.
    kPreparable,
  };

  struct LockOptions {
    bool read_lock = false;
    bool write_lock = false;
  };

  // Is the |space| ready to use (defined correctly) or not.
  virtual StatusOr<ReadyState> IsReady(Space space) = 0;

  // Tries to make the |space| become ready and have enough |size| to write.
  virtual Status Prepare(Space space, uint32_t size) = 0;

  // Reads the data from the |space|.
  virtual StatusOr<brillo::Blob> Load(Space space) = 0;

  // Writes the |blob| into the |space|.
  virtual Status Store(Space space, const brillo::Blob& blob) = 0;

  // Locks the |space| with some optional |options|.
  virtual Status Lock(Space space, LockOptions options) = 0;

  // Destroys the |space|.
  virtual Status Destroy(Space space) = 0;

 protected:
  Storage() = default;
  ~Storage() = default;
};

}  // namespace hwsec

#endif  // LIBHWSEC_BACKEND_STORAGE_H_
