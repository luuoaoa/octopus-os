#!/bin/sh
# Runs in fakeroot context AFTER systemctl preset-all, before image generation.
# preset-all disables wpa_supplicant@.service (template) per buildroot's
# 80-buildroot.preset, which deletes any pre-existing instance link. Re-create
# the wlan0 instance link here so it survives into the final rootfs.
set -e

TARGET_DIR="$1"

mkdir -p "${TARGET_DIR}/etc/systemd/system/multi-user.target.wants"
ln -sf /usr/lib/systemd/system/wpa_supplicant@.service \
    "${TARGET_DIR}/etc/systemd/system/multi-user.target.wants/wpa_supplicant@wlan0.service"
