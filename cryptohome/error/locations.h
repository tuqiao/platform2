// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_ERROR_LOCATIONS_H_
#define CRYPTOHOME_ERROR_LOCATIONS_H_

#include "cryptohome/error/cryptohome_error.h"

namespace cryptohome {

namespace error {

// This file defines the various location code used by CryptohomeError
// Each of the location should only be used in one error site.

// This file is generated and maintained by the cryptohome/error/location_db.py
// utility. Please run this command in cros_sdk to update this file:
// $ /mnt/host/source/src/platform2/cryptohome/error/tool/location_db.py
//       --update

// Note that we should prevent the implicit cast of this enum class to
// ErrorLocation so that if the macro is not used, the compiler will catch it.
enum class ErrorLocationSpecifier : CryptohomeError::ErrorLocation {
  // Start of generated content. Do NOT modify after this line.
  /* ./user_session/real_user_session.cc */
  kLocUserSessionMountEphemeralFailed = 100,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountGuestMountPointBusy = 101,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountGuestNoGuestSession = 102,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountGuestSessionMountFailed = 103,
  /* ./userdataauth.cc */
  kLocUserDataAuthNoEphemeralMountForOwner = 104,
  /* ./userdataauth.cc */
  kLocUserDataAuthEphemeralMountWithoutCreate = 105,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountAuthSessionNotFound = 106,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountAuthSessionNotAuthed = 107,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountNoAccountID = 108,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountCantGetPublicMountSalt = 109,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountNoKeySecret = 110,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountCreateNoKey = 111,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountCreateMultipleKey = 112,
  /* ./userdataauth.cc */
  kLocUserDataAuthMountCreateKeyNotSpecified = 113,
  // End of generated content.
};
// The enum value should not exceed 65535, otherwise we need to adjust the way
// Unified Error Code is allocated in cryptohome/error/cryptohome_tpm_error.h
// and libhwsec/error/tpm_error.h so that “Cryptohome.Error.LeafErrorWithTPM”
// UMA will continue to work.

}  // namespace error

}  // namespace cryptohome

#endif  // CRYPTOHOME_ERROR_LOCATIONS_H_
