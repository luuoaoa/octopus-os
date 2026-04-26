#!/bin/sh
# Replace the boot FAT contents on /dev/mmcblk0p1 with a fresh boot.vfat
# staged at /tmp/boot.vfat. Designed to be invoked over SSH from the dev
# host as part of an in-place update.
#
# Caller is responsible for re-staging /boot/network-config and
# /boot/user-data after this script returns; everything currently on /boot
# is wiped by the dd.
set -eu

STAGE=/tmp/boot.vfat
DEV=/dev/mmcblk0p1

[ -f "$STAGE" ]                    || { echo "install-update: no $STAGE" >&2; exit 1; }
[ -b "$DEV" ]                      || { echo "install-update: no $DEV" >&2; exit 1; }

SIZE=$(stat -c '%s' "$STAGE")
PART_BYTES=$(blockdev --getsize64 "$DEV")

if [ "$SIZE" -gt "$PART_BYTES" ]; then
    echo "install-update: $STAGE ($SIZE bytes) is larger than $DEV ($PART_BYTES bytes); refusing" >&2
    exit 1
fi
if [ "$SIZE" -lt 1048576 ]; then
    echo "install-update: $STAGE ($SIZE bytes) looks suspiciously small; refusing" >&2
    exit 1
fi

# Quiesce /boot and unmount so we can write the raw FAT.
sync
umount /boot 2>/dev/null || true
if mount | grep -q ' on /boot '; then
    echo "install-update: /boot still mounted, refusing to dd over the live FAT" >&2
    exit 1
fi

echo "install-update: writing $SIZE bytes from $STAGE to $DEV"
dd if="$STAGE" of="$DEV" bs=4M conv=fsync status=none
sync

# Bring /boot back rw so the caller can drop network-config / user-data on it.
mount -o rw /boot

rm -f "$STAGE"
echo "install-update: done; /boot is fresh and writable"
