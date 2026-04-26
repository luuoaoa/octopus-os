#!/bin/sh
# Update a running pifansrf Pi over SSH using rsync. Streams only changed
# files to /boot/ — no 512MB FAT image staging in /tmp (which would fill
# the Pi's RAM since /tmp is tmpfs and we only have ~280MB free).
#
# Usage: bin/update-pi.sh user@host
#
# Requires:
#   - output/images/                  (run ./docker-build.sh first)
#   - SSH reachable to the Pi (ssh_pwauth: true)
#   - rsync on both sides (BR2_PACKAGE_RSYNC=y in pifansrf defconfig)
#   - customize/network-config and customize/user-data — optional but
#     pretty much always wanted (otherwise the Pi reboots with no WiFi).
set -eu

HOST="${1:-}"
[ -n "$HOST" ] || { echo "usage: $0 user@host" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG_DIR="${SCRIPT_DIR}/output/images"
CUSTOMIZE="${SCRIPT_DIR}/customize"

[ -f "${IMG_DIR}/zImage" ]       || { echo "no ${IMG_DIR}/zImage; run ./docker-build.sh" >&2; exit 1; }
[ -f "${IMG_DIR}/rootfs.cpio.gz" ] || { echo "no ${IMG_DIR}/rootfs.cpio.gz" >&2; exit 1; }

echo "==> remount /boot rw on ${HOST}"
ssh "${HOST}" 'mount -o remount,rw /boot'

# If anything below fails, try to leave /boot ro again (defensive).
trap 'ssh "${HOST}" "sync; mount -o remount,ro /boot" 2>/dev/null || true' EXIT

echo "==> rsync kernel + dtb + cpio rootfs -> /boot/"
rsync -aiv --no-times \
    "${IMG_DIR}/zImage" \
    "${IMG_DIR}/bcm2708-rpi-zero-w.dtb" \
    "${IMG_DIR}/rootfs.cpio.gz" \
    "${HOST}:/boot/"

echo "==> rsync rpi-firmware contents -> /boot/"
rsync -aiv --no-times "${IMG_DIR}/rpi-firmware/" "${HOST}:/boot/"

if [ -d "${CUSTOMIZE}" ]; then
    echo "==> rsync customize/ -> /boot/"
    rsync -aiv --no-times "${CUSTOMIZE}/" "${HOST}:/boot/"
else
    echo "WARN: no ${CUSTOMIZE}/ on dev host; Pi will keep its existing /boot/{network-config,user-data}"
fi

echo "==> sync + remount ro + reboot ${HOST}"
# The reboot kills our SSH connection; backgrounding it lets ssh exit cleanly.
ssh "${HOST}" 'sync && mount -o remount,ro /boot && (sleep 1; systemctl reboot) >/dev/null 2>&1 &' || true
trap - EXIT
echo "done."
