#!/bin/bash
# make-sdk.sh [BOARD]: build the Octopus SDK for a board, from source
# (docs/sdk-build.md). BOARD is a configs/<BOARD>_defconfig with the
# Octopus OS in it; the default, raspad, is any 64-bit Raspberry Pi
# (aarch64). falconwing_mainline is the Chumby One itself (ARMv5).
#
# In the pinned build container (as docker-build.sh): the board's image,
# then buildroot's SDK (the toolchain and the sysroot, relocatable). Then
# two Docker images, tagged with the board:
#
#   octopus-sdk:BOARD   the SDK + octopus-app + the examples (x86-64)
#   octopus-sim:BOARD   the board's root filesystem, to run its UI on the PC
#
# The first build takes an hour or two (everything from source); later
# ones only what changed. Use the images with OCTOPUS_BOARD=BOARD
# octopus-app ... (raspad is octopus-app's default too).
set -euo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${1:-raspad}"
DEFCONFIG="${BOARD}_defconfig"
OUT="output-${BOARD//_/-}"
BUILDER=octopus-builder:ubuntu20.04

[ -f "$TOP/configs/$DEFCONFIG" ] || { echo "no configs/$DEFCONFIG" >&2; exit 1; }
grep -q '^BR2_PACKAGE_LIBOCTOPUS=y' "$TOP/configs/$DEFCONFIG" ||
	echo "warning: $DEFCONFIG has no liboctopus: no SDK for Octopus apps" >&2
grep -q '^BR2_TARGET_ROOTFS_TAR=y' "$TOP/configs/$DEFCONFIG" ||
	echo "warning: $DEFCONFIG makes no rootfs.tar: no simulator image" >&2

echo "== the build container"
docker build --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t "$BUILDER" "$TOP"

echo "== $BOARD: the image and the SDK (in $OUT/)"
# (This repository's own packages - libchumby, the shell, the apps - are
# local sources buildroot doesn't watch: rebuilt every time, or a pull's
# changes to them wouldn't reach the image or the SDK.)
docker run --rm -v "$TOP:/work" -w /work/buildroot "$BUILDER" bash -c '
	set -e
	make O=/work/'"$OUT"' BR2_EXTERNAL=/work '"$DEFCONFIG"'
	rebuild=
	for mk in /work/package/*/*.mk; do
		grep -q "_SITE_METHOD = local" "$mk" || continue
		pkg=$(basename "$(dirname "$mk")")
		var=BR2_PACKAGE_$(echo "$pkg" | tr a-z- A-Z_)
		grep -q "^$var=y" /work/'"$OUT"'/.config || continue
		# liboctopus first: the others build against its headers.
		if [ "$pkg" = liboctopus ]; then rebuild="$pkg-rebuild $rebuild"
		else rebuild="$rebuild $pkg-rebuild"; fi
	done
	make O=/work/'"$OUT"' $rebuild
	make O=/work/'"$OUT"'
	make O=/work/'"$OUT"' sdk'

exec sh "$TOP/sdk/make-images.sh" "$BOARD"
