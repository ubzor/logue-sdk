#!/bin/bash
# Build script for the drone NTS-1 mkII family: ARM units + web simulators.
#
# Builds every drone-* module in this directory:
#   - drone-osc    (oscillator)      ARM unit + standalone sim (sim/drone.html)
#   - drone-modfx  (modulation FX)   ARM unit + standalone sim (sim/fx.html)
#   - drone-delfx  (delay FX)        ARM unit + standalone sim (sim/fx.html)
#   - drone-revfx  (reverb FX)       ARM unit + standalone sim (sim/fx.html)
#   - drone-chain  (full OSC->MOD->DEL->REV chain)  WEB-ONLY: one unified
#     wasm worklet (sim/index.html) -- the ONLY web simulator built by default
#     (served by serve.sh at http://localhost:8000/). The 4 standalone FX/osc
#     pages are legacy: the chain page covers all four units, so they are only
#     rebuilt when requested explicitly (they take a long time to build).
#
# Each ARM module produces a loadable <PROJECT>.nts1mkiiunit via `make install`
# (the Makefile also runs the undefined-symbol guard). Each sim module
# produces a self-contained simulator in its sim/ dir via `make wasm`
# (html + js + wasm + copied assets + Vite flyon.css/js + vercel.json).
#
# Usage:
#   ./build.sh                 # 4 ARM units + the chain simulator (default)
#   ./build.sh arm             # ARM units only (all 4)
#   ./build.sh sim             # web simulators only (default: drone-chain)
#   ./build.sh sim all         # all 5 simulators incl. the 4 legacy standalone pages
#   ./build.sh drone-osc       # build only the listed module(s), both targets
#   ./build.sh sim drone-osc   # sim target, only the listed module(s)
#
# (Any argument not in {arm, sim, all} is treated as a module name; the target
# is "both" unless a target keyword was given. drone-chain only has a sim
# target. For sims, the default set is drone-chain; pass "all" for every sim.)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM_DIR="$SCRIPT_DIR"

# ARM units (produce .nts1mkiiunit via `make install`).
ARM_MODULES=(drone-osc drone-modfx drone-delfx drone-revfx)
# The only web simulator that matters: the unified chain page (all 4 units in
# one worklet). This is the DEFAULT sim target.
CHAIN_MODULE=drone-chain
# Legacy standalone sims (one page per unit). The chain page supersedes them,
# so they are built only when requested explicitly: ./build.sh sim all /
# ./build.sh sim drone-osc drone-modfx ...
LEGACY_SIM_MODULES=(drone-osc drone-modfx drone-delfx drone-revfx)

# ---- Parse arguments: target keyword (arm | sim) + optional module names -----
TARGET="both"
SIMS_ALL=0
MODULES=()
for ARG in "$@"; do
  case "$ARG" in
    arm) TARGET="arm" ;;
    sim) TARGET="sim" ;;
    all) SIMS_ALL=1 ;;
    *)   MODULES+=("$ARG") ;;
  esac
done
# No module names given -> build the default set for the requested target:
#   both/arm -> the 4 ARM units;  sim/both -> the chain simulator only.
if [ "${#MODULES[@]}" -eq 0 ]; then
  case "$TARGET" in
    sim) MODULES=("$CHAIN_MODULE") ;;
    *)   MODULES=("${ARM_MODULES[@]}") ;;
  esac
fi

# ---- FlyonUI (Tailwind v4) web assets (Vite) ---------------------------------
# The web-simulator CSS/JS are built once from the shared websim/ project. Each
# module's `make wasm` also (re)builds them, so building them here keeps the
# web assets in sync even for an ARM-only run. Always rebuild (no stale-check):
# the assets can drift independently and Vite is fast (~1 s).
SANDBOX_DIR="$(cd "$PLATFORM_DIR/../../websim" && pwd)"
if [ "$TARGET" != "arm" ]; then
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

# ---- Build each web simulator ------------------------------------------------
# Resolve the set of simulators to build:
#   - default (no module names): drone-chain only (the unified page);
#   - explicit module names: those modules; "all" -> chain + all legacy pages.
# The Vite flyon.css/js were already built above, so each `make wasm` here only
# recompiles the C/C++ and links (the slow part) -- one wasm module per sim.
if [ "$TARGET" != "arm" ]; then
  SIM_BUILD=()
  if [ "${#MODULES[@]}" -eq 0 ] || [ "$SIMS_ALL" -eq 1 ]; then
    SIM_BUILD=("$CHAIN_MODULE")
    if [ "$SIMS_ALL" -eq 1 ]; then
      SIM_BUILD+=("${LEGACY_SIM_MODULES[@]}")
    fi
  else
    SIM_BUILD=("${MODULES[@]}")
  fi

  for MOD in "${SIM_BUILD[@]}"; do
    MOD_DIR="$PLATFORM_DIR/$MOD"
    if [ ! -d "$MOD_DIR" ] || [ ! -f "$MOD_DIR/wasm.cc" ]; then
      continue
    fi
    echo "============================================"
    echo "Building $MOD (web simulator)"
    echo "============================================"
    if (cd "$MOD_DIR" && make wasm); then
      echo "[$MOD] SIM OK"
    else
      echo "[$MOD] SIM FAILED" >&2
      BUILD_OK=0
    fi
    echo ""
  done
fi

echo "============================================"
echo "Build complete!"
echo "============================================"
# Summary: the requested ARM units + the simulators actually built this run
# (drone-chain when no explicit module list was given, or all sims for "all").
SUMMARY_MODULES=("${MODULES[@]}")
if [ "$SIMS_ALL" -eq 1 ]; then
  SUMMARY_MODULES+=("$CHAIN_MODULE" "${LEGACY_SIM_MODULES[@]}")
elif [ "${#MODULES[@]}" -gt 0 ] && [ "${MODULES[0]}" != "$CHAIN_MODULE" ]; then
  # Default run (no explicit module list): MODULES was seeded with the
  # ARM set (both) or the chain sim (sim), so the chain sim was built too.
  SUMMARY_MODULES+=("$CHAIN_MODULE")
fi
for MOD in "${SUMMARY_MODULES[@]}"; do
  # ARTIFACT name = PROJECT from config.mk (may differ from the folder name,
  # e.g. drone-osc -> drone_osc.nts1mkiiunit). Find any .nts1mkiiunit in the dir.
  FOUND=$(ls "$PLATFORM_DIR/$MOD/"*.nts1mkiiunit 2>/dev/null | head -1)
  if [ -n "$FOUND" ]; then
    BYTES=$(wc -c < "$FOUND")
    KIB_1DP=$(awk -v b="$BYTES" 'BEGIN { printf "%.1f", b / 1024.0 }')
    echo "  $MOD: $(basename "$FOUND")  ($BYTES bytes, ~${KIB_1DP} KiB)"
  fi
  # Web simulator artifact (html; drone-chain -> index.html, others -> <P>.html).
  SIMHTML=$(ls "$PLATFORM_DIR/$MOD/sim/"*.html 2>/dev/null | grep -v obj | head -1)
  if [ -n "$SIMHTML" ]; then
    echo "  $MOD: sim -> ${SIMHTML#"$PLATFORM_DIR"/}"
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