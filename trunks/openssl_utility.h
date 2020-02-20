// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TRUNKS_OPENSSL_UTILITY_H_
#define TRUNKS_OPENSSL_UTILITY_H_

#include <openssl/ec.h>

#include "trunks/tpm_generated.h"
#include "trunks/trunks_export.h"

namespace trunks {

// Converts the ECC |point| in the TPMS_ECC_POINT format to the OpenSSL EC_POINT
// format with the given curve group |ec_group|. If succeeded, stores the result
// in |ec_point| and returns true; otherwise, returns false.
//
// |ec_group| and |ec_point| should already be initialized (e.g., by calling
// EC_GROUP_new() and EC_POINT_new()).
TRUNKS_EXPORT bool TpmToOpensslEccPoint(const TPMS_ECC_POINT& point,
                                        const EC_GROUP* ec_group,
                                        EC_POINT* ec_point);

// Converts the ECC |point| in the OpenSSL EC_POINT format to the TPMS_ECC_POINT
// format with the given curve group |ec_group|. If succeeded, stores the result
// in |ecc_point| and returns true; otherwise, returns false.
//
// |ec_group| and |point| should already be initialized (e.g., by calling
// EC_GROUP_new() and EC_POINT_new()). |ecc_point| should be initialized too.
TRUNKS_EXPORT bool OpensslToTpmEccPoint(const EC_GROUP* ec_group,
                                        const EC_POINT* point,
                                        TPMS_ECC_POINT* ecc_point);

}  // namespace trunks

#endif  // TRUNKS_OPENSSL_UTILITY_H_
