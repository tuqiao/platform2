// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file contains wrapper of fmap functions.

#ifndef HAMMERD_FMAP_UTILS_H_
#define HAMMERD_FMAP_UTILS_H_

#include <fmap.h>
#include <stdint.h>

#include <string>

#include <base/macros.h>

namespace hammerd {

class FmapInterface {
 public:
  virtual int64_t Find(const uint8_t* image, unsigned int len) = 0;
  virtual const fmap_area* FindArea(const fmap* fmap,
                                    const std::string& name) = 0;
};

class Fmap : public FmapInterface {
 public:
  Fmap() = default;
  int64_t Find(const uint8_t* image, unsigned int len) override;
  const fmap_area* FindArea(const fmap* fmap, const std::string& name) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(Fmap);
};

}  // namespace hammerd
#endif  // HAMMERD_FMAP_UTILS_H_
