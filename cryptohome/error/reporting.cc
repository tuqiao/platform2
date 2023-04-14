// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/error/reporting.h"

#include <string>

#include <base/logging.h>
#include <brillo/hash/MurmurHash3.h>
#include <libhwsec/error/tpm_error.h>

#include "cryptohome/cryptohome_metrics.h"

namespace cryptohome {

namespace error {

namespace {

using hwsec_foundation::status::StatusChain;

// Report every node in the error.
void ReportAllLocations(const StatusChain<CryptohomeError>& stack,
                        const std::string& error_bucket_name) {
  for (const auto& err : stack.const_range()) {
    auto loc = err.local_location();
    ReportCryptohomeErrorAllLocations(error_bucket_name,
                                      static_cast<uint32_t>(loc));
  }
}

// Just a random number.
constexpr uint32_t kHashedStackSeed = 10114;

// Report the entire error id's hash.
void ReportHashedStack(const user_data_auth::CryptohomeErrorInfo& info,
                       const std::string& error_bucket_name) {
  std::string error_id = info.error_id();
  uint32_t result;
  brillo::MurmurHash3_x86_32(error_id.c_str(), error_id.size(),
                             kHashedStackSeed, &result);
  LOG(INFO) << "Reporting cryptohome error hashed stack " << result << " from "
            << error_id;
  ReportCryptohomeErrorHashedStack(error_bucket_name, result);
}

// Report all node that contains kDevCheckUnexpectedState.
void ReportDevCheckUnexpectedState(const StatusChain<CryptohomeError>& stack,
                                   const std::string& error_bucket_name) {
  for (const auto& err : stack.const_range()) {
    if (!std::holds_alternative<PossibleActions>(err.local_actions())) {
      continue;
    }
    const auto& possible_actions =
        std::get<PossibleActions>(err.local_actions());
    if (possible_actions[PossibleAction::kDevCheckUnexpectedState]) {
      auto loc = err.local_location();
      ReportCryptohomeErrorDevCheckUnexpectedState(error_bucket_name,
                                                   static_cast<uint32_t>(loc));
    }
  }
}

void ReportLeafNode(const StatusChain<CryptohomeError>& stack,
                    const std::string& error_bucket_name) {
  bool have_tpm_error = false;
  CryptohomeError::ErrorLocation last_non_tpm_loc, last_tpm_loc;
  // last_non_tpm_loc is a location that is not of the type CryptohomeTPMError,
  // i.e. it doesn't have kUnifiedErrorBit set. last_tpm_loc is a location
  // that is of the type CryptohomeTPMError.

  for (const auto& node : stack.const_range()) {
    auto loc = node.local_location();
    if ((loc & hwsec::unified_tpm_error::kUnifiedErrorBit) != 0) {
      // TPM case.
      have_tpm_error = true;
      last_tpm_loc = loc;
    } else {
      // Non-TPM case.
      last_non_tpm_loc = loc;
    }
  }

  if (!have_tpm_error) {
    // No TPM error, just report the leaf node.
    ReportCryptohomeErrorLeaf(error_bucket_name,
                              static_cast<uint32_t>(last_non_tpm_loc));
  } else {
    // There's a TPM error, report the leaf node and the TPM error.
    // For the TPM error, we always report only the last node.
    CryptohomeError::ErrorLocation tpm_error_to_report = last_tpm_loc;

    // The unified error bit is not reported.
    tpm_error_to_report =
        tpm_error_to_report & (~hwsec::unified_tpm_error::kUnifiedErrorBit);
    DCHECK_EQ(
        tpm_error_to_report & (~hwsec::unified_tpm_error::kUnifiedErrorMask),
        0);
    DCHECK_EQ(last_non_tpm_loc & (~hwsec::unified_tpm_error::kUnifiedErrorMask),
              0);
    CryptohomeError::ErrorLocation mixed =
        ((last_non_tpm_loc & hwsec::unified_tpm_error::kUnifiedErrorMask)
         << 16) |
        (tpm_error_to_report & hwsec::unified_tpm_error::kUnifiedErrorMask);

    ReportCryptohomeErrorLeafWithTPM(error_bucket_name,
                                     static_cast<uint32_t>(mixed));
  }
}

}  // namespace

void ReportCryptohomeError(const StatusChain<CryptohomeError>& err,
                           const user_data_auth::CryptohomeErrorInfo& info,
                           const std::string& error_bucket_name) {
  if (err.ok()) {
    // No error? No need to report.
    return;
  }

  LOG(WARNING) << "Cryptohome " << error_bucket_name
               << " reported on DBus API: " << err;

  // The actual reportings.
  ReportAllLocations(err, error_bucket_name);
  ReportHashedStack(info, error_bucket_name);
  ReportDevCheckUnexpectedState(err, error_bucket_name);
  ReportLeafNode(err, error_bucket_name);
}

}  // namespace error

}  // namespace cryptohome
