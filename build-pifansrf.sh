#!/bin/bash
# Build the pifansrf image. Output lands in ./output/, dl cache in ./dl/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"

cd "${SCRIPT_DIR}/buildroot"
make O="${OUTPUT_DIR}" BR2_EXTERNAL="${SCRIPT_DIR}" pifansrf_defconfig
make O="${OUTPUT_DIR}"
