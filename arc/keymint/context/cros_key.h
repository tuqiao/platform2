// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ARC_KEYMINT_CONTEXT_CROS_KEY_H_
#define ARC_KEYMINT_CONTEXT_CROS_KEY_H_

#include <memory>
#include <string>

#include <base/memory/ref_counted.h>
#include <base/memory/weak_ptr.h>
#include <brillo/secure_blob.h>
#include <chaps/pkcs11/cryptoki.h>
#include <hardware/keymaster_defs.h>
#include <keymaster/key.h>
#include <keymaster/key_factory.h>
#include <keymaster/operation.h>

#include "arc/keymint/context/chaps_client.h"
#include "arc/keymint/context/context_adaptor.h"
#include "arc/keymint/context/crypto_operation.h"
#include "arc/keymint/key_data.pb.h"

namespace arc::keymint::context {

class CrosOperationFactory;

class CrosKeyFactory : public ::keymaster::KeyFactory {
 public:
  CrosKeyFactory(base::WeakPtr<ContextAdaptor> context_adaptor,
                 keymaster_algorithm_t algorithm);
  // Not copyable nor assignable.
  CrosKeyFactory(const CrosKeyFactory&) = delete;
  CrosKeyFactory& operator=(const CrosKeyFactory&) = delete;

  // Creates a ::keymaster::Key object given an instance of KeyData.
  //
  // If the blob was generated by arc-keymintd for a CrOS key (like chaps
  // keys), this method will load it with the configuration necessary to execute
  // operations on the original key in chaps.
  //
  // Returns error otherwise as the blob was either generated by Android or is
  // invalid.
  keymaster_error_t LoadKey(
      KeyData&& key_data,
      ::keymaster::AuthorizationSet&& hw_enforced,
      ::keymaster::AuthorizationSet&& sw_enforced,
      ::keymaster::UniquePtr<::keymaster::Key>* key) const;

  // Needed to implement pure virtual function in parent class and will return
  // error. Should never be called.
  keymaster_error_t LoadKey(
      ::keymaster::KeymasterKeyBlob&& key_material,
      const ::keymaster::AuthorizationSet& additional_params,
      ::keymaster::AuthorizationSet&& hw_enforced,
      ::keymaster::AuthorizationSet&& sw_enforced,
      ::keymaster::UniquePtr<::keymaster::Key>* key) const override;

  // Retrieve the operation factory for CrOS keys.
  ::keymaster::OperationFactory* GetOperationFactory(
      keymaster_purpose_t purpose) const override;

  // Key generation is not handled by this factory and will return error. Should
  // never be called.
  keymaster_error_t GenerateKey(
      const ::keymaster::AuthorizationSet& key_description,
      ::keymaster::UniquePtr<::keymaster::Key> attestation_signing_key,
      const ::keymaster::KeymasterBlob& issuer_subject,
      ::keymaster::KeymasterKeyBlob* key_blob,
      ::keymaster::AuthorizationSet* hw_enforced,
      ::keymaster::AuthorizationSet* sw_enforced,
      ::keymaster::CertificateChain* cert_chain) const override;

  keymaster_error_t ImportKey(
      const ::keymaster::AuthorizationSet& key_description,
      keymaster_key_format_t input_key_material_format,
      const ::keymaster::KeymasterKeyBlob& input_key_material,
      ::keymaster::UniquePtr<::keymaster::Key> attestation_signing_key,
      const ::keymaster::KeymasterBlob& issuer_subject,
      ::keymaster::KeymasterKeyBlob* output_key_blob,
      ::keymaster::AuthorizationSet* hw_enforced,
      ::keymaster::AuthorizationSet* sw_enforced,
      ::keymaster::CertificateChain* cert_chain) const override;

  // Key import is not handled by this factory and this will return error. This
  // method should never be called.
  const keymaster_key_format_t* SupportedImportFormats(
      size_t* format_count) const override;

  // Key export is not handled by this factory and this will return error. This
  // method should never be called.
  const keymaster_key_format_t* SupportedExportFormats(
      size_t* format_count) const override;

  // Expose the dbus adaptor object to be used by operations.
  const base::WeakPtr<ContextAdaptor>& context_adaptor() const {
    return context_adaptor_;
  }

 private:
  base::WeakPtr<ContextAdaptor> context_adaptor_;

