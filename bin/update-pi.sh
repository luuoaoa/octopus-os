#!/bin/sh
# Push a freshly-built boot.vfat (and the customize/ files) to a running
# pifansrf Pi over SSH, then reboot it. Use to upgrade kernel + cpio rootfs
# in place without yanking the SD card.
#
# Usage: bin/update-pi.sh user@host
#
# Requires:
#   - output/images/boot.vfat       (run ./docker-build.sh first)
#   - SSH reachable to the Pi (ssh_pwauth: true on the running image)
#   - customize/network-config and customize/user-data, optional but
#     pretty much always wanted (otherwise the Pi reboots with no WiFi)
set -eu

HOST="${1:-}"
[ -n "$HOST" ] || { echo "usage: $0 user@host" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${SCRIPT_DIR}/output/images/boot.vfat"
CUSTOMIZE="${SCRIPT_DIR}/customize"

[ -f "$IMG" ] || { echo "no $IMG; run ./docker-build.sh first" >&2; exit 1; }

echo "==> staging $IMG -> ${HOST}:/tmp/boot.vfat ($(stat -c '%s' "$IMG") bytes)"
scp -q "$IMG" "${HOST}:/tmp/boot.vfat"

echo "==> running install-update.sh on ${HOST}"
ssh "$HOST" /usr/local/bin/install-update.sh

if [ -d "$CUSTOMIZE" ]; then
    for f in network-config user-data; do
        if [ -f "${CUSTOMIZE}/${f}" ]; then
            echo "==> uploading customize/${f} -> ${HOST}:/boot/${f}"
            scp -q "${CUSTOMIZE}/${f}" "${HOST}:/boot/${f}"
        fi
    done
else
    echo "WARN: no ${CUSTOMIZE}/ directory; the Pi will reboot without WiFi config"
fi

echo "==> rebooting ${HOST}"
# Background the reboot on the remote so the SSH connection closes cleanly
# instead of hanging on the dying server.
ssh "$HOST" 'sync; (sleep 1; systemctl reboot) >/dev/null 2>&1 &' || true
echo "done."
