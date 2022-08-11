/* Copyright 2022 The ChromiumOS Authors.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_HAL_FAKE_CAMERA_HAL_H_
#define CAMERA_HAL_FAKE_CAMERA_HAL_H_

#include "cros-camera/cros_camera_hal.h"

namespace cros {

// This class is not thread-safe. All functions in camera_module_t are called by
// one mojo thread which is in hal adapter. The hal adapter makes sure these
// functions are not called concurrently. The hal adapter also has different
// dedicated threads to handle camera_module_callbacks_t, camera3_device_ops_t,
// and camera3_callback_ops_t.
class CameraHal {
 public:
  CameraHal();
  CameraHal(const CameraHal&) = delete;
  CameraHal& operator=(const CameraHal&) = delete;

  ~CameraHal();

  static CameraHal& GetInstance();

  // Implementations for camera_module_t.
  int GetNumberOfCameras() const;
  int SetCallbacks(const camera_module_callbacks_t* callbacks);
  int Init();

  // Implementations for cros_camera_hal_t.
  void SetUp(CameraMojoChannelManagerToken* token);
  void TearDown();
  void SetPrivacySwitchCallback(PrivacySwitchStateChangeCallback callback);
  int OpenDevice(int id,
                 const hw_module_t* module,
                 hw_device_t** hw_device,
                 ClientType client_type);
  int GetCameraInfo(int id, struct camera_info* info, ClientType client_type);
};
}  // namespace cros

extern camera_module_t HAL_MODULE_INFO_SYM;

#endif  // CAMERA_HAL_FAKE_CAMERA_HAL_H_
