// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libhwsec/backend/tpm2/state.h"

#include <utility>

#include <libhwsec-foundation/status/status_chain_macros.h>
#include <tpm_manager/proto_bindings/tpm_manager.pb.h>
#include <tpm_manager-client/tpm_manager/dbus-proxies.h>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "libhwsec/backend/tpm2/backend.h"
#include "libhwsec/error/tpm_manager_error.h"

using hwsec_foundation::status::MakeStatus;

namespace hwsec {

StatusOr<bool> StateTpm2::IsEnabled() {
  tpm_manager::GetTpmNonsensitiveStatusRequest request;
  tpm_manager::GetTpmNonsensitiveStatusReply reply;

  if (brillo::ErrorPtr err;
      !backend_.GetProxy().GetTpmManager().GetTpmNonsensitiveStatus(
          request, &reply, &err, Proxy::kDefaultDBusTimeoutMs)) {
    return MakeStatus<TPMError>(TPMRetryAction::kCommunication)
        .Wrap(std::move(err));
  }

  RETURN_IF_ERROR(MakeStatus<TPMManagerError>(reply.status()));

  return reply.is_enabled();
}

StatusOr<bool> StateTpm2::IsReady() {
  tpm_manager::GetTpmNonsensitiveStatusRequest request;
  tpm_manager::GetTpmNonsensitiveStatusReply reply;

  if (brillo::ErrorPtr err;
      !backend_.GetProxy().GetTpmManager().GetTpmNonsensitiveStatus(
          request, &reply, &err, Proxy::kDefaultDBusTimeoutMs)) {
    return MakeStatus<TPMError>(TPMRetryAction::kCommunication)
        .Wrap(std::move(err));
  }

  RETURN_IF_ERROR(MakeStatus<TPMManagerError>(reply.status()));

  return reply.is_owned();
}

Status StateTpm2::Prepare() {
  tpm_manager::TakeOwnershipRequest request;
  tpm_manager::TakeOwnershipReply reply;

  if (brillo::ErrorPtr err; !backend_.GetProxy().GetTpmManager().TakeOwnership(
          request, &reply, &err, Proxy::kDefaultDBusTimeoutMs)) {
    return MakeStatus<TPMError>(TPMRetryAction::kCommunication)
        .Wrap(std::move(err));
  }

  return MakeStatus<TPMManagerError>(reply.status());
}

void StateTpm2::WaitUntilReady(base::OnceCallback<void(Status)> callback) {
  if (!received_ready_signal_.has_value()) {
    received_ready_signal_ = false;
    backend_.GetProxy()
        .GetTpmManager()
        .RegisterSignalOwnershipTakenSignalHandler(
            base::IgnoreArgs<const tpm_manager::OwnershipTakenSignal&>(
                base::BindRepeating(&StateTpm2::OnReady,
                                    weak_factory_.GetWeakPtr())),
            base::DoNothing());
  }

  if (received_ready_signal_.value() == false) {
    received_ready_signal_ = IsReady().value_or(false);
  }

  if (received_ready_signal_.value() == true) {
    std::move(callback).Run(OkStatus());
    return;
  }

  ready_callbacks_.push_back(std::move(callback));
}

void StateTpm2::OnReady() {
  received_ready_signal_ = true;

  std::vector<base::OnceCallback<void(Status)>> callbacks =
      std::move(ready_callbacks_);
  ready_callbacks_.clear();

  for (base::OnceCallback<void(Status)>& callback : callbacks) {
    std::move(callback).Run(OkStatus());
  }
}

}  // namespace hwsec
