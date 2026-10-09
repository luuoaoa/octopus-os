#!/bin/sh
# Update a running Chumby One (falconwing-mainline) over SSH, no reflash.
#
# The root is a read-only squashfs file on the card, and nothing else on
# the card is in use (see board/falconwing-mainline/README.md), so it can be
# replaced live:
#
#   rootfs.squashfs   -> /media/sd (carrier, p2): streamed to a temp name,
#                        SHA-256 checked, then renamed into place (the
#                        mounted old file stays valid until the reboot)
#   early boot        -> /media/sd/{bin,lib,sbin,newroot} (init-ram, busybox,
#                        libs, mountpoint)
#   kernel + DTB      -> the krnA slot of p1, only if it differs; read
#                        back and compared after writing (the one step
#                        that could leave the board unbootable)
#   customize-chumby/{network-config,user-data,authorized_keys}
#                     -> /boot (FAT, p3), only those present locally
#
# then reboots. /var (alarms, SSH host keys, ...) is left alone.
#
# Usage:  bin/update-chumby.sh [user@]host [--no-reboot]
#         Key auth if you have a key installed, else password "root"
#         (CHUMBY_SSH_PASSWORD=... to override; needs sshpass).
set -eu

HOST="${1:-}"
[ -n "$HOST" ] || { echo "usage: $0 [user@]host [--no-reboot]" >&2; exit 1; }
case "$HOST" in *@*) ;; *) HOST="root@${HOST}" ;; esac
REBOOT=1
[ "${2:-}" = "--no-reboot" ] && REBOOT=

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${SCRIPT_DIR}/output-falconwing-mainline"
IMG_DIR="${OUT}/images"
CARRIER="${OUT}/build/carrier"
BOOT_IMG="${IMG_DIR}/falconwing-boot.img"
CUSTOMIZE="${SCRIPT_DIR}/customize-chumby"

for f in "${IMG_DIR}/rootfs.squashfs" "${BOOT_IMG}" "${CARRIER}/sbin/init-ram"; do
    [ -e "$f" ] || { echo "missing $f; run: bash docker-build.sh falconwing-mainline" >&2; exit 1; }
done

SSH_CTL="${TMPDIR:-/tmp}/update-chumby-$$.sock"
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

cleanup() {
    ssh ${SSH_OPTS} "${HOST}" 'sync; mount -o remount,ro /boot' 2>/dev/null || true
    ssh ${SSH_OPTS} -O exit "${HOST}" 2>/dev/null || true
    rm -f "${SSH_CTL}"
}
trap cleanup EXIT

# One authenticated master connection; everything else rides on it.
# Never let ssh fall back to an interactive or GUI password prompt.
if ! SSH_ASKPASS_REQUIRE=never ssh ${SSH_OPTS} -o BatchMode=yes "${HOST}" true 2>/dev/null; then
    command -v sshpass >/dev/null 2>&1 || { echo "key auth failed and sshpass is not installed" >&2; exit 1; }
    SSH_ASKPASS_REQUIRE=never sshpass -p "${CHUMBY_SSH_PASSWORD:-root}" \
        ssh ${SSH_OPTS} -o PubkeyAuthentication=no "${HOST}" true
fi

on 'mountpoint -q /media/sd && [ -w /media/sd ]' ||
    { echo "${HOST}: /media/sd (the carrier) is not mounted rw: not a RAM-root image?" >&2; exit 1; }

# --- system image ---
SUM=$(sha256sum "${IMG_DIR}/rootfs.squashfs" | cut -d' ' -f1)
if [ "$(on 'sha256sum /media/sd/rootfs.squashfs' | cut -d' ' -f1)" = "${SUM}" ]; then
    echo "==> rootfs.squashfs unchanged"
else
    echo "==> rootfs.squashfs ($(stat -c %s "${IMG_DIR}/rootfs.squashfs") bytes)"
    on 'cat > /media/sd/rootfs.squashfs.new' < "${IMG_DIR}/rootfs.squashfs"
    [ "$(on 'sha256sum /media/sd/rootfs.squashfs.new' | cut -d' ' -f1)" = "${SUM}" ] ||
        { on 'rm -f /media/sd/rootfs.squashfs.new'; echo "checksum mismatch after transfer" >&2; exit 1; }
    on 'mv /media/sd/rootfs.squashfs.new /media/sd/rootfs.squashfs && sync'
fi

# --- early boot (the carrier's busybox, libraries, init-ram) ---
echo "==> early boot files"
tar -C "${CARRIER}" -cf - bin lib sbin newroot | on 'tar -C /media/sd -xf - && sync'

# --- kernel: the krnA slot of p1 ---
# Slot offset/size from the config block, as post-image.sh does.
KRN=$(python3 -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[0xC000:0xC004] == b"Cfg*", "no config block"
for i in range(0xC400, 0xC800, 16):
    off, size, _, name = struct.unpack("<III4s", d[i:i + 16])
    if name == b"krnA":
        print(off, size)
        break
else:
    sys.exit("krnA not found")
' "${BOOT_IMG}")
KRN_OFF=${KRN% *}
KRN_SIZE=${KRN#* }
BS=4096
SLOT_SUM=$(dd if="${BOOT_IMG}" bs=${BS} skip=$((KRN_OFF / BS)) count=$((KRN_SIZE / BS)) status=none | sha256sum | cut -d' ' -f1)
read_slot='echo 3 > /proc/sys/vm/drop_caches; dd if=/dev/mmcblk0p1 bs='${BS}' skip='$((KRN_OFF / BS))' count='$((KRN_SIZE / BS))' 2>/dev/null | sha256sum'
if [ "$(on "${read_slot}" | cut -d' ' -f1)" = "${SLOT_SUM}" ]; then
    echo "==> kernel unchanged"
else
    echo "==> kernel: writing the krnA slot (${KRN_SIZE} bytes at $(printf 0x%x "${KRN_OFF}"))"
    dd if="${BOOT_IMG}" bs=${BS} skip=$((KRN_OFF / BS)) count=$((KRN_SIZE / BS)) status=none |
        on "dd of=/dev/mmcblk0p1 bs=${BS} seek=$((KRN_OFF / BS)) conv=notrunc,fsync 2>/dev/null"
    if [ "$(on "${read_slot}" | cut -d' ' -f1)" != "${SLOT_SUM}" ]; then
        echo "ERROR: the kernel slot reads back wrong. Do NOT reboot; re-run this script." >&2
        exit 1
    fi
    echo "    verified"
fi

# --- config partition: only files present locally, never deletes ---
if [ -d "${CUSTOMIZE}" ]; then
    on 'mount -o remount,rw /boot'
    for f in network-config user-data authorized_keys; do
        [ -f "${CUSTOMIZE}/${f}" ] || continue
        echo "==> customize-chumby/${f} -> /boot/${f}"
        on "cat > /boot/${f}" < "${CUSTOMIZE}/${f}"
    done
    on 'sync; mount -o remount,ro /boot'
fi

if [ -n "${REBOOT}" ]; then
    echo "==> reboot ${HOST}"
    on 'sync; (sleep 1; systemctl reboot) >/dev/null 2>&1 &' || true
fi
echo "done."
