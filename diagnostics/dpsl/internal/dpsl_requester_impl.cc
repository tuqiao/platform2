// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/dpsl/internal/dpsl_requester_impl.h"

#include <utility>

#include <base/bind.h>
#include <base/location.h>
#include <base/logging.h>
#include <base/run_loop.h>
#include <base/threading/thread_task_runner_handle.h>

#include "diagnostics/constants/grpc_constants.h"
#include "diagnostics/dpsl/internal/callback_utils.h"
#include "diagnostics/dpsl/public/dpsl_thread_context.h"

#include "wilco_dtc_supportd.pb.h"  // NOLINT(build/include)

namespace diagnostics {

namespace {

std::string GetWilcoDtcSupportdGrpcUri(
    DpslRequester::GrpcClientUri grpc_client_uri) {
  switch (grpc_client_uri) {
    case DpslRequester::GrpcClientUri::kLocalDomainSocket:
      return kWilcoDtcSupportdGrpcDomainSocketUri;
    case DpslRequester::GrpcClientUri::kVmVsock:
      return GetWilcoDtcSupportdGrpcGuestVsockUri();
  }
  NOTREACHED() << "Unexpected GrpcClientUri: "
               << static_cast<int>(grpc_client_uri);
  return "";
}

}  // namespace

DpslRequesterImpl::DpslRequesterImpl(
    const std::string& wilco_dtc_supportd_grpc_uri)
    : message_loop_(base::MessageLoop::current()),
      async_grpc_client_(base::ThreadTaskRunnerHandle::Get(),
                         wilco_dtc_supportd_grpc_uri) {
  DCHECK(message_loop_);
}

DpslRequesterImpl::~DpslRequesterImpl() {
  CHECK(sequence_checker_.CalledOnValidSequence());

  // Prevent new requests from being processed.
  async_grpc_client_shutting_down_ = true;

  // Note: this potentially may be a nested run loop - if the consumer of the
  // library destroys DpslRequesterImpl from a task running on the current
  // message loop.
  base::RunLoop run_loop;
  async_grpc_client_.Shutdown(run_loop.QuitClosure());
  run_loop.Run();
}

void DpslRequesterImpl::SendMessageToUi(
    std::unique_ptr<grpc_api::SendMessageToUiRequest> request,
    SendMessageToUiCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncSendMessageToUi,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetProcData(
    std::unique_ptr<grpc_api::GetProcDataRequest> request,
    GetProcDataCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetProcData,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetSysfsData(
    std::unique_ptr<grpc_api::GetSysfsDataRequest> request,
    GetSysfsDataCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetSysfsData,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::PerformWebRequest(
    std::unique_ptr<grpc_api::PerformWebRequestParameter> request,
    PerformWebRequestCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncPerformWebRequest,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetEcTelemetry(
    std::unique_ptr<grpc_api::GetEcTelemetryRequest> request,
    GetEcTelemetryRequestCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetEcTelemetry,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetAvailableRoutines(
    std::unique_ptr<grpc_api::GetAvailableRoutinesRequest> request,
    GetAvailableRoutinesCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetAvailableRoutines,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::RunRoutine(
    std::unique_ptr<grpc_api::RunRoutineRequest> request,
    RunRoutineCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncRunRoutine,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetRoutineUpdate(
    std::unique_ptr<grpc_api::GetRoutineUpdateRequest> request,
    GetRoutineUpdateCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetRoutineUpdate,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetOsVersion(
    std::unique_ptr<grpc_api::GetOsVersionRequest> request,
    GetOsVersionCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetOsVersion,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetConfigurationData(
    std::unique_ptr<grpc_api::GetConfigurationDataRequest> request,
    GetConfigurationDataCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetConfigurationData,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetVpdField(
    std::unique_ptr<grpc_api::GetVpdFieldRequest> request,
    GetVpdFieldCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetVpdField,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetBluetoothData(
    std::unique_ptr<grpc_api::GetBluetoothDataRequest> request,
    GetBluetoothDataCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetBluetoothData,
      std::move(request), std::move(callback));
}

void DpslRequesterImpl::GetDriveSystemData(
    std::unique_ptr<grpc_api::GetDriveSystemDataRequest> request,
    GetDriveSystemDataCallback callback) {
  ScheduleGrpcClientMethodCall(
      FROM_HERE, &grpc_api::WilcoDtcSupportd::Stub::AsyncGetDriveSystemData,
      std::move(request), std::move(callback));
}

template <typename GrpcStubMethod, typename RequestType, typename ResponseType>
void DpslRequesterImpl::ScheduleGrpcClientMethodCall(
    const base::Location& location,
    GrpcStubMethod grpc_stub_method,
    std::unique_ptr<RequestType> request,
    std::function<void(std::unique_ptr<ResponseType>)> response_callback) {
  message_loop_->task_runner()->PostTask(
      location,
      base::Bind(
          &DpslRequesterImpl::CallGrpcClientMethod<GrpcStubMethod, RequestType,
                                                   ResponseType>,
          weak_ptr_factory_.GetWeakPtr(), grpc_stub_method,
          base::Passed(std::move(request)),
          MakeCallbackFromStdFunction(std::move(response_callback))));
}

template <typename GrpcStubMethod, typename RequestType, typename ResponseType>
void DpslRequesterImpl::CallGrpcClientMethod(
    GrpcStubMethod grpc_stub_method,
    std::unique_ptr<RequestType> request,
    base::Callback<void(std::unique_ptr<ResponseType>)> response_callback) {
  DCHECK(sequence_checker_.CalledOnValidSequence());
  if (async_grpc_client_shutting_down_) {
    // Bail out if the client is already being shut down, to avoid doing
    // CallRpc() in this state.
    response_callback.Run(nullptr /* response */);
    return;
  }
  async_grpc_client_.CallRpc(grpc_stub_method, *request,
                             std::move(response_callback));
}

// static
std::unique_ptr<DpslRequester> DpslRequester::Create(
    DpslThreadContext* thread_context, GrpcClientUri grpc_client_uri) {
  CHECK(thread_context);
  CHECK(thread_context->BelongsToCurrentThread());

  return std::make_unique<DpslRequesterImpl>(
      GetWilcoDtcSupportdGrpcUri(grpc_client_uri));
}

}  // namespace diagnostics
