//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef TPM_MANAGER_SERVER_TPM_INITIALIZER_IMPL_H_
#define TPM_MANAGER_SERVER_TPM_INITIALIZER_IMPL_H_

#include <string>
#include <vector>

#include <base/macros.h>
#include <trousers/tss.h>
#include <trousers/trousers.h>  // NOLINT(build/include_alpha)

#include "tpm_manager/server/dbus_service.h"
#include "tpm_manager/server/openssl_crypto_util_impl.h"
#include "tpm_manager/server/tpm_connection.h"
#include "tpm_manager/server/tpm_initializer.h"

namespace tpm_manager {

class LocalDataStore;
class TpmStatus;

// This class initializes a Tpm1.2 chip by taking ownership. Example use of
// this class is:
// LocalDataStore data_store;
// TpmStatusImpl status;
// OwnershipTakenCallBack callback;
// TpmInitializerImpl initializer(&data_store, &status, callback);
// initializer.InitializeTpm();
//
// If the tpm is unowned, InitializeTpm injects a random owner password,
// initializes and unrestricts the SRK, and persists the owner password to disk
// until all the owner dependencies are satisfied.
//
// The ownership taken callback must be provided when the tpm initializer is
// constructed and stay alive during the entire lifetime of the tpm initializer.
class TpmInitializerImpl : public TpmInitializer {
 public:
  // Does not take ownership of |local_data_store| or |tpm_status|.
  TpmInitializerImpl(LocalDataStore* local_data_store,
                     TpmStatus* tpm_status,
                     const OwnershipTakenCallBack& ownership_taken_callback);
  ~TpmInitializerImpl() override = default;

  // TpmInitializer methods.
  bool InitializeTpm() override;
  bool PreInitializeTpm() override;
  void VerifiedBootHelper() override;
  bool ResetDictionaryAttackLock() override;

 private:
  // This method checks if an EndorsementKey exists on the Tpm and creates it
  // if not. Returns true on success, else false. The |connection| already has
  // the owner password injected.
  bool InitializeEndorsementKey(TpmConnection* connection);

  // This method takes ownership of the Tpm with the default TSS password.
  // Returns true on success, else false. The |connection| already has the
  // default owner password injected.
  bool TakeOwnership(TpmConnection* connection);

  // This method initializes the SRK if it does not exist, zero's the SRK
  // password and unrestricts its usage. Returns true on success, else false.
  // The |connection| already has the current owner password injected.
  bool InitializeSrk(TpmConnection* connection);

  // This method changes the Tpm owner password from the default TSS password
  // to the password provided in the |owner_password| argument.
  // Returns true on success, else false. The |connection| already has the old
  // owner password injected.
  bool ChangeOwnerPassword(TpmConnection* connection,
                           const std::string& owner_password);

  bool ReadOwnerPasswordFromLocalData(std::string* owner_password);

  // create delegate with the default label and store the result in |delegate|.
  bool CreateDelegateWithDefaultLabel(AuthDelegate* delegate);

  // Creates a TPM owner delegate for future use.
  //
  // Parameters
  //   bound_pcrs - Specifies the PCRs to which the delegate is bound.
  //   delegate_family_label - Specifies the label of the created delegate
  //                           family. Should be equal to
  //                           |kDefaultDelegateFamilyLabel| in most cases. Non-
  //                           default values are primarily intended for testing
  //                           purposes.
  //   delegate_label - Specifies the label of the created delegate. Should be
  //                    equal to |kDefaultDelegateLabel| in most cases. Non-
  //                    default values are primarily intended for testing
  //                    purposes.
  //   delegate_blob - The blob for the owner delegate.
  //   delegate_secret - The delegate secret that will be required to perform
  //                     privileged operations in the future.
  bool CreateAuthDelegate(const std::vector<uint32_t>& bound_pcrs,
                          uint8_t delegate_family_label,
                          uint8_t delegate_label,
                          std::string* delegate_blob,
                          std::string* delegate_secret);

  // Retrieves a |data| attribute defined by |flag| and |sub_flag| from a TSS
  // |object_handle|. The |context_handle| is only used for TSS memory
  // management.
  bool GetDataAttribute(TSS_HCONTEXT context,
                        TSS_HOBJECT object,
                        TSS_FLAG flag,
                        TSS_FLAG sub_flag,
                        std::string* data);

  OpensslCryptoUtilImpl openssl_util_;
  LocalDataStore* local_data_store_;
  TpmStatus* tpm_status_;

  // Callback function called after TPM ownership is taken.
  const OwnershipTakenCallBack& ownership_taken_callback_;

  DISALLOW_COPY_AND_ASSIGN(TpmInitializerImpl);
};

}  // namespace tpm_manager

#endif  // TPM_MANAGER_SERVER_TPM_INITIALIZER_IMPL_H_
