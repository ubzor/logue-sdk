#!/bin/bash
# Build script for the drone NTS-1 mkII family: ARM units + the web simulator.
#
# Builds every drone-* module in this directory:
#   - drone-osc    (oscillator)      ARM unit
#   - drone-modfx  (modulation FX)   ARM unit
#   - drone-delfx  (delay FX)        ARM unit
#   - drone-revfx  (reverb FX)       ARM unit
#   - drone-chain  (full OSC->MOD->DEL->REV chain)  WEB-ONLY: the ONE unified
#     wasm worklet (sim/index.html) -- the only web simulator that exists
#     (served by serve.sh at http://localhost:8000/). It runs all four DSP
#     stages in one worklet, so there are no per-unit standalone pages.
#
# Each ARM module produces a loadable <PROJECT>.nts1mkiiunit via `make install`
# (the Makefile also runs the undefined-symbol guard). drone-chain produces a
# self-contained simulator in its sim/ dir via `make wasm` (html + js + wasm +
# copied assets + Vite flyon.css/js + vercel.json).
#
# Usage:
#   ./build.sh               # 4 ARM units + the chain simulator (default)
#   ./build.sh arm           # ARM units only (all 4)
#   ./build.sh sim           # the chain web simulator only
#   ./build.sh drone-osc     # build only the listed ARM module
#   ./build.sh drone-chain   # build only the chain web simulator
#
# (Any argument not in {arm, sim} is treated as a module name; the target is
# "both" unless a target keyword was given. The web simulator is always
# drone-chain: the four drone-* modules no longer have standalone pages.)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM_DIR="$SCRIPT_DIR"

# ARM units (produce .nts1mkiiunit via `make install`).
ARM_MODULES=(drone-osc drone-modfx drone-delfx drone-revfx)
# The only web simulator: the unified chain page (all 4 units in one worklet).
CHAIN_MODULE=drone-chain

# ---- Parse arguments: target keyword (arm | sim) + optional module names -----
TARGET="both"
MODULES=()
for ARG in "$@"; do
  case "$ARG" in
    arm) TARGET="arm" ;;
    sim) TARGET="sim" ;;
    *)   MODULES+=("$ARG") ;;
  esac
done
# No module names given -> build the default set for the requested target:
#   both/arm -> the 4 ARM units;  sim -> the chain simulator only.
# A "both" run (no target keyword) is the default build: the 4 ARM units
# PLUS the unified chain simulator.
if [ "${#MODULES[@]}" -eq 0 ]; then
  case "$TARGET" in
    sim) MODULES=("$CHAIN_MODULE") ;;
    both) MODULES=("${ARM_MODULES[@]}" "$CHAIN_MODULE") ;;
    *)   MODULES=("${ARM_MODULES[@]}") ;;
  esac
fi

# ---- FlyonUI (Tailwind v4) web assets (Vite) ---------------------------------
# The web-simulator CSS/JS are built once from the shared websim/ project
# (only needed when the drone-chain simulator is built in this run). Always
# rebuild (no stale-check): the assets can drift independently and Vite is
# fast (~1 s).
SANDBOX_DIR="$(cd "$PLATFORM_DIR/../../websim" && pwd)"
NEED_VITE=0
for MOD in "${MODULES[@]}"; do
  [ "$MOD" = "$CHAIN_MODULE" ] && NEED_VITE=1
done
# The Vite assets and the chain simulator are built only for sim/both runs:
# an ARM-only run needs no web assets, and an explicit ARM module list does
# not imply the web simulator either.
if [ "$TARGET" = "arm" ] || [ "$NEED_VITE" -eq 0 ]; then
  BUILD_SIM=0
else
  BUILD_SIM=1
fi
if [ "$BUILD_SIM" -eq 1 ]; then
  echo "============================================"
  echo "Building FlyonUI assets (Vite) in $SANDBOX_DIR"
  echo "============================================"
  (cd "$SANDBOX_DIR" && {
    if [ ! -d node_modules ]; then
      echo "node_modules missing - running npm install..."
      npm install --no-audit --no-fund
    fi
    npx vite build
  })
  echo ""
fi

