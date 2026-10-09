#!/bin/sh
# Runs inside fakeroot after buildroot's "systemctl preset-all", which
# would drop links to template instances made earlier: enable them here.
set -eu

TARGET_DIR="$1"
WANTS="${TARGET_DIR}/etc/systemd/system/multi-user.target.wants"

mkdir -p "${WANTS}"
ln -sf /usr/lib/systemd/system/wpa_supplicant@.service "${WANTS}/wpa_supplicant@wlan0.service"
ln -sf /usr/lib/systemd/system/udhcpc@.service "${WANTS}/udhcpc@wlan0.service"
