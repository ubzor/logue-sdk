#!/bin/bash
# Build script for the drone NTS-1 mkII unit family (ARM units).
#
# Builds every drone-* module in this directory:
#   - drone-osc    (oscillator)
#   - drone-modfx  (modulation FX)
#   - drone-delfx  (delay FX)
#   - drone-revfx  (reverb FX)
#
# Each module produces a loadable <PROJECT>.nts1mkiiunit via `make install`,
# which for the enhanced Makefile also runs the undefined-symbol (UND) guard.
#
# Usage:
#   ./build.sh            # build all 4 modules
#   ./build.sh drone-osc  # build only the listed module(s)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM_DIR="$SCRIPT_DIR"

# Modules that exist by default. A caller may pass explicit module names to
# restrict the build; otherwise build everything in this list.
ALL_MODULES=(drone-osc drone-modfx drone-delfx drone-revfx)

if [ "$#" -gt 0 ]; then
  MODULES=("$@")
else
  MODULES=("${ALL_MODULES[@]}")
fi

# ---- FlyonUI (Tailwind v4) web assets (Vite) ---------------------------------
# The web-simulator CSS/JS are built once from the shared websim/ project. Each
# module's `make wasm` also (re)builds them, so building them here keeps the
# web assets in sync even for an ARM-only run. Always rebuild (no stale-check):
# the assets can drift independently and Vite is fast (~1 s).
SANDBOX_DIR="$(cd "$PLATFORM_DIR/../../websim" && pwd)"
echo "============================================"
echo "Building FlyonUI assets (Vite) in $SANDBOX_DIR"
echo "============================================"
cd "$SANDBOX_DIR"
if [ ! -d node_modules ]; then
  echo "node_modules missing - running npm install..."
  npm install --no-audit --no-fund
fi
npx vite build
echo ""

# ---- Build each ARM unit -----------------------------------------------------
BUILD_OK=1
for MOD in "${MODULES[@]}"; do
  MOD_DIR="$PLATFORM_DIR/$MOD"
  if [ ! -d "$MOD_DIR" ]; then
    echo "WARNING: module dir not found, skipping: $MOD_DIR"
    continue
  fi
  echo "============================================"
  echo "Building $MOD (ARM unit)"
  echo "============================================"
  if (cd "$MOD_DIR" && make install); then
    echo "[$MOD] OK"
  else
    echo "[$MOD] FAILED" >&2
    BUILD_OK=0
  fi
  echo ""
done

echo "============================================"
echo "Build complete!"
echo "============================================"
for MOD in "${MODULES[@]}"; do
  UNIT="$PLATFORM_DIR/$MOD/$MOD.nts1mkiiunit"
  # ARTIFACT name = PROJECT from config.mk (may differ from the folder name,
  # e.g. drone-osc -> drone.nts1mkiiunit). Find any .nts1mkiiunit in the dir.
  FOUND=$(ls "$PLATFORM_DIR/$MOD/"*.nts1mkiiunit 2>/dev/null | head -1)
  if [ -n "$FOUND" ]; then
    BYTES=$(wc -c < "$FOUND")
    KIB_1DP=$(awk -v b="$BYTES" 'BEGIN { printf "%.1f", b / 1024.0 }')
    echo "  $MOD: $FOUND  ($BYTES bytes, ~${KIB_1DP} KiB)"
  fi
done

# ---- Section breakdown for each unit (arm-none-eabi-size), if available ------
SIZE_BIN=""
if command -v arm-none-eabi-size >/dev/null 2>&1; then
  SIZE_BIN="arm-none-eabi-size"
else
  GUESS="$PLATFORM_DIR/../../tools/gcc/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-size"
  if [ -x "$GUESS" ]; then SIZE_BIN="$GUESS"; fi
fi

if [ -n "$SIZE_BIN" ]; then
  echo ""
  for MOD in "${MODULES[@]}"; do
    FOUND=$(ls "$PLATFORM_DIR/$MOD/"*.nts1mkiiunit 2>/dev/null | head -1)
    [ -n "$FOUND" ] || continue
    echo "Section breakdown — $MOD ($(basename "$SIZE_BIN")):"
    "$SIZE_BIN" "$FOUND"
    echo ""
  done
fi

exit $((1 - BUILD_OK))