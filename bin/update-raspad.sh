#!/bin/sh
# Update a running RasPad (configs/raspad_defconfig) over SSH, no reflash.
#
# The image's root (output-raspad/images/rootfs.ext4) and boot FAT
# (boot.vfat) are unpacked here (debugfs, mtools: no root needed) and
# rsynced onto the live system:
#
#   rootfs  -> /     only what changed; files are replaced by rename, so
#                    running programs keep their old copy until the
#                    reboot. Kept: /var (SSH host keys, the journal,
#                    alarms), /etc/machine-id, and the mounts.
#   boot    -> /boot the kernel, DTB, firmware, config.txt, cmdline.txt and
#                    the customize-raspad/ files built in (remounted rw
#                    for the copy)
#
# then reboots (unless --no-reboot).
#
# usage: bin/update-raspad.sh [user@]host [--no-reboot]
set -eu

HOST="${1:-}"
[ -n "$HOST" ] || { echo "usage: $0 [user@]host [--no-reboot]" >&2; exit 1; }
case "$HOST" in *@*) ;; *) HOST="root@${HOST}" ;; esac
REBOOT=1
[ "${2:-}" = "--no-reboot" ] && REBOOT=

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${SCRIPT_DIR}/output-raspad"
IMG_DIR="${OUT}/images"
HOST_BIN="${OUT}/host/bin"
HOST_SBIN="${OUT}/host/sbin"

for f in "${IMG_DIR}/rootfs.ext4" "${IMG_DIR}/boot.vfat" "${HOST_SBIN}/debugfs" "${HOST_BIN}/mcopy"; do
    [ -e "$f" ] || { echo "missing $f; build output-raspad first (board/raspad/README.md)" >&2; exit 1; }
done

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/update-raspad.XXXXXX")"
SSH_CTL="${STAGE}.sock"
SSH_OPTS="\
-o ControlMaster=auto \
-o ControlPath=${SSH_CTL} \
-o ControlPersist=300s \
-o StrictHostKeyChecking=no \
-o UserKnownHostsFile=/dev/null \
-o GlobalKnownHostsFile=/dev/null \
-o LogLevel=ERROR \
-o ConnectTimeout=30"
on() { ssh ${SSH_OPTS} "${HOST}" "$@"; }
rsync_to() { rsync -e "ssh ${SSH_OPTS}" "$@"; }

cleanup() {
    ssh ${SSH_OPTS} "${HOST}" 'sync; mount -o remount,ro /boot' 2>/dev/null || true
    ssh ${SSH_OPTS} -O exit "${HOST}" 2>/dev/null || true
    rm -rf "${STAGE}" "${SSH_CTL}"
}
trap cleanup EXIT

# One authenticated master connection; never an interactive password prompt.
if ! SSH_ASKPASS_REQUIRE=never ssh ${SSH_OPTS} -o BatchMode=yes "${HOST}" true 2>/dev/null; then
    command -v sshpass >/dev/null 2>&1 || { echo "key auth failed and sshpass is not installed" >&2; exit 1; }
    SSH_ASKPASS_REQUIRE=never sshpass -p "${RASPAD_SSH_PASSWORD:-root}" \
        ssh ${SSH_OPTS} -o PubkeyAuthentication=no "${HOST}" true
fi
on 'grep -q "raspad" /etc/motd 2>/dev/null || grep -qi "raspad" /etc/issue' ||
    { echo "${HOST} doesn't look like a raspad image; not touching it" >&2; exit 1; }

echo "==> unpacking the image"
mkdir -p "${STAGE}/root" "${STAGE}/boot"
"${HOST_SBIN}/debugfs" -R "rdump / ${STAGE}/root" "${IMG_DIR}/rootfs.ext4" 2>/dev/null
[ -x "${STAGE}/root/sbin/init" ] || [ -L "${STAGE}/root/sbin/init" ] ||
    { echo "unpacking rootfs.ext4 failed" >&2; exit 1; }
MTOOLS_SKIP_CHECK=1 "${HOST_BIN}/mcopy" -s -n -i "${IMG_DIR}/boot.vfat" "::/*" "${STAGE}/boot/"

echo "==> rootfs -> /"
# Owners: everything in the image is root's, and root writes it there.
rsync_to -rlptDH --delete --no-owner --no-group --stats \
    --exclude=/var/ --exclude=/boot/ --exclude=/proc/ --exclude=/sys/ \
    --exclude=/dev/ --exclude=/run/ --exclude=/tmp/ --exclude=/lost+found/ \
    --exclude=/etc/machine-id --exclude=/root/.ash_history \
    "${STAGE}/root/" "${HOST}:/" | grep -E "Number of regular files transferred|Total bytes sent"

echo "==> boot -> /boot"
on 'mount -o remount,rw /boot'
# FAT: no owners, modes or links; 2 s timestamps.
rsync_to -rt --modify-window=2 --no-perms --inplace --stats \
    "${STAGE}/boot/" "${HOST}:/boot/" | grep -E "Number of regular files transferred|Total bytes sent"
on 'sync; mount -o remount,ro /boot'

if [ -n "${REBOOT}" ]; then
    echo "==> reboot ${HOST}"
    on 'systemctl daemon-reload; (sleep 1; systemctl reboot) >/dev/null 2>&1 &'
fi
echo "done."
