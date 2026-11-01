#!/bin/bash
# Build script for drone NTS-1 mkII ARM unit

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$SCRIPT_DIR/sim"
SANDBOX_DIR="$(cd "$SCRIPT_DIR/../../../websim" && pwd)"

echo "============================================"
echo "Building drone NTS-1 mkII ARM unit"
echo "============================================"

cd "$SCRIPT_DIR"
make install

# Build the FlyonUI (Tailwind v4) web assets via Vite, so the web simulator
# stays in sync with the unit build. Always rebuild (no stale-check): osc.html,
# app.css, entry.js, vite config and the flyonui package can all drift
# independently, and a single stale flyon.css was enough to mask that. Vite is
# fast (~1 s) so the unconditional rebuild is cheap insurance.
echo ""
echo "============================================"
echo "Building FlyonUI assets (Vite)..."
echo "============================================"
cd "$SANDBOX_DIR"
if [ ! -d node_modules ]; then
  echo "node_modules missing - running npm install..."
  npm install --no-audit --no-fund
fi
npx vite build
echo ""

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
