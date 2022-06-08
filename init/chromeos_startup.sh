#!/bin/sh

# Copyright 2012 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

. /usr/share/misc/chromeos-common.sh
. /usr/share/misc/lvm-utils.sh

# UNDO_MOUNTS stores the mounts that have currently been mounted. In case the
# startup operations fail (from unrecoverable state on disk), the mounts
# in UNDO_MOUNTS are unmounted in reverse order.
UNDO_MOUNTS=

# ENCRYPTED_STATEFUL_MNT stores the path to the initial mount point for the
# encrypted stateful partition.
ENCRYPTED_STATEFUL_MNT="/mnt/stateful_partition/encrypted"

# Flag file indicating that mount encrypted stateful failed last time.
# If the file is present and mount_encrypted failed again, machine would enter
# self-repair mode.
MOUNT_ENCRYPTED_FAILED_FILE="/mnt/stateful_partition/mount_encrypted_failed"

# USE_ENCRYPTED_REBOOT_VAULT determines whether the encrypted reboot vault
# should be created/mounted.
USE_ENCRYPTED_REBOOT_VAULT=1

# USE_LVM_STATEFUL_PARTITION determines whether the device should attempt
# to use the new LVM stateful partition format.
USE_LVM_STATEFUL_PARTITION=0

# TMPFILES_LOG defines the path that tmpfiles.d errors will be written to for
# debugging failures.
TMPFILES_LOG="/run/tmpfiles.log"

# Unmounts the incomplete mount setup during the failure path. Failure to
# set up mounts in this script result in the entire stateful partition getting
# wiped using clobber-state.
cleanup_mounts() {
  # On failure unmount all saved mount points and repair stateful
  for mount_point in ${UNDO_MOUNTS}; do
    if [ "${mount_point}" = "${ENCRYPTED_STATEFUL_MNT}" ]; then
      do_umount_var_and_home_chronos
    else
      umount -n "${mount_point}"
    fi
  done
  exit 1
}

# Adds mounts to UNDO_MOUNT.
remember_mount() {
    UNDO_MOUNTS="$1 ${UNDO_MOUNTS}"
}

# Used to mount essential mount points for the system from the stateful
# or encrypted stateful partition.
# On failure, clobbers the stateful partition.
mount_or_fail() {
  local mount_point
  # -c: Never canonicalize: it is a hazard to resolve symlinks.
  # -n: Do not write to mtab: we don't use it.
  if mount -c -n "$@" ; then
    # Last parameter contains the mount point
    shift "$(( $# - 1 ))"
    # Push it on the undo stack if we fail later
    remember_mount "$1"
    return
  fi
  cleanup_mounts "failed to mount $*"
}

# Assert that the argument is a directory.
# On failure, clobbers the stateful partition.
check_directory() {
  local path="$1"
  if [ -L "${path}" ] || [ ! -d "${path}" ]; then
    cleanup_mounts "${path} is not a directory"
  fi
}

# Returns if the TPM is owned or couldn't determine.
is_tpm_owned() {
  local tpm_owned
  # Depending on the kernel version, the file containing tpm owned information
  # can be in one of two locations. Specifically, for kernel versions 3.10 and
  # 3.14 the folder misc is used (/sys/class/misc/tpm0/device/owned). Starting
  # from version 3.18 the folder tpm is used.
  if [ -e /sys/class/misc/tpm0/device/owned ]; then
    tpm_owned="$(cat /sys/class/misc/tpm0/device/owned)"
  else
    tpm_owned="$(cat /sys/class/tpm/tpm0/device/owned)"
  fi
  [ "${tpm_owned}" != "0" ]
}

# Some startup functions are split into a separate library which may be
# different for different targets (e.g., regular Chrome OS vs. embedded).
. /usr/share/cros/startup_utils.sh

# CROS_DEBUG equals one if we've booted in developer mode or we've
# booted a developer image.
crossystem "cros_debug?1"
CROS_DEBUG="$((! $?))"

# Developer mode functions (defined in dev_utils.sh and will be loaded
# only when CROS_DEBUG=1).
dev_mount_packages() { true; }
dev_is_debug_build() { false; }
dev_pop_paths_to_preserve() { true; }
dev_update_stateful_partition() { true; }

# do_* are wrapper functions that may be redefined in developer mode or test
# images. Find more implementation in {dev,test,factory}_utils.sh.
do_umount_var_and_home_chronos() { umount_var_and_home_chronos; }

if [ "${CROS_DEBUG}" -eq 1 ]; then
  . /usr/share/cros/dev_utils.sh
fi

# Prepare to mount stateful partition
ROOT_DEV="$(rootdev -s)"
ROOTDEV_RET_CODE=$?
ROOT_DEV_DISK="$(rootdev -d -s)"
# Example root dev types we need to handle: /dev/sda2 -> /dev/sda,
# /dev/mmcblk0p0 -> /dev/mmcblk0p, /dev/ubi2_1 -> /dev/ubi
ROOTDEV_TYPE="$(echo "${ROOT_DEV}" | sed 's/[0-9_]*$//')"
ROOTDEV_NAME="${ROOT_DEV_DISK##/dev/}"
ROOTDEV_REMOVABLE="$(cat "/sys/block/${ROOTDEV_NAME}/removable")"

# Load the GPT helper functions and the image settings.
. "/usr/sbin/write_gpt.sh"
if [ "${ROOTDEV_REMOVABLE}" = "1" ]; then
  load_partition_vars
else
  load_base_vars
fi

# Path to the securityfs directory for configuring inode security policies.
LSM_INODE_POLICIES="/sys/kernel/security/chromiumos/inode_security_policies"

# Block symlink and FIFO access on the given path.
block_symlink_and_fifo() {
  printf "%s" "$1" > "${LSM_INODE_POLICIES}/block_symlink"
  printf "%s" "$1" > "${LSM_INODE_POLICIES}/block_fifo"
}

# Allow symlink access on the given path.
allow_symlink() {
  printf "%s" "$1" > "${LSM_INODE_POLICIES}/allow_symlink"
}

# Allow fifo access on the given path.
allow_fifo() {
  printf "%s" "$1" > "${LSM_INODE_POLICIES}/allow_fifo"
}

# Check if one string contains the other.
string_contains() {
  case "$1" in
    *"$2"*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

# This file is created by clobber-state after the transition
# to dev mode.
DEV_MODE_FILE="/mnt/stateful_partition/.developer_mode"

# Flag file indicating that encrypted stateful should be preserved across TPM
# clear. If the file is present, it's expected that TPM is not owned.
PRESERVATION_REQUEST_FILE="/mnt/stateful_partition/preservation_request"

# This file is created after the TPM is initialized and the device is owned.
INSTALL_ATTRIBUTES_FILE=\
"/mnt/stateful_partition/home/.shadow/install_attributes.pb"

# File used to trigger a stateful reset.  Contains arguments for
# the "clobber-state" call.  This file may exist at boot time, as
# some use cases operate by creating this file with the necessary
# arguments and then rebooting.
RESET_FILE="/mnt/stateful_partition/factory_install_reset"

# Returns if device needs to clobber even though there's no devmode file present
# and boot is in verified mode.
needs_clobber_without_devmode_file() {
  ! is_tpm_owned && [ ! -O "${PRESERVATION_REQUEST_FILE}" ] &&
  [ -O "${INSTALL_ATTRIBUTES_FILE}" ]
}

dev_pop_paths_to_preserve

# Always return success to avoid killing init
exit 0
