#!/bin/bash
# Build (if needed) and serve the WASM web simulator for drone NTS-1 mkII

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$SCRIPT_DIR/sim"
# The FlyonUI (Tailwind v4) assets are built by Vite from the shared websim/
# project. They live next to the other simulator sources.
SANDBOX_DIR="$(cd "$SCRIPT_DIR/../../../websim" && pwd)"

# Build the FlyonUI assets (flyon.css + flyon.js) if missing or stale. Stale
# if any Vite input is newer than the last build, or if node_modules is
# missing (first run). Vite is fast (<2 s) so we also rebuild when the shell
# HTML changed (new utility classes may need to be picked up by Tailwind) or
# when the flyonui package files changed. All of it - base/theme layers plus
# component CSS - is compiled into the single flyon.css by @tailwindcss/vite
# (the flyonuiRepair plugin in vite.flyonui.js fixes flyonui's minified
# artifacts before Lightning CSS runs, so no separate verbatim stylesheet is
# needed - see websim/app.css header).
flyon_stale() {
  local out="$SIM_DIR/flyon.css"
  [ ! -f "$out" ] && return 0
  [ ! -d "$SANDBOX_DIR/node_modules" ] && return 0
  for f in "$SANDBOX_DIR/app.css" "$SANDBOX_DIR/entry.js" \
           "$SANDBOX_DIR/vite.config.js" "$SANDBOX_DIR/vite.flyonui.js" \
           "$SANDBOX_DIR/package.json" "$SANDBOX_DIR/osc.html"; do
    [ -f "$f" ] && [ "$f" -nt "$out" ] && return 0
  done
  # The flyonui component files imported by app.css can also become stale.
  for f in button card range switches badge kbd label divider; do
    local p="$SANDBOX_DIR/node_modules/flyonui/components/$f.css"
    [ -f "$p" ] && [ "$p" -nt "$out" ] && return 0
  done
  return 1
}

if flyon_stale; then
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
fi

# Build the WASM simulator if it's missing or stale
# Stale if any source is newer than drone.html, or if the generated assets
# are incomplete (html exists but wasm/js are missing -> interrupted build).
if [ ! -f "$SIM_DIR/drone.html" ] || [ ! -f "$SIM_DIR/drone.wasm" ] || [ ! -f "$SIM_DIR/drone.js" ] || \
   [ "$SCRIPT_DIR/wasm.cc" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/header.c" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/osc.h" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/Makefile" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/config.mk" -nt "$SIM_DIR/drone.html" ]; then
    echo "============================================"
    echo "Building WASM simulator..."
    echo "============================================"
    cd "$SCRIPT_DIR"
    make wasm
    echo ""
fi

echo "============================================"
echo "Serving drone NTS-1 mkII web simulator"
echo "============================================"
echo "URL:       http://localhost:${1:-8000}/drone.html"
echo "Directory: $SIM_DIR"
echo ""
echo "Press Ctrl+C to stop the server"
echo "============================================"

# Serve using a small Python HTTP server.
#
# The Wasm audio worklet build uses -sWASM_WORKERS + -sSHARED_MEMORY, which
# require SharedArrayBuffer. Browsers only enable SharedArrayBuffer when the
# page is cross-origin isolated, which requires the COOP/COEP response headers
# below. Plain `python3 -m http.server` does not send them, so we use a tiny
# custom handler instead.
cd "$SIM_DIR"
PORT="${1:-8000}"
BIND="${2:-0.0.0.0}"
echo "Headers:   COOP/COEP enabled (for SharedArrayBuffer)"
echo ""
exec python3 - "$PORT" "$BIND" <<'PYEOF'
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1])
bind = sys.argv[2]

class COIHandler(SimpleHTTPRequestHandler):
    # Enable cross-origin isolation so SharedArrayBuffer is available.
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

ThreadingHTTPServer((bind, port), COIHandler).serve_forever()
PYEOF
