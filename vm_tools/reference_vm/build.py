#!/usr/bin/env python3
# Copyright 2023 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Build a VM disk image that can boot as a guest on ChromeOS with VM integrations.
"""

import argparse
import contextlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
from typing import Dict, List, Optional, Tuple

import yaml


SCRIPT_PATH = pathlib.Path(__file__).parent
DISK_CONFIG = SCRIPT_PATH / "disk_config"
SETUP_SCRIPT = SCRIPT_PATH / "setup.sh"
DATA_PATH = SCRIPT_PATH / "data"
LVM_VG_NAME = "refvm"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "-o",
        "--out",
        default="refvm.img",
        help="output file (default: %(default)s)",
    )
    ap.add_argument(
        "-s",
        "--size",
        default=20,
        type=int,
        help="image size in GiB (default: %(default)s)",
    )
    ap.add_argument("--cache-dir", help="directory for debootstrap caches")
    ap.add_argument(
        "--cros-version",
        required=True,
        help="install VM tools for this CrOS version",
    )
    ap.add_argument(
        "--cros-tools",
        default="release",
        choices=["release", "staging"],
        help="source of VM tools (default: %(default)s)",
    )
    ap.add_argument(
        "--debian-release",
        default="bookworm",
        help="OS version to be installed (default: %(default)s)",
    )
    args = ap.parse_args()

    cache_dir = pathlib.Path(args.cache_dir) if args.cache_dir else None
    # TODO(jamesye): auto-detect the latest version
    cros_version = args.cros_version
    cros_packages_url = (
        f"https://storage.googleapis.com/cros-packages/{cros_version}/"
        if args.cros_tools == "release"
        else f"https://storage.googleapis.com/cros-packages-staging/{cros_version}/"
    )

    image_path = pathlib.Path(args.out)
    image_size = args.size * 1024**3

    if image_path.exists():
        raise Exception(f"Image file '{image_path}' already exists")

    with tempfile.TemporaryDirectory() as temp_dir_name, open(
        image_path, "wb"
    ) as image:
        temp_dir = pathlib.Path(temp_dir_name)
        image.seek(image_size - 1)
        image.write(b"\0")
        image.seek(0)
        with setup_loop(image_path) as loop:
            disk_vars, fstab = setup_storage(temp_dir, loop)

            target = temp_dir / "target"
            with mount_target(target, disk_vars):
                run_debootstrap(
                    args.debian_release, target, cache_dir=cache_dir
                )
                with open(target / "etc/fstab", "w") as f:
                    print("Writing fstab")
                    f.write(fstab)

                with prepare_chroot(target):
                    shutil.copytree(DATA_PATH, target / "tmp/data")
                    shutil.copy(SETUP_SCRIPT, target / "tmp/setup.sh")
                    os.chmod(target / "tmp/setup.sh", 0o755)
                    run_in_chroot(
                        target,
                        ["/tmp/setup.sh"],
                        env={
                            "CROS_PACKAGES_URL": cros_packages_url,
                            "RELEASE": args.debian_release,
                        },
                    )

                    print("Install complete, syncing")
                    subprocess.run(["sync"])


@contextlib.contextmanager
def setup_loop(loop_file: pathlib.Path):
    res = subprocess.run(
        ["losetup", "--show", "-f", loop_file],
        capture_output=True,
        check=True,
        text=True,
    )

    loop_device = pathlib.Path(res.stdout.strip())

    try:
        yield loop_device
    finally:
        subprocess.run(["vgchange", "-an", LVM_VG_NAME], check=True)
        subprocess.run(["losetup", "-d", loop_device], check=True)


def setup_storage(
    temp_dir: pathlib.Path, target_device: pathlib.Path
) -> Tuple[Dict[str, str], str]:
    ignored_vgs = [
        vg for vg in list_volume_groups() if not vg.startswith(LVM_VG_NAME)
    ]

    subprocess.run(
        [
            "setup-storage",
            "-X",
            "-y",
            "-L",
            temp_dir,
            "-f",
            DISK_CONFIG,
            "-D",
            target_device.relative_to(pathlib.Path("/dev")),
        ],
        check=True,
        env={"SS_IGNORE_VG": ",".join(ignored_vgs), **os.environ},
    )

    with open(temp_dir / "disk_var.yml") as f:
        disk_vars = yaml.load(f, Loader=yaml.SafeLoader)
    with open(temp_dir / "fstab") as f:
        fstab = f.read()

    return (disk_vars, fstab)


def list_volume_groups() -> List[str]:
    res = subprocess.run(
        ["vgs", "--reportformat=json"],
        capture_output=True,
        check=True,
        text=True,
    )
    data = json.loads(res.stdout)
    vg_names = []
    for vg in data["report"][0]["vg"]:
        vg_names.append(vg["vg_name"])

    return vg_names


@contextlib.contextmanager
def mount_target(target: pathlib.Path, disk_vars: Dict[str, str]):
    try:
        location = target
        location.mkdir()
        subprocess.run(
            ["mount", disk_vars["ROOT_PARTITION"], location], check=True
        )

        location = target / "boot"
        location.mkdir()
        subprocess.run(
            ["mount", disk_vars["BOOT_PARTITION"], location], check=True
        )

        location = target / "boot/efi"
        location.mkdir()
        subprocess.run(["mount", disk_vars["ESP_DEVICE"], location], check=True)

        yield
    finally:
        subprocess.run(["umount", "-R", target], check=True)


@contextlib.contextmanager
def prepare_chroot(target: pathlib.Path):
    mounts = []
    try:
        mountpoint = target / "dev"
        subprocess.run(["mount", "--bind", "/dev", mountpoint], check=True)
        mounts.append(mountpoint)

        mountpoint = target / "dev/pts"
        subprocess.run(["mount", "--bind", "/dev/pts", mountpoint], check=True)
        mounts.append(mountpoint)

        mountpoint = target / "sys"
        subprocess.run(
            ["mount", "-t", "sysfs", "sysfs", mountpoint], check=True
        )
        mounts.append(mountpoint)

        mountpoint = target / "proc"
        subprocess.run(["mount", "-t", "proc", "proc", mountpoint], check=True)
        mounts.append(mountpoint)

        mountpoint = target / "tmp"
        subprocess.run(
            ["mount", "-t", "tmpfs", "tmpfs", mountpoint], check=True
        )
        mounts.append(mountpoint)

        mountpoint = target / "run"
        subprocess.run(
            ["mount", "-t", "tmpfs", "tmpfs", mountpoint], check=True
        )
        mounts.append(mountpoint)

        yield
    finally:
        for mountpoint in mounts[::-1]:
            subprocess.run(["umount", mountpoint], check=True)


def run_debootstrap(
    suite: str, target: pathlib.Path, cache_dir: Optional[pathlib.Path] = None
):
    cache_args = []
    if cache_dir:
        if not cache_dir.exists():
            cache_dir.mkdir()
        cache_args += ["--cache-dir", cache_dir]
    # Run with eatmydata to improve build time.
    subprocess.run(
        [
            "eatmydata",
            "debootstrap",
            "--include=eatmydata",
            *cache_args,
            suite,
            target,
        ],
        check=True,
    )


def run_in_chroot(
    target: pathlib.Path,
    command: List[str],
    env: Optional[Dict[str, str]] = None,
):
    env = {**os.environ, "LANG": "C.UTF-8", **(env if env else {})}
    # Run with eatmydata to improve build time.
    subprocess.run(
        ["eatmydata", "chroot", target, *command], check=True, env=env
    )


if __name__ == "__main__":
    main()