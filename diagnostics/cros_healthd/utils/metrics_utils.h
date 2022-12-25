// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_CROS_HEALTHD_UTILS_METRICS_UTILS_H_
#define DIAGNOSTICS_CROS_HEALTHD_UTILS_METRICS_UTILS_H_

#include <set>

#include "diagnostics/mojom/public/cros_healthd_probe.mojom.h"

namespace diagnostics {

// These values are logged to UMA. Entries should not be renumbered and
// numeric values should never be reused. Please keep in sync with
// "CrosHealthdTelemetryResult" in tools/metrics/histograms/enums.xml in the
// Chromium repo.
enum class CrosHealthdTelemetryResult {
  kSuccess = 0,
  kError = 1,
  // A special enumerator that must share the highest enumerator value. This
  // value is required when calling |SendEnumToUMA|.
  kMaxValue = kError,
};

// Sends the telemetry result (e.g., success or error) to UMA for each category
// in |requested_categories|.
void SendTelemetryResultToUMA(
    const std::set<ash::cros_healthd::mojom::ProbeCategoryEnum>&
        requested_categories,
    const ash::cros_healthd::mojom::TelemetryInfoPtr& info);

}  // namespace diagnostics

#endif  // DIAGNOSTICS_CROS_HEALTHD_UTILS_METRICS_UTILS_H_
