#!/bin/sh
# First-boot bootstrap: create a fixed-size /dev/mmcblk0p2 starting just
# after partition 1. Skipped on subsequent boots because the systemd unit
# has ConditionPathExists=!/dev/mmcblk0p2.
#
# Uses busybox fdisk + sysfs only — parted is not on this image.
set -eu

DEV=/dev/mmcblk0
ALIGN=2048      # sectors = 1 MiB at 512-byte sectors
P2_SIZE=+512M   # /var size; persistent state for onlyfansd + journals.
                # SD lifetime > usable space, so don't claim the rest of
                # the card just because it's there.

# Read p1 geometry from sysfs (start + size in sectors). End = start+size-1.
P1_START=$(cat /sys/class/block/mmcblk0p1/start)
P1_SIZE=$(cat /sys/class/block/mmcblk0p1/size)
P1_END=$(( P1_START + P1_SIZE - 1 ))

# Round p2 start UP to the next 1 MiB boundary so the filesystem and SD-card
# erase blocks line up — anything else makes writes slower and wear-uneven.
START_P2=$(( ((P1_END + ALIGN) / ALIGN) * ALIGN ))

echo "create-var-partition: end of p1 sector=${P1_END}, aligned p2 start=${START_P2}s, size=${P2_SIZE}"

# Drive busybox fdisk via stdin:
#   n        new partition
#   p        primary
#   2        partition number
#   <s>      explicit start sector
#   <P2_SIZE> last sector — busybox accepts +size{,K,M,G,T} shorthand
#   w        write + BLKRRPART (kernel re-reads the table)
fdisk -u "$DEV" <<EOF
n
p
2
$START_P2
$P2_SIZE
w
EOF

# Wait for the new partition node to materialise before we format it.
udevadm settle --timeout=10

# Format unconditionally. We can't trust an existing fs signature on this
# device: an earlier version of the image gave /var the rest of the disk,
# so after reducing /var to +512M and reflashing, the new (smaller) p2
# starts at the same sector as the old (larger) one and the old ext4
# superblock is still sitting at offset 1024 (sectors beyond p1 aren't
# touched by `dd if=sdcard.img`). If we left it there, the kernel would
# refuse to mount with "EXT4-fs: bad geometry, block count exceeds size
# of device" because s_blocks_count still describes the old fs. -F skips
# the "really format an existing fs?" prompt; this script only runs on
# first boot (ConditionPathExists=!/dev/mmcblk0p2), so there's no real
# user data to protect at this point.
mkfs.ext4 -F -L var /dev/mmcblk0p2
