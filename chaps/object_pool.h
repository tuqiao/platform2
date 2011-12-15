// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHAPS_OBJECT_POOL_H
#define CHAPS_OBJECT_POOL_H

namespace chaps {

// An ObjectPool instance manages a collection of objects.  A persistent object
// pool is backed by a database where all object data and object-related
// metadata is stored.
// TODO(dkrahn): Fully define this interface.
class ObjectPool {
 public:
  virtual ~ObjectPool() {}
  // These methods get and set internal persistent blobs. These internal blobs
  // are for use by Chaps. PKCS #11 applications will not see these when
  // searching for objects.
  //   blob_id - The value of this identifier must be managed by the caller.
  //             Only one blob can be set per blob_id (i.e. a subsequent call
  //             to SetInternalBlob with the same blob_id will overwrite the
  //             blob).
  virtual bool GetInternalBlob(int blob_id, std::string* blob) = 0;
  virtual bool SetInternalBlob(int blob_id, const std::string& blob) = 0;
  // SetKey sets the encryption key for objects in this pool. This is only
  // relevant if the pool is persistent; an object pool has no obligation to
  // encrypt object data in memory.
  virtual void SetKey(const std::string& key) = 0;
};

}  // namespace

#endif  // CHAPS_OBJECT_POOL_H
