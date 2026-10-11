#!/bin/bash
# Build the RasPad / any 64-bit Raspberry Pi image (the Chumby's UI on a
# Pi). Output lands in ./output-raspad/, dl cache shared in ./dl/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output-raspad"

cd "${SCRIPT_DIR}/buildroot"
make O="${OUTPUT_DIR}" BR2_EXTERNAL="${SCRIPT_DIR}" raspad_defconfig
make O="${OUTPUT_DIR}"
