// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/cryptorecovery/recovery_crypto_tpm2_backend_impl.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <base/check.h>
#include <base/logging.h>
#include <brillo/secure_blob.h>
#include <crypto/scoped_openssl_types.h>
#include <openssl/bn.h>
#include <openssl/ec.h>

#include <libhwsec-foundation/crypto/big_num_util.h>
#include <trunks/authorization_delegate.h>
#include <trunks/error_codes.h>
#include <trunks/hmac_session.h>
#include <trunks/openssl_utility.h>
#include <trunks/policy_session.h>
#include <trunks/tpm_generated.h>
#include <trunks/tpm_utility.h>
#include <trunks/trunks_factory.h>
#include <trunks/trunks_factory_impl.h>

#include "cryptohome/tpm2_impl.h"

using ::hwsec_foundation::BigNumToSecureBlob;
using ::hwsec_foundation::CreateBigNum;
using ::hwsec_foundation::CreateBigNumContext;
using ::hwsec_foundation::EllipticCurve;
using ::hwsec_foundation::ScopedBN_CTX;

namespace cryptohome {
namespace cryptorecovery {
namespace {

std::map<uint32_t, std::string> ToStrPcrMap(
    const std::map<uint32_t, brillo::Blob>& pcr_map) {
  std::map<uint32_t, std::string> str_pcr_map;
  for (const auto& [index, value] : pcr_map) {
    str_pcr_map[index] = brillo::BlobToString(value);
  }
  return str_pcr_map;
}

bool UpdatePolicyPcrOr(const std::string& obfuscated_username,
                       std::string* policy_digest,
                       trunks::PolicySession* policy_session,
                       Tpm2Impl* tpm2_impl) {
  // Obtain the Trunks context for sending TPM commands.
  Tpm2Impl::TrunksClientContext* trunks = nullptr;
  if (!tpm2_impl->GetTrunksContext(&trunks)) {
    LOG(ERROR) << "Failed to get trunks context";
    return false;
  }
  // Calculate policy digests for each of the sets of PCR restrictions
  // separately.
  std::map<uint32_t, brillo::Blob> default_pcr_map =
      tpm2_impl->GetPcrMap(obfuscated_username, /*use_extended_pcr=*/false);
  std::map<uint32_t, std::string> str_default_pcr_map =
      ToStrPcrMap(default_pcr_map);
  std::map<uint32_t, brillo::Blob> extended_pcr_map =
      tpm2_impl->GetPcrMap(obfuscated_username, /*use_extended_pcr=*/true);
  std::map<uint32_t, std::string> str_extended_pcr_map =
      ToStrPcrMap(extended_pcr_map);
  std::vector<std::string> pcr_policy_digests;

  std::string extended_pcr_policy_digest;
  trunks::TPM_RC tpm_result = trunks->tpm_utility->GetPolicyDigestForPcrValues(
      str_extended_pcr_map, false, &extended_pcr_policy_digest);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Error getting extended PCR policy digest: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  pcr_policy_digests.push_back(extended_pcr_policy_digest);

  std::string default_pcr_policy_digest;
  tpm_result = trunks->tpm_utility->GetPolicyDigestForPcrValues(
      str_default_pcr_map, false, &default_pcr_policy_digest);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Error getting default PCR policy digest: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  pcr_policy_digests.push_back(default_pcr_policy_digest);

  // Apply PolicyOR for restricting to the disjunction of the specified sets of
  // PCR restrictions.
  tpm_result = policy_session->PolicyOR(pcr_policy_digests);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Error restricting policy to logical disjunction of PCRs: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  tpm_result = policy_session->GetDigest(policy_digest);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Error getting policy digest: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  return true;
}

}  // namespace

RecoveryCryptoTpm2BackendImpl::RecoveryCryptoTpm2BackendImpl(
    Tpm2Impl* tpm2_impl)
    : tpm2_impl_(tpm2_impl) {
  DCHECK(tpm2_impl_);
}

RecoveryCryptoTpm2BackendImpl::~RecoveryCryptoTpm2BackendImpl() = default;

brillo::SecureBlob RecoveryCryptoTpm2BackendImpl::GenerateKeyAuthValue() {
  return brillo::SecureBlob();
}

bool RecoveryCryptoTpm2BackendImpl::EncryptEccPrivateKey(
    const EncryptEccPrivateKeyRequest& request,
    EncryptEccPrivateKeyResponse* response) {
  DCHECK(response);

  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return false;
  }

  const BIGNUM* own_priv_key_bn =
      EC_KEY_get0_private_key(request.own_key_pair.get());
  if (!own_priv_key_bn) {
    LOG(ERROR) << "Failed to get own_priv_key_bn";
    return false;
  }
  if (!request.ec.IsScalarValid(*own_priv_key_bn)) {
    LOG(ERROR) << "Scalar is not valid";
    return false;
  }
  // Convert own private key to blob.
  brillo::SecureBlob own_priv_key;
  if (!BigNumToSecureBlob(*own_priv_key_bn, request.ec.ScalarSizeInBytes(),
                          &own_priv_key)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return false;
  }

