#!/bin/sh
# sdcard.img: the boot FAT and the ext4 root (genimage.cfg).
#
# customize-raspad/{network-config,user-data,authorized_keys,
# wpa_supplicant.conf} at the top of the tree (gitignored: your WiFi
# password) go onto the boot FAT too, so a fresh card joins your network.
# The formats are sdcard-config-apply's (board/falconwing/rootfs-common).
set -eu

BOARD_DIR="$(dirname "$0")"
CUSTOMIZE="${BR2_EXTERNAL_PIFANSRF_PATH}/customize-raspad"
GENIMAGE_CFG="${BINARIES_DIR}/genimage-raspad.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
ROOTPATH_TMP="$(mktemp -d)"
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT

EXTRA=""
mkdir -p "${BINARIES_DIR}/customize"
rm -f "${BINARIES_DIR}"/customize/*
for f in network-config user-data authorized_keys wpa_supplicant.conf; do
	if [ -f "${CUSTOMIZE}/${f}" ]; then
		cp "${CUSTOMIZE}/${f}" "${BINARIES_DIR}/customize/${f}"
		EXTRA="${EXTRA}\t\t\t\"customize/${f}\",\n"
		echo "raspad: ${f} from customize-raspad/ onto the boot FAT"
	fi
done
# (genimage puts each listed file at the FAT's root, by its name.)
EXTRA=$(printf "%b" "${EXTRA}")
awk -v extra="${EXTRA}" '{ print } /files = \{/ && extra != "" { print extra }' \
	"${BOARD_DIR}/genimage.cfg" > "${GENIMAGE_CFG}"

rm -rf "${GENIMAGE_TMP}"
genimage \
	--rootpath "${ROOTPATH_TMP}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
