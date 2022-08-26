/*
 * Copyright 2022 The ChromiumOS Authors.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_COMMON_TEST_SUPPORT_FAKE_STILL_CAPTURE_PROCESSOR_H_
#define CAMERA_COMMON_TEST_SUPPORT_FAKE_STILL_CAPTURE_PROCESSOR_H_

#include <map>

#include <gtest/gtest.h>

#include "common/still_capture_processor.h"

namespace cros::tests {

class FakeStillCaptureProcessor : public StillCaptureProcessor {
 public:
  FakeStillCaptureProcessor() = default;
  ~FakeStillCaptureProcessor() override = default;

  void Initialize(const camera3_stream_t* const still_capture_stream,
                  CaptureResultCallback result_callback) override;
  void Reset() override;
  void QueuePendingOutputBuffer(
      int frame_number,
      camera3_stream_buffer_t output_buffer,
      const camera_metadata_t* request_settings) override;
  void QueuePendingAppsSegments(int frame_number,
                                buffer_handle_t blob_buffer) override;
  void QueuePendingYuvImage(int frame_number,
                            buffer_handle_t yuv_buffer) override;

 private:
  void MaybeProduceCaptureResult(int frame_number);

  CaptureResultCallback result_callback_;

  struct ResultDescriptor {
    bool has_apps_segments = false;
    bool has_yuv_buffer = false;
  };
  std::map<int, ResultDescriptor> result_descriptor_;
};

}  // namespace cros::tests

#endif  // CAMERA_COMMON_TEST_SUPPORT_FAKE_STILL_CAPTURE_PROCESSOR_H_
