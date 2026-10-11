#!/bin/sh
# Runs after board/falconwing-mainline/post-fakeroot.sh (wpa_supplicant@
# and udhcpc@ on wlan0, /etc links to /run/chumby-config).
set -eu

TARGET_DIR="$1"
WANTS="${TARGET_DIR}/etc/systemd/system/multi-user.target.wants"

mkdir -p "${WANTS}"
# The Pi 4 has Ethernet as well.
ln -sf /usr/lib/systemd/system/udhcpc@.service "${WANTS}/udhcpc@eth0.service"
# The speakers' bass and EQ, as set in Settings, at every boot.
ln -sf /usr/lib/systemd/system/chumby-sound.service "${WANTS}/chumby-sound.service"
