#!/bin/bash
# Build the falconwing (Chumby One), mainline kernel image. Output lands in ./output-falconwing-mainline/,
# dl cache in ./dl/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output-falconwing-mainline"

cd "${SCRIPT_DIR}/buildroot"
make O="${OUTPUT_DIR}" BR2_EXTERNAL="${SCRIPT_DIR}" falconwing_mainline_defconfig
make O="${OUTPUT_DIR}"
