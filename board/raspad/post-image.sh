#!/bin/sh
# sdcard.img: the boot FAT and the ext4 root (genimage.cfg).
#
# The FAT gets whatever the build made for it: the firmware for every
# model (rpi-firmware/: bootcode.bin, start*.elf, fixup*.dat, overlays,
# config.txt, cmdline.txt), the kernel, every DTB - so the list follows
# the defconfig's DTS names.
#
# customize-raspad/{network-config,user-data,authorized_keys,
# wpa_supplicant.conf} at the top of the tree (gitignored: your WiFi
# password) go onto the boot FAT too, so a fresh card joins your network.
# The formats are sdcard-config-apply's (board/falconwing-mainline/rootfs/usr/bin).
set -eu

BOARD_DIR="$(dirname "$0")"
CUSTOMIZE="${BR2_EXTERNAL_OCTOPUS_PATH}/customize-raspad"
GENIMAGE_CFG="${BINARIES_DIR}/genimage-raspad.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
ROOTPATH_TMP="$(mktemp -d)"
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT

mkdir -p "${BINARIES_DIR}/customize"
rm -f "${BINARIES_DIR}"/customize/*
FILES=""
add() { FILES="${FILES}\t\t\t\"$1\",\n"; }
for f in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do
	[ -e "$f" ] && add "${f#${BINARIES_DIR}/}"
done
add Image
for f in network-config user-data authorized_keys wpa_supplicant.conf; do
	if [ -f "${CUSTOMIZE}/${f}" ]; then
		cp "${CUSTOMIZE}/${f}" "${BINARIES_DIR}/customize/${f}"
		add "customize/${f}"
		echo "raspad: ${f} from customize-raspad/ onto the boot FAT"
	fi
done
# (genimage puts each listed file at the FAT's root, by its name.)
FILES=$(printf "%b" "${FILES}" | sed '$ s/,$//')
awk -v files="${FILES}" '/^#BOOT_FILES#$/ { print files; next } { print }' \
	"${BOARD_DIR}/genimage.cfg" > "${GENIMAGE_CFG}"

rm -rf "${GENIMAGE_TMP}"
genimage \
	--rootpath "${ROOTPATH_TMP}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
