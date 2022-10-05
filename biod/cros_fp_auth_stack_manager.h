// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BIOD_CROS_FP_AUTH_STACK_MANAGER_H_
#define BIOD_CROS_FP_AUTH_STACK_MANAGER_H_

#include "biod/auth_stack_manager.h"

#include <memory>
#include <string>
#include <vector>

#include <base/memory/weak_ptr.h>

#include "biod/cros_fp_device.h"
#include "biod/cros_fp_record_manager.h"
#include "biod/power_button_filter_interface.h"
#include "biod/proto_bindings/constants.pb.h"
#include "biod/proto_bindings/messages.pb.h"

namespace biod {

class BiodMetrics;

class CrosFpAuthStackManager : public AuthStackManager {
 public:
  // Current state of CrosFpAuthStackManager. We maintain a state machine
  // because some operations can only be processed in some states.
  enum class State {
    // Initial state, neither any session is pending nor we're expecting
    // Create/AuthenticateCredential commands to come.
    kNone,
    // An EnrollSession is ongoing.
    kEnroll,
    // An EnrollSession is completed successfully and we're expecting a
    // CreateCredential command.
    kEnrollDone,
  };

  CrosFpAuthStackManager(
      std::unique_ptr<PowerButtonFilterInterface> power_button_filter,
      std::unique_ptr<ec::CrosFpDeviceInterface> cros_fp_device,
      BiodMetricsInterface* biod_metrics,
      std::unique_ptr<CrosFpRecordManagerInterface> record_manager);
  CrosFpAuthStackManager(const CrosFpAuthStackManager&) = delete;
  CrosFpAuthStackManager& operator=(const CrosFpAuthStackManager&) = delete;

  // AuthStackManager overrides:
  ~CrosFpAuthStackManager() override = default;

  BiometricType GetType() override;
  AuthStackManager::Session StartEnrollSession() override;
  CreateCredentialReply CreateCredential(
      const CreateCredentialRequest& request) override;
  AuthStackManager::Session StartAuthSession() override;
  AuthenticateCredentialReply AuthenticateCredential(
      const AuthenticateCredentialRequest& request) override;
  void RemoveRecordsFromMemory() override;
  bool ReadRecordsForSingleUser(const std::string& user_id) override;
  void SetEnrollScanDoneHandler(const AuthStackManager::EnrollScanDoneCallback&
                                    on_enroll_scan_done) override;
  void SetAuthScanDoneHandler(
      const AuthStackManager::AuthScanDoneCallback& on_auth_scan_done) override;
  void SetSessionFailedHandler(const AuthStackManager::SessionFailedCallback&
                                   on_session_failed) override;

 protected:
  void EndEnrollSession() override;
  void EndAuthSession() override;

 private:
  using SessionAction = base::RepeatingCallback<void(const uint32_t event)>;

  void OnMkbpEvent(uint32_t event);
  void KillMcuSession();
  void OnTaskComplete();

  void OnEnrollScanDone(ScanResult result,
                        const AuthStackManager::EnrollStatus& enroll_status,
                        brillo::Blob auth_nonce);
  void OnSessionFailed();

  bool RequestEnrollImage();
  bool RequestEnrollFingerUp();
  void DoEnrollImageEvent(uint32_t event);
  void DoEnrollFingerUpEvent(uint32_t event);

  std::string CurrentStateToString();
  // Whether current state is waiting for a next session action.
  bool IsActiveState();
  bool CanStartEnroll();

  BiodMetricsInterface* biod_metrics_ = nullptr;
  std::unique_ptr<ec::CrosFpDeviceInterface> cros_dev_;

  SessionAction next_session_action_;

  // This vector contains RecordIds of templates loaded into the MCU.
  std::vector<std::string> loaded_records_;

  AuthStackManager::EnrollScanDoneCallback on_enroll_scan_done_;
  AuthStackManager::AuthScanDoneCallback on_auth_scan_done_;
  AuthStackManager::SessionFailedCallback on_session_failed_;

  State state_ = State::kNone;

  std::unique_ptr<PowerButtonFilterInterface> power_button_filter_;

  std::unique_ptr<CrosFpRecordManagerInterface> record_manager_;

  base::WeakPtrFactory<CrosFpAuthStackManager> session_weak_factory_;
};

}  // namespace biod

#endif  // BIOD_CROS_FP_AUTH_STACK_MANAGER_H_
