#!/bin/sh
# First-boot bootstrap: create /dev/mmcblk0p2 spanning the rest of the SD card,
# starting from the sector after partition 1. Skipped on subsequent boots
# because the systemd unit has ConditionPathExists=!/dev/mmcblk0p2.
set -eu

DEV=/dev/mmcblk0

# End of p1 in sectors. Round the start of p2 UP to the next 1MiB boundary
# (2048 * 512-byte sectors) so the filesystem and SD-card erase blocks line up
# — anything else makes parted print 'not properly aligned' and slows writes.
END_P1=$(parted -s -m "$DEV" unit s print | awk -F: '/^1:/ {print $3}' | tr -d 's')
ALIGN=2048
START_P2=$(( ((END_P1 + ALIGN) / ALIGN) * ALIGN ))

echo "create-var-partition: end of p1 sector=${END_P1}, aligned p2 start=${START_P2}s, creating p2 to end"
parted -s -a optimal "$DEV" mkpart primary ext4 "${START_P2}s" 100%

# Make the kernel pick up the new partition entry.
partprobe "$DEV" 2>/dev/null || true
udevadm settle --timeout=10
