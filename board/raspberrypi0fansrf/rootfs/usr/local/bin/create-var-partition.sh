#!/bin/sh
# First-boot bootstrap: create /dev/mmcblk0p2 spanning the rest of the SD card,
# starting from the sector after partition 1. Skipped on subsequent boots
# because the systemd unit has ConditionPathExists=!/dev/mmcblk0p2.
set -eu

DEV=/dev/mmcblk0

# End of p1 in sectors, then add 1 to get the start of p2.
END_P1=$(parted -s -m "$DEV" unit s print | awk -F: '/^1:/ {print $3}' | tr -d 's')
START_P2=$((END_P1 + 1))

echo "create-var-partition: end of p1 sector=${END_P1}, creating p2 from ${START_P2}s to end"
parted -s -a optimal "$DEV" mkpart primary ext4 "${START_P2}s" 100%

# Make the kernel pick up the new partition entry.
partprobe "$DEV" 2>/dev/null || true
udevadm settle --timeout=10
