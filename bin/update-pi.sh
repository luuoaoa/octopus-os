#!/bin/sh
# Update a running pifansrf Pi over SSH using rsync. Streams only changed
# files to /boot/ — no 512MB FAT image staging in /tmp (Pi 0W's tmpfs/RAM
# is too small for that).
#
# Usage:  bin/update-pi.sh [user@]host
#         (user defaults to root; password defaults to "root", override
#          with PIFANSRF_SSH_PASSWORD=… in the environment.)
#
# Notes:
#   - Skips host-key verification because the Pi regenerates its host
#     key every boot (initramfs, no persistent /etc/dropbear).
#   - Multiplexes ssh/rsync through one ControlMaster connection, so
#     sshpass auth happens exactly once.
#   - Customize files (network-config, user-data, onlyfansd.yaml) are
#     only pushed if present locally; missing customize files DO NOT
#     delete what's on the Pi.
set -eu

HOST="${1:-}"
[ -n "$HOST" ] || { echo "usage: $0 [user@]host" >&2; exit 1; }
case "$HOST" in *@*) ;; *) HOST="root@${HOST}" ;; esac

PASSWD="${PIFANSRF_SSH_PASSWORD:-root}"

if ! command -v sshpass >/dev/null 2>&1; then
    cat >&2 <<'EOF'
sshpass not installed. Install one of:
  Ubuntu/Debian:  sudo apt install sshpass
  macOS (brew):   brew install hudochenkov/sshpass/sshpass
EOF
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG_DIR="${SCRIPT_DIR}/output/images"
CUSTOMIZE="${SCRIPT_DIR}/customize"

[ -f "${IMG_DIR}/zImage" ]         || { echo "no ${IMG_DIR}/zImage; run ./docker-build.sh" >&2; exit 1; }
[ -f "${IMG_DIR}/rootfs.cpio.gz" ] || { echo "no ${IMG_DIR}/rootfs.cpio.gz" >&2; exit 1; }

SSH_CTL="${TMPDIR:-/tmp}/update-pi-$$.sock"
SSH_OPTS="\
-o ControlMaster=auto \
-o ControlPath=${SSH_CTL} \
-o ControlPersist=300s \
-o StrictHostKeyChecking=no \
-o UserKnownHostsFile=/dev/null \
-o GlobalKnownHostsFile=/dev/null \
-o LogLevel=ERROR \
-o ConnectTimeout=10"

cleanup() {
    # Best-effort: remount /boot ro and close the ssh master. Either
    # may fail silently if the Pi has already rebooted — that's fine.
    ssh ${SSH_OPTS} "${HOST}" 'sync; mount -o remount,ro /boot' 2>/dev/null || true
    ssh ${SSH_OPTS} -O exit "${HOST}" 2>/dev/null || true
    rm -f "${SSH_CTL}"
}
trap cleanup EXIT

# Open the master with sshpass — every subsequent ssh / rsync goes
# through this multiplexed channel, no further auth.
sshpass -p "${PASSWD}" ssh ${SSH_OPTS} "${HOST}" true

# vfat doesn't support unix owner/group/perms — strip rsync archive bits
# that would try to chown/chmod on the FAT and fail.
RSYNC_OPTS="-rl -i --no-owner --no-group --no-perms --no-times"
RSYNC_E="ssh ${SSH_OPTS}"

echo "==> remount /boot rw on ${HOST}"
ssh ${SSH_OPTS} "${HOST}" 'mount -o remount,rw /boot'

echo "==> rsync kernel + dtb + cpio rootfs -> /boot/"
rsync -e "${RSYNC_E}" ${RSYNC_OPTS} \
    "${IMG_DIR}/zImage" \
    "${IMG_DIR}/bcm2708-rpi-zero-w.dtb" \
    "${IMG_DIR}/rootfs.cpio.gz" \
    "${HOST}:/boot/"

echo "==> rsync rpi-firmware contents -> /boot/"
rsync -e "${RSYNC_E}" ${RSYNC_OPTS} "${IMG_DIR}/rpi-firmware/" "${HOST}:/boot/"

# Customize: only known names, only if present locally, never delete.
sent_customize=
for f in network-config user-data onlyfansd.yaml; do
    if [ -f "${CUSTOMIZE}/${f}" ]; then
        echo "==> rsync customize/${f} -> /boot/${f}"
        rsync -e "${RSYNC_E}" ${RSYNC_OPTS} "${CUSTOMIZE}/${f}" "${HOST}:/boot/${f}"
        sent_customize=1
    fi
done
[ -z "$sent_customize" ] && \
    echo "==> no customize/{network-config,user-data,onlyfansd.yaml} found; leaving /boot's existing files alone"

echo "==> sync + remount ro + reboot ${HOST}"
ssh ${SSH_OPTS} "${HOST}" 'sync && mount -o remount,ro /boot && (sleep 1; systemctl reboot) >/dev/null 2>&1 &' || true
echo "done."
