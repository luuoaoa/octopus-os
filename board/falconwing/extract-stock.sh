#!/bin/bash
# Pull the bits we reuse from a stock Chumby One (falconwing) SD card image:
#
#   stock/boot.img          raw copy of partition 1 (type 0x53): the i.MX23
#                           boot stream, config block and the 2.6.28 kernels
#                           (krnA/krnB). Written back byte-for-byte at the
#                           same offset (sector 4) by genimage.
#   stock/rootfs/lib/modules/2.6.28-chumby
#                           kernel modules matching that kernel (USB host,
#                           wifi, watchdog, ...).
#   stock/rootfs/lib/firmware
#                           wifi firmware blobs (rt73.bin, rt2870.bin, ...).
#
# Run on the host (needs python3 and debugfs from e2fsprogs; no root):
#   board/falconwing/extract-stock.sh /path/to/sda.img
#
# The source image is only read, never modified. stock/ is gitignored.
set -euo pipefail

IMG="${1:?usage: $0 <stock-sdcard.img>}"
BOARD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${BOARD_DIR}/stock"

command -v debugfs >/dev/null || { echo "debugfs (e2fsprogs) not found" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# Print "<start> <count>" (in 512-byte sectors) of primary partition N.
part_geom() {
    python3 - "$IMG" "$1" <<'EOF'
import struct, sys
img, n = sys.argv[1], int(sys.argv[2])
with open(img, 'rb') as f:
    mbr = f.read(512)
assert mbr[510:512] == b'\x55\xaa', 'no MBR signature'
e = mbr[446 + 16 * (n - 1): 446 + 16 * n]
ptype = e[4]
start, count = struct.unpack('<II', e[8:16])
print(start, count, ptype)
EOF
}

read -r P1_START P1_COUNT P1_TYPE < <(part_geom 1)
read -r P2_START P2_COUNT P2_TYPE < <(part_geom 2)

if [ "${P1_TYPE}" != "$((0x53))" ]; then
    echo "partition 1 type is ${P1_TYPE}, expected 0x53 — not a falconwing card?" >&2
    exit 1
fi
if [ "${P1_START}" != "4" ]; then
    echo "WARNING: partition 1 starts at sector ${P1_START}, expected 4." >&2
    echo "         genimage-falconwing.cfg hardcodes offset 2048 bytes; adjust it." >&2
fi

rm -rf "${OUT}"
mkdir -p "${OUT}/rootfs/lib"

echo "p1: start=${P1_START} sectors=${P1_COUNT}"
dd if="${IMG}" of="${OUT}/boot.img" bs=512 skip="${P1_START}" count="${P1_COUNT}" status=none

# Sanity check: config block magic at 0xC000 and active slot.
python3 - "${OUT}/boot.img" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
cb = d[0xC000:0xC000 + 0x500]
if cb[:4] != b'Cfg*':
    sys.exit('config block magic Cfg* not found at 0xC000')
active = struct.unpack('<I', cb[8:12])[0]
print('config block: version=%s active_index=%d (%s -> root=/dev/mmcblk0p%d)'
      % (cb[0x10:0x20].rstrip(b'\0').decode(), active, 'krnA' if active == 0 else 'krnB', 2 + active))
if active != 0:
    print('WARNING: active slot is B; this board writes the rootfs to p2 only.')
EOF

echo "p2: start=${P2_START} sectors=${P2_COUNT}"
dd if="${IMG}" of="${TMP}/p2.img" bs=512 skip="${P2_START}" count="${P2_COUNT}" status=none

# debugfs rdump complains it can't chown as non-root; contents are fine.
debugfs -R "rdump /lib/modules ${OUT}/rootfs/lib" "${TMP}/p2.img" 2>/dev/null || true
debugfs -R "rdump /lib/firmware ${OUT}/rootfs/lib" "${TMP}/p2.img" 2>/dev/null || true

KVER=2.6.28-chumby
MODDIR="${OUT}/rootfs/lib/modules/${KVER}"
[ -f "${MODDIR}/modules.dep" ] || { echo "modules.dep missing in ${MODDIR}" >&2; exit 1; }
# Dangling links into the original build host.
rm -f "${MODDIR}/build" "${MODDIR}/source"
chmod -R u+rwX,go+rX "${OUT}/rootfs"

echo "modules: $(find "${MODDIR}" -name '*.ko' | wc -l) .ko files"
echo "firmware: $(ls "${OUT}/rootfs/lib/firmware" | tr '\n' ' ')"
echo "done -> ${OUT}"
