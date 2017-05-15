// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file contains wrapper of fmap functions.

#include "hammerd/fmap_utils.h"

namespace hammerd {

int64_t Fmap::Find(const uint8_t* image, unsigned int len) {
  return static_cast<int64_t>(fmap_find(image, len));
}
const fmap_area* Fmap::FindArea(const fmap* fmap, const std::string& name) {
  return fmap_find_area(fmap, name.data());
}

}  // namespace hammerd
