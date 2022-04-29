// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rmad/state_handler/finalize_state_handler.h"

#include <algorithm>
#include <string>

#include <base/bind.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/notreached.h>
#include <base/synchronization/lock.h>
#include <base/task/task_traits.h>
#include <base/task/thread_pool.h>

#include "rmad/constants.h"
#include "rmad/utils/cr50_utils_impl.h"
#include "rmad/utils/crossystem_utils_impl.h"
#include "rmad/utils/fake_cr50_utils.h"
#include "rmad/utils/fake_crossystem_utils.h"
#include "rmad/utils/fake_flashrom_utils.h"
#include "rmad/utils/flashrom_utils_impl.h"

namespace {

constexpr char kEmptyBoardIdType[] = "ffffffff";
constexpr char kTestBoardIdType[] = "5a5a4352";             // ZZCR.
constexpr char kPvtBoardIdFlags[] = "00007f80";             // pvt.
constexpr char kCustomLabelPvtBoardIdFlags[] = "00003f80";  // customlabel_pvt.

}  // namespace

namespace rmad {

namespace fake {

FakeFinalizeStateHandler::FakeFinalizeStateHandler(
    scoped_refptr<JsonStore> json_store,
    scoped_refptr<DaemonCallback> daemon_callback,
    const base::FilePath& working_dir_path)
    : FinalizeStateHandler(
          json_store,
          daemon_callback,
          working_dir_path,
          std::make_unique<FakeCr50Utils>(working_dir_path),
          std::make_unique<FakeCrosSystemUtils>(working_dir_path),
          std::make_unique<FakeFlashromUtils>()) {}

}  // namespace fake

FinalizeStateHandler::FinalizeStateHandler(
    scoped_refptr<JsonStore> json_store,
    scoped_refptr<DaemonCallback> daemon_callback)
    : BaseStateHandler(json_store, daemon_callback),
      working_dir_path_(kDefaultWorkingDirPath) {
  cr50_utils_ = std::make_unique<Cr50UtilsImpl>();
  crossystem_utils_ = std::make_unique<CrosSystemUtilsImpl>();
  flashrom_utils_ = std::make_unique<FlashromUtilsImpl>();
}

FinalizeStateHandler::FinalizeStateHandler(
    scoped_refptr<JsonStore> json_store,
    scoped_refptr<DaemonCallback> daemon_callback,
    const base::FilePath& working_dir_path,
    std::unique_ptr<Cr50Utils> cr50_utils,
    std::unique_ptr<CrosSystemUtils> crossystem_utils,
    std::unique_ptr<FlashromUtils> flashrom_utils)
    : BaseStateHandler(json_store, daemon_callback),
      working_dir_path_(working_dir_path),
      cr50_utils_(std::move(cr50_utils)),
      crossystem_utils_(std::move(crossystem_utils)),
      flashrom_utils_(std::move(flashrom_utils)) {}

RmadErrorCode FinalizeStateHandler::InitializeState() {
  if (!state_.has_finalize()) {
    state_.set_allocated_finalize(new FinalizeState);
    status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_UNKNOWN);
    status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_UNKNOWN);
  }
  if (!task_runner_) {
    task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::TaskPriority::BEST_EFFORT, base::MayBlock()});
  }

  return RMAD_ERROR_OK;
}

void FinalizeStateHandler::RunState() {
  StartStatusTimer();
  if (status_.status() == FinalizeStatus::RMAD_FINALIZE_STATUS_UNKNOWN) {
    StartFinalize();
  }
}

void FinalizeStateHandler::CleanUpState() {
  StopStatusTimer();
}

