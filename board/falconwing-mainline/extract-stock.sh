#!/bin/bash
# Pull what the image reuses from a stock Chumby One (falconwing) SD card:
#
#   stock/boot.img   raw copy of partition 1 (type 0x53): the i.MX23 boot
#                    stream and config block, and the kernel slots krnA/krnB.
#                    post-image.sh writes our kernel into krnA and puts the
#                    whole thing back at the same offset (sector 4).
#
# Run once, on the host (needs python3; no root):
#   board/falconwing-mainline/extract-stock.sh /path/to/stock-sdcard.img
#
# The source image is only read, never modified. stock/ is gitignored.
set -euo pipefail

IMG="${1:?usage: $0 <stock-sdcard.img>}"
BOARD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${BOARD_DIR}/stock"

# "<start> <count> <type>" (512-byte sectors) of primary partition 1.
read -r P1_START P1_COUNT P1_TYPE < <(python3 - "$IMG" <<'EOF'
import struct, sys
with open(sys.argv[1], 'rb') as f:
    mbr = f.read(512)
assert mbr[510:512] == b'\x55\xaa', 'no MBR signature'
e = mbr[446:462]
start, count = struct.unpack('<II', e[8:16])
print(start, count, e[4])
EOF
)

if [ "${P1_TYPE}" != "$((0x53))" ]; then
    echo "partition 1 type is ${P1_TYPE}, expected 0x53 — not a Chumby One card?" >&2
    exit 1
fi
if [ "${P1_START}" != "4" ]; then
    echo "WARNING: partition 1 starts at sector ${P1_START}, expected 4;" >&2
    echo "         genimage.cfg puts it back at sector 4." >&2
fi

rm -rf "${OUT}"
mkdir -p "${OUT}"
echo "p1: start=${P1_START} sectors=${P1_COUNT}"
dd if="${IMG}" of="${OUT}/boot.img" bs=512 skip="${P1_START}" count="${P1_COUNT}" status=none

# Sanity check: the config block's magic at 0xC000, and the active slot.
python3 - "${OUT}/boot.img" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
cb = d[0xC000:0xC000 + 0x500]
if cb[:4] != b'Cfg*':
    sys.exit('config block magic Cfg* not found at 0xC000')
active = struct.unpack('<I', cb[8:12])[0]
print('config block: version=%s active_index=%d (%s)'
      % (cb[0x10:0x20].rstrip(b'\0').decode(), active, 'krnA' if active == 0 else 'krnB'))
if active != 0:
    print('WARNING: active slot is B; the image boots krnA.')
EOF
echo "done -> ${OUT}/boot.img"