  const EC_POINT* pub_point =
      EC_KEY_get0_public_key(request.own_key_pair.get());
  if (!pub_point) {
    LOG(ERROR) << "Failed to get pub_point";
    return false;
  }
  crypto::ScopedBIGNUM pub_point_x_bn = CreateBigNum(),
                       pub_point_y_bn = CreateBigNum();
  if (!pub_point_x_bn || !pub_point_y_bn) {
    LOG(ERROR) << "Failed to allocate BIGNUM";
    return false;
  }
  if (!request.ec.GetAffineCoordinates(*pub_point, context.get(),
                                       pub_point_x_bn.get(),
                                       pub_point_y_bn.get())) {
    LOG(ERROR) << "Failed to get destination share x coordinate";
    return false;
  }
  brillo::SecureBlob pub_point_x;
  if (!BigNumToSecureBlob(*pub_point_x_bn, MAX_ECC_KEY_BYTES, &pub_point_x)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return false;
  }
  brillo::SecureBlob pub_point_y;
  if (!BigNumToSecureBlob(*pub_point_y_bn, MAX_ECC_KEY_BYTES, &pub_point_y)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return false;
  }

  // Obtain the Trunks context for sending TPM commands.
  Tpm2Impl::TrunksClientContext* trunks = nullptr;
  if (!tpm2_impl_->GetTrunksContext(&trunks)) {
    LOG(ERROR) << "Failed to get trunks context";
    return false;
  }
  // Create the TPM session.
  std::unique_ptr<trunks::HmacSession> hmac_session =
      trunks->factory->GetHmacSession();
  // TODO(b:196192089): set enable_encryption to true
  trunks::TPM_RC tpm_result = hmac_session->StartUnboundSession(
      /*salted=*/true, /*enable_encryption=*/false);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to start TPM session: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  // Translate cryptohome CurveType to trunks curveID
  trunks::TPM_ECC_CURVE tpm_curve_id = trunks::TPM_ECC_NONE;
  switch (request.ec.GetCurveType()) {
    case EllipticCurve::CurveType::kPrime256:
      tpm_curve_id = trunks::TPM_ECC_NIST_P256;
      break;
    case EllipticCurve::CurveType::kPrime384:
      tpm_curve_id = trunks::TPM_ECC_NIST_P384;
      break;
    case EllipticCurve::CurveType::kPrime521:
      tpm_curve_id = trunks::TPM_ECC_NIST_P521;
      break;
  }
  if (tpm_curve_id == trunks::TPM_ECC_NONE) {
    LOG(ERROR) << "Invalid tpm2 curve id";
    return false;
  }

  // Generate policy digest
  std::unique_ptr<trunks::PolicySession> trial_session =
      trunks->factory->GetTrialSession();
  tpm_result = trial_session->StartUnboundSession(
      /*salted=*/false, /*enable_encryption=*/false);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Start unbound session failed: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  std::string policy_digest;
  if (!UpdatePolicyPcrOr(request.obfuscated_username, &policy_digest,
                         trial_session.get(), tpm2_impl_)) {
    LOG(ERROR) << "Get policy digest from PCR map failed.";
    return false;
  }

  // Encrypt its own private key via the TPM2_Import command.
  std::string encrypted_own_priv_key_string;
  tpm_result = trunks->tpm_utility->ImportECCKeyWithPolicyDigest(
      trunks::TpmUtility::AsymmetricKeyUsage::kDecryptKey, tpm_curve_id,
      pub_point_x.to_string(), pub_point_y.to_string(),
      own_priv_key.to_string(), policy_digest, hmac_session->GetDelegate(),
      &encrypted_own_priv_key_string);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to import its own private key into TPM: "
               << trunks::GetErrorString(tpm_result);
    return false;
  }
  // Return the share wrapped with the TPM storage key.
  response->encrypted_own_priv_key =
      brillo::SecureBlob(encrypted_own_priv_key_string);
  return true;
}

