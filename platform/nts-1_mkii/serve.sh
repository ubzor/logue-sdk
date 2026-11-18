#!/bin/bash
# Serve the WASM web simulator for the drone NTS-1 mkII units.
#
# Serves the "drone-chain" simulator: the full DRONE audio path
# OSC -> MOD -> DEL -> REV -> out running as a SINGLE wasm audio worklet
# (drone-chain/), with the MOD/DEL/REV TIME+DEPTH knobs and the full 44-
# parameter OSC UI. The UI is the SvelteKit app in websim/app/ (the legacy
# static shell websim/index.html is no longer served). The landing URL is
# the document root (http://host:8000/). This is the ONLY web simulator
# of the drone family: the four per-unit standalone pages no longer exist.
#
# Usage:
#   ./serve.sh [PORT] [BIND]      # default: 8000 0.0.0.0
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM_DIR="$SCRIPT_DIR"
# websim/ lives at the repository root: root/platform/nts-1_mkii/../.. = root,
# so the shared web-simulator project is two levels up from this script.
SANDBOX_DIR="$(cd "$PLATFORM_DIR/../../websim" && pwd)"
APP_DIR="$SANDBOX_DIR/app"

# The module to serve: the chained (osc -> mod -> del -> rev) wasm engine.
# Its sim/ dir holds the emscripten artifacts (index.js + index.wasm); the
# SvelteKit app in websim/app/ is the UI and serves them as static files.
SERVE_MODULE="drone-chain"
SIM_DIR="$PLATFORM_DIR/$SERVE_MODULE/sim"

# ---- Build the SvelteKit app if missing or stale -----------------------------
# Stale if any of the app sources (svelte components, css, wiring, glue
# relink script, build config) is newer than the build output, or if the
# build output is missing entirely. The wasm artifacts are synced from
# drone-chain/sim into app/static/ below (before the build), so a newer
# wasm rebuild also invalidates the app build.
APP_BUILD="$APP_DIR/build"
APP_SOURCES=(
  "$APP_DIR/src"
  "$APP_DIR/static"
  "$APP_DIR/scripts"
  "$APP_DIR/svelte.config.js"
  "$APP_DIR/vite.config.js"
  "$APP_DIR/vite.flyonui.js"
  "$APP_DIR/package.json"
)
app_stale=0
if [ ! -d "$APP_BUILD/client" ]; then
  app_stale=1
else
  for s in "${APP_SOURCES[@]}"; do
    [ -e "$s" ] || continue
    if [ "$s" -nt "$APP_BUILD/client" ] || \
       [ -n "$(find "$s" -newer "$APP_BUILD/client" -print -quit 2>/dev/null)" ]; then
      app_stale=1
      break
    fi
  done
fi

# ---- Build the WASM simulator if missing or stale -----------------------------
# Stale if any source is newer than index.html, or if the generated assets are
# incomplete (html exists but wasm/js are missing -> interrupted build).
# drone-chain's DSP spans the sibling drone-* module dirs, so any of those
# headers changing (e.g. the FX DSP in drone-modfx/modfx.h) must also rebuild.
M=$PLATFORM_DIR/$SERVE_MODULE
if [ ! -f "$SIM_DIR/index.html" ] || [ ! -f "$SIM_DIR/index.wasm" ] || [ ! -f "$SIM_DIR/index.js" ] || \
   [ "$M/wasm.cc" -nt "$SIM_DIR/index.html" ] || \
   [ "$M/chain.h" -nt "$SIM_DIR/index.html" ] || \
   [ "$M/chain.cc" -nt "$SIM_DIR/index.html" ] || \
   [ "$M/header.c" -nt "$SIM_DIR/index.html" ] || \
   [ "$M/Makefile" -nt "$SIM_DIR/index.html" ] || \
   [ "$M/config.mk" -nt "$SIM_DIR/index.html" ] || \
   [ "$PLATFORM_DIR/drone-osc/osc.h" -nt "$SIM_DIR/index.html" ] || \
   [ "$PLATFORM_DIR/drone-modfx/modfx.h" -nt "$SIM_DIR/index.html" ] || \
   [ "$PLATFORM_DIR/drone-delfx/delay.h" -nt "$SIM_DIR/index.html" ] || \
   [ "$PLATFORM_DIR/drone-revfx/reverb.h" -nt "$SIM_DIR/index.html" ]; then
  echo "============================================"
  echo "Building WASM simulator ($SERVE_MODULE)..."
  echo "============================================"
  (cd "$PLATFORM_DIR/$SERVE_MODULE" && make wasm)
  echo ""
  # A fresh wasm rebuild must always invalidate the app build (the glue is
  # copied into app/static/ right before it, which also triggers staleness,
  # but say it explicitly for the log).
  app_stale=1
fi

if [ "$app_stale" -eq 1 ]; then
  echo "============================================"
  echo "Building SvelteKit app (websim/app)..."
  echo "============================================"
  cd "$APP_DIR"
  if [ ! -d node_modules ]; then
    echo "node_modules missing - running npm install..."
    npm install --no-audit --no-fund
  fi
  # Sync the emscripten artifacts (glue + wasm) into the app's static/ so
  # the built client serves them from the document root (/index.js,
  # /index.wasm). The Makefile's vercel.json/flyon.css copy steps are legacy
  # (static shell) and no longer needed: COOP/COEP headers now come from
  # the SvelteKit server hooks (src/hooks.server.ts) + vite dev headers.
  mkdir -p "$APP_DIR/static"
  cp -p "$SIM_DIR/index.js" "$APP_DIR/static/index.js"
  cp -p "$SIM_DIR/index.wasm" "$APP_DIR/static/index.wasm"
  npm run build
  # Re-patch the glue AFTER the build: the build wipes build/client/ and the
  # precompressed .br/.gz copies go stale. relink-glue.mjs re-applies the
  # wiring patch, re-copies static/ -> build/client/ and drops stale .br/.gz.
  node "$APP_DIR/scripts/relink-glue.mjs"
  echo ""
fi

echo "============================================"
echo "Serving $SERVE_MODULE web simulator (SvelteKit)"
echo "============================================"
echo "URL:       http://localhost:${1:-8000}/"
echo "App:       $APP_DIR/build (adapter-node)"
echo "Wasm:      $SIM_DIR"
echo ""
echo "Press Ctrl+C to stop the server"
echo "============================================"

# Serve the built SvelteKit app (adapter-node). COOP/COEP headers for the
# wasm audio worklet (SharedArrayBuffer) come from the app's server hooks
# (src/hooks.server.ts), not from the shell.
#
# Bind address: adapter-node hardcodes process.env.HOST with a localhost
# default; PORT is honored. Set HOST for the bind address, run from a
# stable cwd (the app dir, not the sim dir) so the build tree stays valid.
cd "$APP_DIR"
PORT="${1:-8000}"
HOST="${2:-0.0.0.0}"
echo "Headers:   COOP/COEP enabled (for SharedArrayBuffer, via server hooks)"
echo ""
exec env PORT="$PORT" HOST="$HOST" node build