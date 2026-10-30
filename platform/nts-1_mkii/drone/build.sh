#!/bin/bash
# Build script for drone NTS-1 mkII ARM unit

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================"
echo "Building drone NTS-1 mkII ARM unit"
echo "============================================"

cd "$SCRIPT_DIR"
make install

echo ""
echo "============================================"
echo "Build complete!"
echo "============================================"

UNIT="$SCRIPT_DIR/drone.nts1mkiiunit"
echo "ARM unit: $UNIT"

# ---- Report the size of the built unit ----------------------------------------
# (1) total file size in bytes / KiB
# (2) section breakdown via arm-none-eabi-size, if the toolchain is reachable
#     directly (in PATH or under the SDK tools dir). --gc-sections already
#     dropped dead code, so this is the image the device actually loads.
if [ -f "$UNIT" ]; then
  BYTES=$(wc -c < "$UNIT")
  KIB=$(( (BYTES + 1023) / 1023 ))
  # 1 decimal place, without relying on bc (not always installed)
  KIB_1DP=$(awk -v b="$BYTES" 'BEGIN { printf "%.1f", b / 1024.0 }')
  echo ""
  echo "Size: $BYTES bytes (~${KIB_1DP} KiB, $KIB KiB rounded up)"

  SIZE_BIN=""
  if command -v arm-none-eabi-size >/dev/null 2>&1; then
    SIZE_BIN="arm-none-eabi-size"
  else
    # The Makefile builds from a fixed toolchain dir (../../tools/gcc/...)
    GUESS="$SCRIPT_DIR/../../../tools/gcc/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-size"
    if [ -x "$GUESS" ]; then
      SIZE_BIN="$GUESS"
    fi
  fi

  if [ -n "$SIZE_BIN" ]; then
    echo ""
    echo "Section breakdown ($(basename "$SIZE_BIN")):"
    "$SIZE_BIN" "$UNIT"
  else
    echo ""
    echo "(section breakdown skipped: arm-none-eabi-size not found in PATH or the SDK tools dir)"
  fi
fi
