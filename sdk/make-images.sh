#!/bin/sh
# make-images.sh [BOARD]: the SDK's two Docker images from a board's build
# (sdk/make-sdk.sh runs this last; on its own, after the board's image and
# "make sdk" are built in output-BOARD/). BOARD defaults to rpi64.
#
#   octopus-sdk:BOARD   the toolchain (buildroot's SDK) + octopus-app + examples
#   octopus-sim:BOARD   the board's root filesystem + the simulator's session
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:-rpi64}
IMG=$TOP/output-$(echo "$BOARD" | tr _ -)/images
set -- "$IMG"/*_sdk-buildroot.tar.gz
SDK_TAR=$1

[ -f "$SDK_TAR" ] || { echo "no SDK in $IMG: make sdk first (sdk/make-sdk.sh $BOARD)" >&2; exit 1; }
[ -f "$IMG/rootfs.tar" ] || { echo "no $IMG/rootfs.tar: BR2_TARGET_ROOTFS_TAR=y" >&2; exit 1; }

# The SDK's directory, and its C compiler's prefix (an external toolchain's
# <arch>-linux-gcc, or buildroot's own <tuple>-gcc).
SDK_DIR=$(tar -tzf "$SDK_TAR" | head -1 | cut -d/ -f1)
GCC=$(tar -tzf "$SDK_TAR" | grep -E "^$SDK_DIR/bin/[a-z0-9_]+-[a-z0-9_-]*linux-[a-z0-9_]*gcc\$" |
	grep -v -- '-gcc-[0-9]' | head -1)
CROSS=$(basename "$GCC" | sed 's/gcc$//')
[ -n "$CROSS" ] || { echo "no C compiler found in $SDK_TAR" >&2; exit 1; }
echo "$BOARD: SDK $SDK_DIR, compiler ${CROSS}gcc"

ctx=$(mktemp -d)
trap 'rm -rf "$ctx"' EXIT

cp "$SDK_TAR" "$ctx/sdk.tar.gz"
cp "$TOP/sdk/Dockerfile" "$TOP/sdk/octopus-app" "$ctx/"
cp -r "$TOP/sdk/examples" "$ctx/"
docker build -t "octopus-sdk:$BOARD" --build-arg SDK_DIR="$SDK_DIR" \
	--build-arg CROSS="$CROSS" --build-arg BOARD="$BOARD" "$ctx"

rm -rf "${ctx:?}"/*
cp "$IMG/rootfs.tar" "$TOP/sdk/sim/Dockerfile" "$TOP/sdk/sim/octopus-sim-session" \
	"$TOP/sdk/sim/asound.conf" "$ctx/"
docker build -t "octopus-sim:$BOARD" "$ctx"

echo "octopus-sdk:$BOARD, octopus-sim:$BOARD built: OCTOPUS_BOARD=$BOARD octopus-app ..."
echo "To hand them out:"
echo "  docker save octopus-sdk:$BOARD octopus-sim:$BOARD | zstd > octopus-sdk-$BOARD.tar.zst"
