#!/bin/sh
# First-boot bootstrap: create /dev/mmcblk0p2 spanning the rest of the SD card,
# starting from the sector after partition 1. Skipped on subsequent boots
# because the systemd unit has ConditionPathExists=!/dev/mmcblk0p2.
#
# Uses busybox fdisk + sysfs only — parted is not on this image.
set -eu

DEV=/dev/mmcblk0
ALIGN=2048   # sectors = 1 MiB at 512-byte sectors

# Read p1 geometry from sysfs (start + size in sectors). End = start+size-1.
P1_START=$(cat /sys/class/block/mmcblk0p1/start)
P1_SIZE=$(cat /sys/class/block/mmcblk0p1/size)
P1_END=$(( P1_START + P1_SIZE - 1 ))

# Round p2 start UP to the next 1 MiB boundary so the filesystem and SD-card
# erase blocks line up — anything else makes writes slower and wear-uneven.
START_P2=$(( ((P1_END + ALIGN) / ALIGN) * ALIGN ))

echo "create-var-partition: end of p1 sector=${P1_END}, aligned p2 start=${START_P2}s, creating p2 to end"

# Drive busybox fdisk via stdin:
#   n      new partition
#   p      primary
#   2      partition number
#   <s>    explicit start sector
#   <blank> default end (= rest of disk)
#   w      write + BLKRRPART (kernel re-reads the table)
fdisk -u "$DEV" <<EOF
n
p
2
$START_P2

w
EOF

# Wait for the new partition node to materialise before downstream units
# (systemd-makefs on /var) try to use it.
udevadm settle --timeout=10
