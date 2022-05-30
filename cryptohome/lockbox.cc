// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/lockbox.h"

#include <arpa/inet.h>
#include <limits.h>
#include <openssl/sha.h>
#include <stdint.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <base/check.h>
#include <base/check_op.h>
#include <base/files/file_path.h>
#include <base/logging.h>
#include <base/notreached.h>
#include <base/strings/string_split.h>
#include <base/sys_byteorder.h>
#include <base/threading/platform_thread.h>
#include <base/time/time.h>
#include <brillo/secure_blob.h>
#include <libhwsec/status.h>
#include <libhwsec-foundation/crypto/secure_blob_util.h>
#include <libhwsec-foundation/crypto/sha.h>

#include "cryptohome/platform.h"

using base::FilePath;
using brillo::SecureBlob;
using hwsec::TPMErrorBase;
using hwsec_foundation::SecureBlobToHex;
using hwsec_foundation::Sha256;

namespace cryptohome {

std::ostream& operator<<(std::ostream& out, LockboxError error) {
  return out << static_cast<int>(error);
}

Lockbox::Lockbox(Tpm* tpm, uint32_t nvram_index)
    : tpm_(tpm), nvram_index_(nvram_index) {}

Lockbox::~Lockbox() {}

bool Lockbox::Reset(LockboxError* error) {
  if (!tpm_ || !tpm_->IsEnabled() || !tpm_->IsOwned()) {
    *error = LockboxError::kTpmUnavailable;
    LOG(ERROR) << "TPM unavailable";
    return false;
  }

  // If we have authorization, recreate the lockbox space.
  brillo::SecureBlob owner_password;
  if (tpm_->IsOwnerPasswordPresent()) {
    if (tpm_->IsNvramDefined(nvram_index_) &&
        !tpm_->DestroyNvram(nvram_index_)) {
      LOG(ERROR) << "Failed to destroy lockbox data before creation.";
      *error = LockboxError::kTpmError;
      return false;
    }

    // If we store the encryption salt in lockbox, protect it from reading
    // in non-verified boot mode.
    uint32_t nvram_perm =
        Tpm::kTpmNvramWriteDefine |
        (IsKeyMaterialInLockbox() ? Tpm::kTpmNvramBindToPCR0 : 0);
    uint32_t nvram_bytes = LockboxContents::kNvramSize;
    if (!tpm_->DefineNvram(nvram_index_, nvram_bytes, nvram_perm)) {
      *error = LockboxError::kTpmError;
      LOG(ERROR) << "Failed to define NVRAM space.";
      return false;
    }
    LOG(INFO) << "Lockbox created.";
    return true;
  } else {
    LOG(WARNING) << "No owner password when trying to reset LockBox.";
  }

  // Check if the space is already set up correctly.
  if (!tpm_->IsNvramDefined(nvram_index_)) {
    LOG(ERROR) << "NVRAM space absent when resetting LockBox.";
    *error = LockboxError::kNvramSpaceAbsent;
    return false;
  }

  if (tpm_->IsNvramLocked(nvram_index_)) {
    LOG(ERROR) << "NVRAM space locked after resetting LockBox.";
    *error = LockboxError::kNvramInvalid;
    return false;
  }

  // The NVRAM space that we are looking at is not created by us, and it is too
  // expensive to extensively inspect it. Given the above, we aren't sure about
  // all its attributes, all we know is that:
  // 1. It's not locked.
  // 2. It exists (is defined).
  // Therefore, it is highly likely that the NVRAM space is writable, and
  // suitable for our use case. The most probable scenario is that this NVRAM
  // index is created by previous installations of Chromium OS, so we'll just
  // continue to use it.
  LOG(INFO) << "Existing Lockbox seems writable.";
  return true;
}

bool Lockbox::Store(const brillo::Blob& blob, LockboxError* error) {
  if (!tpm_ || !tpm_->IsEnabled()) {
    *error = LockboxError::kTpmUnavailable;
    LOG(ERROR) << "TPM unavailable";
    return false;
  }

  if (!tpm_->IsNvramDefined(nvram_index_) ||
      tpm_->IsNvramLocked(nvram_index_)) {
    *error = LockboxError::kNvramInvalid;
    return false;
  }

  // Check defined NVRAM size and construct a LockboxContents instance.
  std::unique_ptr<LockboxContents> contents = LockboxContents::New();
  unsigned int nvram_size = tpm_->GetNvramSize(nvram_index_);
  if (nvram_size != LockboxContents::kNvramSize) {
    LOG(ERROR) << "Unsupported NVRAM space size " << nvram_size << ".";
    *error = LockboxError::kNvramInvalid;
    return false;
  }

  // Grab key material from the TPM.
  brillo::SecureBlob key_material(contents->key_material_size());
  if (IsKeyMaterialInLockbox()) {
    if (hwsec::Status err =
            tpm_->GetRandomDataSecureBlob(key_material.size(), &key_material);
        !err.ok()) {
      LOG(ERROR) << "Failed to get key material from the TPM: " << err;
      *error = LockboxError::kTpmError;
      return false;
    }
  } else {
    // Save a TPM command, and just fill the salt field with zeroes.
    LOG(INFO) << "Skipping random salt generation.";
  }

  brillo::SecureBlob nvram_blob;
  if (!contents->SetKeyMaterial(key_material) || !contents->Protect(blob) ||
      !contents->Encode(&nvram_blob)) {
    LOG(ERROR) << "Failed to set up lockbox contents.";
    *error = LockboxError::kNvramInvalid;
    return false;
  }

  // Write the hash to nvram
  if (!tpm_->WriteNvram(nvram_index_,
                        SecureBlob(nvram_blob.begin(), nvram_blob.end()))) {
    LOG(ERROR) << "Store() failed to write the attribute hash to NVRAM";
    *error = LockboxError::kTpmError;
    return false;
  }
  // Lock nvram index for writing.
  if (!tpm_->WriteLockNvram(nvram_index_)) {
    LOG(ERROR) << "Store() failed to lock the NVRAM space";
    *error = LockboxError::kTpmError;
    return false;
  }
  // Ensure the space is now locked.
  if (!tpm_->IsNvramLocked(nvram_index_)) {
    LOG(ERROR) << "NVRAM space did not lock as expected.";
    *error = LockboxError::kTpmError;
    return false;
  }

  return true;
}

// static
std::unique_ptr<LockboxContents> LockboxContents::New() {
  std::unique_ptr<LockboxContents> result(new LockboxContents());
  result->key_material_.resize(kKeyMaterialSize);
  return result;
}

bool LockboxContents::Decode(const brillo::SecureBlob& nvram_data) {
  // Reject data of incorrect size.
  if (nvram_data.size() != kNvramSize) {
    return false;
  }

  brillo::SecureBlob::const_iterator cursor = nvram_data.begin();

  // Extract the expected data size from the NVRAM. For historic reasons, this
  // is encoded in reverse host byte order (!).
  uint32_t reversed_size;
  uint8_t* reversed_size_ptr = reinterpret_cast<uint8_t*>(&reversed_size);
  std::copy(cursor, cursor + sizeof(reversed_size), reversed_size_ptr);
  cursor += sizeof(reversed_size);
  size_ = base::ByteSwap(reversed_size);

  // Grab the flags.
  flags_ = *cursor++;

  // Grab the key material.
  key_material_.assign(cursor, cursor + key_material_size());
  cursor += key_material_size();

  // Grab the hash.
  std::copy(cursor, cursor + sizeof(hash_), hash_);
  cursor += sizeof(hash_);

  // Per the checks at function entry we should have exactly reached the end.
  CHECK(cursor == nvram_data.end());

  return true;
}

bool LockboxContents::Encode(brillo::SecureBlob* blob) {
  // Encode the data size. For historic reasons, this is encoded in reverse host
  // byte order (!).
  uint32_t reversed_size = base::ByteSwap(size_);
  uint8_t* reversed_size_ptr = reinterpret_cast<uint8_t*>(&reversed_size);
  blob->insert(blob->end(), reversed_size_ptr,
               reversed_size_ptr + sizeof(reversed_size));

  // Append the flags byte.
  blob->push_back(flags_);

  // Append the key material.
  blob->insert(blob->end(), std::begin(key_material_), std::end(key_material_));

  // Append the hash.
  blob->insert(blob->end(), std::begin(hash_), std::end(hash_));

  return true;
}

bool LockboxContents::SetKeyMaterial(const brillo::SecureBlob& key_material) {
  if (key_material.size() != key_material_size()) {
    return false;
  }

  key_material_ = key_material;
  return true;
}

bool LockboxContents::Protect(const brillo::Blob& blob) {
  brillo::SecureBlob salty_blob(blob);
  salty_blob.insert(salty_blob.end(), key_material_.begin(),
                    key_material_.end());
  SecureBlob salty_blob_hash = Sha256(salty_blob);
  CHECK_EQ(sizeof(hash_), salty_blob_hash.size());
  std::copy(salty_blob_hash.begin(), salty_blob_hash.end(), std::begin(hash_));
  size_ = blob.size();
  return true;
}

LockboxContents::VerificationResult LockboxContents::Verify(
    const brillo::Blob& blob) {
  // Make sure that the file size matches what was stored in nvram.
  if (blob.size() != size_) {
    LOG(ERROR) << "Verify() expected " << size_ << " , but received "
               << blob.size() << " bytes.";
    return VerificationResult::kSizeMismatch;
  }

  // Compute the hash of the blob to verify.
  brillo::SecureBlob salty_blob(blob);
  salty_blob.insert(salty_blob.end(), key_material_.begin(),
                    key_material_.end());
  SecureBlob salty_blob_hash = Sha256(salty_blob);

  // Validate the blob hash versus the stored hash.
  if (sizeof(hash_) != salty_blob_hash.size() ||
      brillo::SecureMemcmp(hash_, salty_blob_hash.data(), sizeof(hash_))) {
    LOG(ERROR) << "Verify() hash mismatch!";
    return VerificationResult::kHashMismatch;
  }

  return VerificationResult::kValid;
}

}  // namespace cryptohome