# ---- Build each ARM unit -----------------------------------------------------
BUILD_OK=1
for MOD in "${MODULES[@]}"; do
  MOD_DIR="$PLATFORM_DIR/$MOD"
  if [ ! -d "$MOD_DIR" ]; then
    echo "WARNING: module dir not found, skipping: $MOD_DIR"
    continue
  fi
  # Only modules that have an ARM build are built as ARM units. drone-chain
  # is web-only (its Makefile has no `install` target), so skip it here.
  if [ "$TARGET" = "sim" ] || [ ! -f "$MOD_DIR/unit.cc" ]; then
    continue
  fi
  echo "============================================"
  echo "Building $MOD (ARM unit)"
  echo "============================================"
  if (cd "$MOD_DIR" && make install); then
    echo "[$MOD] ARM OK"
  else
    echo "[$MOD] ARM FAILED" >&2
    BUILD_OK=0
  fi
  echo ""
done

# ---- Build the web simulator (drone-chain) -----------------------------------
# The unified chain page is the ONLY web simulator: it runs all four DSP
# stages (OSC -> MOD -> DEL -> REV) in a single wasm worklet. Build it when
# the requested run includes it -- by default (both) or via an explicit name.
# The Vite flyon.css/js were already built above, so `make wasm` here only
# recompiles the C/C++ and links (the slow part).
if [ "$BUILD_SIM" -eq 1 ]; then
  MOD_DIR="$PLATFORM_DIR/$CHAIN_MODULE"
  echo "============================================"
  echo "Building $CHAIN_MODULE (web simulator)"
  echo "============================================"
  if (cd "$MOD_DIR" && make wasm); then
    echo "[$CHAIN_MODULE] SIM OK"
  else
    echo "[$CHAIN_MODULE] SIM FAILED" >&2
    BUILD_OK=0
  fi
  echo ""
fi

# ---- Publish ARM unit binaries to $PLATFORM_DIR/build ------------------------
# After a fully successful ARM build, refresh the shared build/ folder with the
# .nts1mkiiunit files so they can be loaded/inspected in one place. Only the
# units built in THIS run are touched: each unit's previous copy in build/ is
# removed before being re-copied; any other files in build/ are left alone.
# build/ is gitignored (root .gitignore: **/build).
if [ "$BUILD_OK" -eq 1 ] && [ "$TARGET" != "sim" ]; then
  PUBLISH_DIR="$PLATFORM_DIR/build"
  mkdir -p "$PUBLISH_DIR"
  echo "============================================"
  echo "Publishing unit binaries -> $PUBLISH_DIR"
  echo "============================================"
  for MOD in "${ARM_MODULES[@]}"; do
    FOUND=$(ls "$PLATFORM_DIR/$MOD/"*.nts1mkiiunit 2>/dev/null | head -1)
    [ -n "$FOUND" ] || continue
    rm -f "$PUBLISH_DIR/$(basename "$FOUND")"
    cp "$FOUND" "$PUBLISH_DIR/"
    echo "  $(basename "$FOUND")"
  done
  echo ""
fi

echo "============================================"
echo "Build complete!"
echo "============================================"
# Summary: the requested ARM units + the web simulator (drone-chain) when it
# was built in this run.
SUMMARY_MODULES=()
for MOD in "${MODULES[@]}"; do
  [ "$MOD" = "$CHAIN_MODULE" ] && [ "$BUILD_SIM" -ne 1 ] && continue
  SUMMARY_MODULES+=("$MOD")
done
for MOD in "${SUMMARY_MODULES[@]}"; do
  # ARTIFACT name = PROJECT from config.mk (may differ from the folder name,
  # e.g. drone-osc -> drone_osc.nts1mkiiunit). Find any .nts1mkiiunit in the dir.
  FOUND=$(ls "$PLATFORM_DIR/$MOD/"*.nts1mkiiunit 2>/dev/null | head -1)
  if [ -n "$FOUND" ]; then
    BYTES=$(wc -c < "$FOUND")
    KIB_1DP=$(awk -v b="$BYTES" 'BEGIN { printf "%.1f", b / 1024.0 }')
    echo "  $MOD: $(basename "$FOUND")  ($BYTES bytes, ~${KIB_1DP} KiB)"
  fi
  # Web simulator artifact (drone-chain -> sim/index.html; only it has one).
  if [ "$MOD" = "$CHAIN_MODULE" ]; then
    SIMHTML="$PLATFORM_DIR/$MOD/sim/index.html"
    if [ -f "$SIMHTML" ]; then
      echo "  $MOD: sim -> ${SIMHTML#"$PLATFORM_DIR"/}"
    fi
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