BaseStateHandler::GetNextStateCaseReply FinalizeStateHandler::GetNextStateCase(
    const RmadState& state) {
  if (!state.has_finalize()) {
    LOG(ERROR) << "RmadState missing |finalize| state.";
    return NextStateCaseWrapper(RMAD_ERROR_REQUEST_INVALID);
  }

  switch (state.finalize().choice()) {
    case FinalizeState::RMAD_FINALIZE_CHOICE_UNKNOWN:
      return NextStateCaseWrapper(RMAD_ERROR_REQUEST_ARGS_MISSING);
    case FinalizeState::RMAD_FINALIZE_CHOICE_CONTINUE:
      switch (status_.status()) {
        case FinalizeStatus::RMAD_FINALIZE_STATUS_IN_PROGRESS:
          return NextStateCaseWrapper(RMAD_ERROR_WAIT);
        case FinalizeStatus::RMAD_FINALIZE_STATUS_COMPLETE:
          [[fallthrough]];
        case FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_NON_BLOCKING:
          return NextStateCaseWrapper(RmadState::StateCase::kRepairComplete);
        case FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_BLOCKING:
          return NextStateCaseWrapper(RMAD_ERROR_FINALIZATION_FAILED);
        default:
          break;
      }
      NOTREACHED();
      break;
    case FinalizeState::RMAD_FINALIZE_CHOICE_RETRY:
      StartFinalize();
      return NextStateCaseWrapper(RMAD_ERROR_WAIT);
    default:
      break;
  }

  NOTREACHED();
  return NextStateCaseWrapper(RMAD_ERROR_TRANSITION_FAILED);
}

void FinalizeStateHandler::SendStatusSignal() {
  daemon_callback_->GetFinalizeSignalCallback().Run(status_);
}

void FinalizeStateHandler::StartStatusTimer() {
  StopStatusTimer();
  status_timer_.Start(FROM_HERE, kReportStatusInterval, this,
                      &FinalizeStateHandler::SendStatusSignal);
}

void FinalizeStateHandler::StopStatusTimer() {
  if (status_timer_.IsRunning()) {
    status_timer_.Stop();
  }
}

void FinalizeStateHandler::StartFinalize() {
  status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_IN_PROGRESS);
  status_.set_progress(0);
  status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_UNKNOWN);
  task_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&FinalizeStateHandler::FinalizeTask,
                                        base::Unretained(this)));
}

void FinalizeStateHandler::FinalizeTask() {
  // Enable SWWP if HWWP is still disabled.
  if (int hwwp_status;
      crossystem_utils_->GetHwwpStatus(&hwwp_status) && hwwp_status == 0) {
    if (!flashrom_utils_->EnableSoftwareWriteProtection()) {
      LOG(ERROR) << "Failed to enable software write protection";
      status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_BLOCKING);
      status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_CANNOT_ENABLE_SWWP);
      return;
    }
  }

  status_.set_progress(0.5);

  // Disable factory mode if it's still enabled.
  if (!cr50_utils_->DisableFactoryMode()) {
    LOG(ERROR) << "Failed to disable factory mode";
    status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_BLOCKING);
    status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_CANNOT_ENABLE_HWWP);
    return;
  }

  status_.set_progress(0.8);

  // Make sure HWWP is disabled.
  if (int hwwp_status;
      !crossystem_utils_->GetHwwpStatus(&hwwp_status) || hwwp_status != 1) {
    LOG(ERROR) << "HWWP is still disabled";
    status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_BLOCKING);
    status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_CANNOT_ENABLE_HWWP);
    return;
  }

  status_.set_progress(0.9);

  // Make sure cr50 board ID type and board ID flags are set.
  if (std::string board_id_type; !cr50_utils_->GetBoardIdType(&board_id_type) ||
                                 board_id_type == kEmptyBoardIdType ||
                                 board_id_type == kTestBoardIdType) {
    LOG(ERROR) << "Cr50 board ID type is invalid: " << board_id_type;
    if (base::PathExists(
            working_dir_path_.AppendASCII(kTestDeviceTagFilePath))) {
      LOG(INFO) << "Cr50 board ID check bypassed";
    } else {
      status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_BLOCKING);
      status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_CR50);
      return;
    }
  }
  if (std::string board_id_flags;
      !cr50_utils_->GetBoardIdFlags(&board_id_flags) ||
      (board_id_flags != kPvtBoardIdFlags &&
       board_id_flags != kCustomLabelPvtBoardIdFlags)) {
    LOG(ERROR) << "Cr50 board ID flags is invalid: " << board_id_flags;
    if (base::PathExists(
            working_dir_path_.AppendASCII(kTestDeviceTagFilePath))) {
      LOG(INFO) << "Cr50 board ID flags check bypassed";
    } else {
      status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_FAILED_BLOCKING);
      status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_CR50);
      return;
    }
  }

  // TODO(chenghan): Check GBB flags.
  status_.set_status(FinalizeStatus::RMAD_FINALIZE_STATUS_COMPLETE);
  status_.set_progress(1);
  status_.set_error(FinalizeStatus::RMAD_FINALIZE_ERROR_UNKNOWN);
}

}  // namespace rmad