crypto::ScopedEC_POINT
RecoveryCryptoTpm2BackendImpl::GenerateDiffieHellmanSharedSecret(
    const GenerateDhSharedSecretRequest& request) {
  ScopedBN_CTX context = CreateBigNumContext();
  if (!context.get()) {
    LOG(ERROR) << "Failed to allocate BN_CTX structure";
    return nullptr;
  }

  // Obtain coordinates of the publisher public point.
  crypto::ScopedBIGNUM others_pub_point_x_bn = CreateBigNum(),
                       others_pub_point_y_bn = CreateBigNum();
  if (!others_pub_point_x_bn || !others_pub_point_y_bn) {
    LOG(ERROR) << "Failed to allocate BIGNUM";
    return nullptr;
  }
  if (!request.ec.GetAffineCoordinates(*request.others_pub_point, context.get(),
                                       others_pub_point_x_bn.get(),
                                       others_pub_point_y_bn.get())) {
    LOG(ERROR) << "Failed to get the other party's public point x coordinate";
    return nullptr;
  }
  brillo::SecureBlob others_pub_point_x;
  if (!BigNumToSecureBlob(*others_pub_point_x_bn, MAX_ECC_KEY_BYTES,
                          &others_pub_point_x)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return nullptr;
  }
  brillo::SecureBlob others_pub_point_y;
  if (!BigNumToSecureBlob(*others_pub_point_y_bn, MAX_ECC_KEY_BYTES,
                          &others_pub_point_y)) {
    LOG(ERROR) << "Failed to convert BIGNUM to SecureBlob";
    return nullptr;
  }

  // Obtain the Trunks context for sending TPM commands.
  Tpm2Impl::TrunksClientContext* trunks = nullptr;
  if (!tpm2_impl_->GetTrunksContext(&trunks)) {
    LOG(ERROR) << "Failed to get trunks context";
    return nullptr;
  }
  // Create the TPM session.
  std::unique_ptr<trunks::HmacSession> hmac_session =
      trunks->factory->GetHmacSession();
  // TODO(b:196192089): set enable_encryption to true
  trunks::TPM_RC tpm_result = hmac_session->StartUnboundSession(
      /*salted=*/true, /*enable_encryption=*/false);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to start TPM session: "
               << trunks::GetErrorString(tpm_result);
    return nullptr;
  }
  // Load the destination share (as a key handle) via the TPM2_Load command.
  trunks::TPM_HANDLE key_handle;
  tpm_result =
      trunks->tpm_utility->LoadKey(request.encrypted_own_priv_key.to_string(),
                                   hmac_session->GetDelegate(), &key_handle);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to load TPM key: "
               << trunks::GetErrorString(tpm_result);
    return nullptr;
  }
  trunks::TPMS_ECC_POINT tpm_others_pub_point = {
      trunks::Make_TPM2B_ECC_PARAMETER(others_pub_point_x.to_string()),
      trunks::Make_TPM2B_ECC_PARAMETER(others_pub_point_y.to_string())};

  // Set PCR value to policy
  std::unique_ptr<trunks::PolicySession> policy_session =
      trunks->factory->GetPolicySession();
  tpm_result = policy_session->StartUnboundSession(
      /*salted=*/true, /*enable_encryption=*/false);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to start policy session: "
               << trunks::GetErrorString(tpm_result);
    return nullptr;
  }
  std::string pcr_value;
  tpm_result = trunks->tpm_utility->ReadPCR(kTpmSingleUserPCR, &pcr_value);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to read PCR value: "
               << trunks::GetErrorString(tpm_result);
    return nullptr;
  }
  tpm_result = trunks->tpm_utility->AddPcrValuesToPolicySession(
      std::map<uint32_t, std::string>({{kTpmSingleUserPCR, pcr_value}}),
      /*use_auth_value=*/false, policy_session.get());
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "Failed to add PCR map: "
               << trunks::GetErrorString(tpm_result);
    return nullptr;
  }

  std::string policy_digest;
  if (!UpdatePolicyPcrOr(request.obfuscated_username, &policy_digest,
                         policy_session.get(), tpm2_impl_)) {
    LOG(ERROR) << "Get policy digest from PCR map failed.";
    return nullptr;
  }

  // Perform the multiplication of the destination share and the other party's
  // public point via the TPM2_ECDH_ZGen command.
  trunks::TPM2B_ECC_POINT tpm_point_dh;
  tpm_result = trunks->tpm_utility->ECDHZGen(
      key_handle, trunks::Make_TPM2B_ECC_POINT(tpm_others_pub_point),
      policy_session->GetDelegate(), &tpm_point_dh);
  if (tpm_result != trunks::TPM_RC_SUCCESS) {
    LOG(ERROR) << "ECDH_ZGen failed: " << trunks::GetErrorString(tpm_result);
    return nullptr;
  }
  // Return the point after converting it from the TPM representation.
  crypto::ScopedEC_POINT point_dh = request.ec.CreatePoint();
  if (!point_dh) {
    LOG(ERROR) << "Failed to allocate EC_POINT";
    return nullptr;
  }
  if (!trunks::TpmToOpensslEccPoint(tpm_point_dh.point, *request.ec.GetGroup(),
                                    point_dh.get())) {
    LOG(ERROR) << "TPM ECC point conversion failed";
    return nullptr;
  }
  return point_dh;
}

// Generating Rsa Key is only required for TPM1, so the
// implementation of TPM2 would return a dummy true.
bool RecoveryCryptoTpm2BackendImpl::GenerateRsaKeyPair(
    brillo::SecureBlob* /*encrypted_rsa_private_key*/,
    brillo::SecureBlob* /*rsa_public_key_spki_der*/) {
  return true;
}

// Signing the request payload is only required for TPM1, so the
// implementation of TPM2 would return a dummy true.
bool RecoveryCryptoTpm2BackendImpl::SignRequestPayload(
    const brillo::SecureBlob& /*encrypted_rsa_private_key*/,
    const brillo::SecureBlob& /*request_payload*/,
    brillo::SecureBlob* /*signature*/) {
  return true;
}

}  // namespace cryptorecovery
}  // namespace cryptohome