  mutable std::unique_ptr<CrosOperationFactory> sign_factory_;
};

// Base class for ChromeOS keys.
class CrosKey : public ::keymaster::Key {
 public:
  CrosKey() = delete;
  CrosKey(::keymaster::AuthorizationSet&& hw_enforced,
          ::keymaster::AuthorizationSet&& sw_enforced,
          const CrosKeyFactory* key_factory,
          KeyData&& key_data);
  ~CrosKey() override;
  // Not copyable nor assignable.
  CrosKey(const CrosKey&) = delete;
  CrosKey& operator=(const CrosKey&) = delete;

  const CrosKeyFactory* cros_key_factory() const {
    return static_cast<const CrosKeyFactory*>(key_factory_);
  }

  const KeyData& key_data() const { return key_data_; }

 protected:
  KeyData key_data_;
};

class ChapsKey : public CrosKey {
 public:
  ChapsKey(::keymaster::AuthorizationSet&& hw_enforced,
           ::keymaster::AuthorizationSet&& sw_enforced,
           const CrosKeyFactory* key_factory,
           KeyData&& key_data);
  ChapsKey(ChapsKey&& chaps_key);
  ~ChapsKey() override;
  ChapsKey& operator=(ChapsKey&&);
  // Not copyable nor assignable.
  ChapsKey(const ChapsKey&) = delete;
  ChapsKey& operator=(const ChapsKey&) = delete;

  // Exports the public/private key in the given format.
  //
  // The only supported format is |KM_KEY_FORMAT_X509| for public keys
  // (SubjectPublicKeyInfo).
  //
  // KeyMint does not own private keys so those can't be exported and an error
  // will be returned.
  keymaster_error_t formatted_key_material(
      keymaster_key_format_t format,
      ::keymaster::UniquePtr<uint8_t[]>* out_material,
      size_t* out_size) const override;

  // Returns key label, corresponding to PKCS#11 CKA_LABEL.
  const std::string& label() const { return key_data().chaps_key().label(); }
  // Returns key ID, corresponding to PKCS#11 CKA_ID.
  brillo::Blob id() const {
    return brillo::Blob(key_data().chaps_key().id().begin(),
                        key_data().chaps_key().id().end());
  }
  // Returns the chaps slot where this key is stored.
  ContextAdaptor::Slot slot() const {
    return static_cast<ContextAdaptor::Slot>(key_data().chaps_key().slot());
  }
};

class CrosOperationFactory : public ::keymaster::OperationFactory {
 public:
  CrosOperationFactory(keymaster_algorithm_t algorithm,
                       keymaster_purpose_t purpose);
  ~CrosOperationFactory() override;
  // Not copyable nor assignable.
  CrosOperationFactory(const CrosOperationFactory&) = delete;
  CrosOperationFactory& operator=(const CrosOperationFactory&) = delete;

  // Informs what type of cryptographic operation this factory can handle.
  KeyType registry_key() const override;

  // Returns a |CrosOperation| for the given key.
  ::keymaster::OperationPtr CreateOperation(
      ::keymaster::Key&& key,
      const ::keymaster::AuthorizationSet& begin_params,
      keymaster_error_t* error) override;

 private:
  keymaster_algorithm_t algorithm_;
  keymaster_purpose_t purpose_;
};

class CrosOperation : public ::keymaster::Operation {
 public:
  explicit CrosOperation(keymaster_purpose_t purpose, ChapsKey&& key);
  CrosOperation() = delete;
  ~CrosOperation() override;
  // Not copyable nor assignable.
  CrosOperation(const CrosOperation&) = delete;
  CrosOperation& operator=(const CrosOperation&) = delete;

  // Begins the operation.
  keymaster_error_t Begin(
      const ::keymaster::AuthorizationSet& /* input_params */,
      ::keymaster::AuthorizationSet* /* output_params */) override;

  // Updates the operation with intermediate input and maybe produces
  // intermediate output.
  keymaster_error_t Update(
      const ::keymaster::AuthorizationSet& /* input_params */,
      const ::keymaster::Buffer& input,
      ::keymaster::AuthorizationSet* /* output_params */,
      ::keymaster::Buffer* /* output */,
      size_t* input_consumed) override;

  // Finishes the operation, possibly given a last piece of input and producing
  // the final output.
  keymaster_error_t Finish(
      const ::keymaster::AuthorizationSet& /* input_params */,
      const ::keymaster::Buffer& input,
      const ::keymaster::Buffer& /* signature */,
      ::keymaster::AuthorizationSet* /* output_params */,
      ::keymaster::Buffer* output) override;

  // Aborts the operation.
  keymaster_error_t Abort() override;

 private:
  std::unique_ptr<CryptoOperation> operation_;
};

}  // namespace arc::keymint::context

#endif  // ARC_KEYMINT_CONTEXT_CROS_KEY_H_
