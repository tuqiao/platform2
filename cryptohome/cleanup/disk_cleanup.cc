// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/cleanup/disk_cleanup.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <base/logging.h>
#include <base/time/time.h>
#include <base/timer/elapsed_timer.h>

#include "cryptohome/cleanup/disk_cleanup_routines.h"
#include "cryptohome/cleanup/user_oldest_activity_timestamp_manager.h"
#include "cryptohome/cryptohome_metrics.h"
#include "cryptohome/filesystem_layout.h"
#include "cryptohome/platform.h"
#include "cryptohome/storage/homedirs.h"

namespace cryptohome {

DiskCleanup::DiskCleanup(Platform* platform,
                         HomeDirs* homedirs,
                         UserOldestActivityTimestampManager* timestamp_manager)
    : platform_(platform),
      homedirs_(homedirs),
      timestamp_manager_(timestamp_manager),
      routines_(std::make_unique<DiskCleanupRoutines>(homedirs_, platform_)) {}

std::optional<int64_t> DiskCleanup::AmountOfFreeDiskSpace() const {
  int64_t free_space = platform_->AmountOfFreeDiskSpace(ShadowRoot());

  if (free_space < 0) {
    return std::nullopt;
  } else {
    return free_space;
  }
}

DiskCleanup::FreeSpaceState DiskCleanup::GetFreeDiskSpaceState() const {
  return GetFreeDiskSpaceState(AmountOfFreeDiskSpace());
}

DiskCleanup::FreeSpaceState DiskCleanup::GetFreeDiskSpaceState(
    std::optional<int64_t> free_disk_space) const {
  if (!free_disk_space) {
    return DiskCleanup::FreeSpaceState::kError;
  }

  int64_t value = free_disk_space.value();
  if (value >= target_free_space_) {
    return DiskCleanup::FreeSpaceState::kAboveTarget;
  } else if (value >= normal_cleanup_threshold_) {
    return DiskCleanup::FreeSpaceState::kAboveThreshold;
  } else if (value >= aggressive_cleanup_threshold_) {
    return DiskCleanup::FreeSpaceState::kNeedNormalCleanup;
  } else {
    return DiskCleanup::FreeSpaceState::kNeedAggressiveCleanup;
  }
}

bool DiskCleanup::HasTargetFreeSpace() const {
  return GetFreeDiskSpaceState() == DiskCleanup::FreeSpaceState::kAboveTarget;
}

bool DiskCleanup::IsFreeableDiskSpaceAvailable() {
  if (!homedirs_->enterprise_owned())
    return false;

  const auto homedirs = homedirs_->GetHomeDirs();

  int unmounted_cryptohomes =
      std::count_if(homedirs.begin(), homedirs.end(),
                    [](auto& dir) { return !dir.is_mounted; });

  return unmounted_cryptohomes > 0;
}

bool DiskCleanup::FreeDiskSpace() {
  auto free_space = AmountOfFreeDiskSpace();

  switch (GetFreeDiskSpaceState(free_space)) {
    case DiskCleanup::FreeSpaceState::kAboveTarget:
    case DiskCleanup::FreeSpaceState::kAboveThreshold:
      // Already have enough space. No need to clean up.
      VLOG(1) << "Skipping cleanup with " << *free_space << " space available";
      ReportDiskCleanupResult(DiskCleanupResult::kDiskCleanupSkip);
      return true;

    case DiskCleanup::FreeSpaceState::kNeedNormalCleanup:
    case DiskCleanup::FreeSpaceState::kNeedAggressiveCleanup:
      // Trigger cleanup.
      VLOG(1) << "Starting cleanup with " << *free_space << " space available";
      break;

    case DiskCleanup::FreeSpaceState::kError:
      LOG(ERROR) << "Failed to get the amount of free disk space";
      return false;
    default:
      LOG(ERROR) << "Unhandled free disk state";
      return false;
  }

  auto now = platform_->GetCurrentTime();

  if (last_free_disk_space_) {
    auto diff = now - *last_free_disk_space_;

    ReportTimeBetweenFreeDiskSpace(diff.InSeconds());
  }

  last_free_disk_space_ = now;

  base::ElapsedTimer total_timer;

  bool result = FreeDiskSpaceInternal();

  if (result) {
    ReportDiskCleanupResult(DiskCleanupResult::kDiskCleanupSuccess);
  } else {
    ReportDiskCleanupResult(DiskCleanupResult::kDiskCleanupError);
  }

  int cleanup_time = total_timer.Elapsed().InMilliseconds();
  ReportFreeDiskSpaceTotalTime(cleanup_time);
  VLOG(1) << "Disk cleanup took " << cleanup_time << "ms.";

  auto after_cleanup = AmountOfFreeDiskSpace();
  if (!after_cleanup) {
    LOG(ERROR) << "Failed to get the amount of free disk space";
    return false;
  }

  auto cleaned_in_mb =
      MAX(0, after_cleanup.value() - free_space.value()) / 1024 / 1024;
  ReportFreeDiskSpaceTotalFreedInMb(cleaned_in_mb);

  VLOG(1) << "Disk cleanup cleared " << cleaned_in_mb << "MB.";

  LOG(INFO) << "Disk cleanup complete.";

  return result;
}

void DiskCleanup::set_routines_for_testing(DiskCleanupRoutines* routines) {
  routines_.reset(routines);
}

bool DiskCleanup::FreeDiskSpaceInternal() {
  // If ephemeral users are enabled, remove all cryptohomes except those
  // currently mounted or belonging to the owner.
  // |AreEphemeralUsers| will reload the policy to guarantee freshness.
  if (homedirs_->AreEphemeralUsersEnabled()) {
    homedirs_->RemoveNonOwnerCryptohomes();

    ReportDiskCleanupProgress(
        DiskCleanupProgress::kEphemeralUserProfilesCleaned);
    return true;
  }

  auto homedirs = homedirs_->GetHomeDirs();
  auto unmounted_homedirs = homedirs;
  FilterMountedHomedirs(&unmounted_homedirs);

  std::sort(
      unmounted_homedirs.begin(), unmounted_homedirs.end(),
      [&](const HomeDirs::HomeDir& a, const HomeDirs::HomeDir& b) {
        return timestamp_manager_->GetLastUserActivityTimestamp(a.obfuscated) >
               timestamp_manager_->GetLastUserActivityTimestamp(b.obfuscated);
      });

  auto normal_cleanup_homedirs = unmounted_homedirs;

  if (last_normal_disk_cleanup_complete_) {
    base::Time cutoff = last_normal_disk_cleanup_complete_.value();
    FilterHomedirsProcessedBeforeCutoff(cutoff, &normal_cleanup_homedirs);
  }

  bool result = true;

  // Clean Cache directories for every unmounted user that has logged out after
  // the last normal cleanup happened.
  for (auto dir = normal_cleanup_homedirs.rbegin();
       dir != normal_cleanup_homedirs.rend(); dir++) {
    if (!routines_->DeleteUserCache(dir->obfuscated))
      result = false;

    if (HasTargetFreeSpace()) {
      ReportDiskCleanupProgress(
          DiskCleanupProgress::kBrowserCacheCleanedAboveTarget);
      return result;
    }
  }

  auto free_disk_space = AmountOfFreeDiskSpace();
  if (!free_disk_space) {
    LOG(ERROR) << "Failed to get the amount of free space";
    return false;
  }

  // Clean GCache directories for every unmounted user that has logged out after
  // after the last normal cleanup happened.
  for (auto dir = normal_cleanup_homedirs.rbegin();
       dir != normal_cleanup_homedirs.rend(); dir++) {
    if (!routines_->DeleteUserGCache(dir->obfuscated))
      result = false;

    if (HasTargetFreeSpace()) {
      ReportDiskCleanupProgress(
          DiskCleanupProgress::kGoogleDriveCacheCleanedAboveTarget);
      return result;
    }
  }

  auto old_free_disk_space = free_disk_space;
  free_disk_space = AmountOfFreeDiskSpace();
  if (!free_disk_space) {
    LOG(ERROR) << "Failed to get the amount of free space";
    return false;
  }

  const int64_t freed_gcache_space =
      free_disk_space.value() - old_free_disk_space.value();
  // Report only if something was deleted.
  if (freed_gcache_space > 0) {
    ReportFreedGCacheDiskSpaceInMb(freed_gcache_space / 1024 / 1024);
  }

  free_disk_space = AmountOfFreeDiskSpace();
  if (!free_disk_space) {
    LOG(ERROR) << "Failed to get the amount of free space";
    return false;
  }

  bool early_stop = false;

  // Purge Dmcrypt cache vaults.
  for (auto dir = normal_cleanup_homedirs.rbegin();
       dir != normal_cleanup_homedirs.rend(); dir++) {
    if (!routines_->DeleteCacheVault(dir->obfuscated))
      result = false;

    if (HasTargetFreeSpace()) {
      early_stop = true;
      break;
    }
  }

  old_free_disk_space = free_disk_space;
  free_disk_space = AmountOfFreeDiskSpace();
  if (!free_disk_space) {
    LOG(ERROR) << "Failed to get the amount of free space";
    return false;
  }

  const int64_t freed_vault_cache_space =
      free_disk_space.value() - old_free_disk_space.value();
  // Report only if something was deleted.
  if (freed_gcache_space > 0) {
    ReportFreedCacheVaultDiskSpaceInMb(freed_vault_cache_space / 1024 / 1024);
  }

  if (!early_stop)
    last_normal_disk_cleanup_complete_ = platform_->GetCurrentTime();

  switch (GetFreeDiskSpaceState(free_disk_space)) {
    case DiskCleanup::FreeSpaceState::kAboveTarget:
      ReportDiskCleanupProgress(
          DiskCleanupProgress::kCacheVaultsCleanedAboveTarget);
      return result;
    case DiskCleanup::FreeSpaceState::kAboveThreshold:
    case DiskCleanup::FreeSpaceState::kNeedNormalCleanup:
      ReportDiskCleanupProgress(
          DiskCleanupProgress::kCacheVaultsCleanedAboveMinimum);
      return result;
    case DiskCleanup::FreeSpaceState::kNeedAggressiveCleanup:
      // continue cleanup
      break;
    case DiskCleanup::FreeSpaceState::kError:
      LOG(ERROR) << "Failed to get the amount of free space";
      return false;
    default:
      LOG(ERROR) << "Unhandled free disk state";
      return false;
  }

  auto aggressive_cleanup_homedirs = unmounted_homedirs;

  if (last_aggressive_disk_cleanup_complete_) {
    base::Time cutoff = last_aggressive_disk_cleanup_complete_.value();
    FilterHomedirsProcessedBeforeCutoff(cutoff, &aggressive_cleanup_homedirs);
  }

  // Clean Android cache directories for every unmounted user that has logged
  // out after after the last normal cleanup happened.
  for (auto dir = aggressive_cleanup_homedirs.rbegin();
       dir != aggressive_cleanup_homedirs.rend(); dir++) {
    if (!routines_->DeleteUserAndroidCache(dir->obfuscated))
      result = false;

    if (HasTargetFreeSpace()) {
      early_stop = true;
      break;
    }
  }

  if (!early_stop)
    last_aggressive_disk_cleanup_complete_ = platform_->GetCurrentTime();

  switch (GetFreeDiskSpaceState()) {
    case DiskCleanup::FreeSpaceState::kAboveTarget:
      ReportDiskCleanupProgress(
          DiskCleanupProgress::kAndroidCacheCleanedAboveTarget);
      return result;
    case DiskCleanup::FreeSpaceState::kAboveThreshold:
    case DiskCleanup::FreeSpaceState::kNeedNormalCleanup:
      ReportDiskCleanupProgress(
          DiskCleanupProgress::kAndroidCacheCleanedAboveMinimum);
      return result;
    case DiskCleanup::FreeSpaceState::kNeedAggressiveCleanup:
      // continue cleanup
      break;
    case DiskCleanup::FreeSpaceState::kError:
      LOG(ERROR) << "Failed to get the amount of free space";
      return false;
    default:
      LOG(ERROR) << "Unhandled free disk state";
      return false;
  }

  // Delete old users, the oldest first. Count how many are deleted.
  // Don't delete anyone if we don't know who the owner is.
  // For consumer devices, don't delete the device owner. Enterprise-enrolled
  // devices have no owner, so don't delete the most-recent user.
  int deleted_users_count = 0;
  std::string owner;
  if (!homedirs_->enterprise_owned() && !homedirs_->GetOwner(&owner))
    return result;

  int mounted_cryptohomes_count =
      std::count_if(homedirs.begin(), homedirs.end(),
                    [](auto& dir) { return dir.is_mounted; });

  for (auto dir = unmounted_homedirs.rbegin(); dir != unmounted_homedirs.rend();
       dir++) {
    if (homedirs_->enterprise_owned()) {
      // Leave the most-recent user on the device intact.
      // The most-recent user is the first in unmounted_homedirs.
      if (dir == unmounted_homedirs.rend() - 1 &&
          mounted_cryptohomes_count == 0) {
        LOG(INFO) << "Skipped deletion of the most recent device user.";
        continue;
      }
    } else if (dir->obfuscated == owner) {
      // We never delete the device owner.
      LOG(INFO) << "Skipped deletion of the device owner.";
      continue;
    }

    auto before_cleanup = AmountOfFreeDiskSpace();
    if (!before_cleanup) {
      LOG(ERROR) << "Failed to get the amount of free space";
      return false;
    }

    LOG(INFO) << "Freeing disk space by deleting user " << dir->obfuscated;
    if (!routines_->DeleteUserProfile(dir->obfuscated))
      result = false;
    timestamp_manager_->RemoveUser(dir->obfuscated);
    ++deleted_users_count;

    auto after_cleanup = AmountOfFreeDiskSpace();
    if (!after_cleanup) {
      LOG(ERROR) << "Failed to get the amount of free space";
      return false;
    }

    auto cleaned_in_mb =
        MAX(0, after_cleanup.value() - before_cleanup.value()) / 1024 / 1024;
    LOG(INFO) << "Removing user " << dir->obfuscated << " freed "
              << cleaned_in_mb << " MiB";

    if (HasTargetFreeSpace())
      break;
  }

  if (deleted_users_count > 0) {
    ReportDeletedUserProfiles(deleted_users_count);
  }

  // We had a chance to delete a user only if any unmounted homes existed.
  if (unmounted_homedirs.size() > 0) {
    ReportDiskCleanupProgress(
        HasTargetFreeSpace()
            ? DiskCleanupProgress::kWholeUserProfilesCleanedAboveTarget
            : DiskCleanupProgress::kWholeUserProfilesCleaned);
  } else {
    ReportDiskCleanupProgress(DiskCleanupProgress::kNoUnmountedCryptohomes);
  }

  return result;
}

void DiskCleanup::FilterMountedHomedirs(
    std::vector<HomeDirs::HomeDir>* homedirs) {
  homedirs->erase(std::remove_if(homedirs->begin(), homedirs->end(),
                                 [](const HomeDirs::HomeDir& dir) {
                                   return dir.is_mounted;
                                 }),
                  homedirs->end());
}

void DiskCleanup::FilterHomedirsProcessedBeforeCutoff(
    base::Time cutoff, std::vector<HomeDirs::HomeDir>* homedirs) {
  homedirs->erase(
      std::remove_if(homedirs->begin(), homedirs->end(),
                     [&](const HomeDirs::HomeDir& dir) {
                       return timestamp_manager_->GetLastUserActivityTimestamp(
                                  dir.obfuscated) < cutoff;
                     }),
      homedirs->end());
}

}  // namespace cryptohome
