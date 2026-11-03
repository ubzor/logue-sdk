#!/bin/bash
# Build (if needed) and serve the WASM web simulator for drone NTS-1 mkII

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$SCRIPT_DIR/sim"
# The FlyonUI (Tailwind v4) assets are built by Vite from the shared websim/
# project. They live next to the other simulator sources.
SANDBOX_DIR="$(cd "$SCRIPT_DIR/../../../websim" && pwd)"

# Build the FlyonUI (Tailwind v4) assets via Vite, so the web simulator
# stays in sync with the unit build. Vite compiles app.css + the flyonui
# component CSS (button/card/range/switch/badge/kbd/label/divider) into a
# single flyon.css, and the entry.js into flyon.js. flyonui's minified
# artifacts are patched by the flyonuiRepair plugin in vite.flyonui.js before
# Lightning CSS runs, so the components import cleanly.
# Always rebuild (no stale-check): osc.html, app.css, entry.js, vite config
# and the flyonui package can all drift independently, and a single stale
# flyon.css was enough to mask that. Vite is fast (~1 s) so the unconditional
# rebuild is cheap insurance.
echo "============================================"
echo "Building FlyonUI assets (Vite)..."
echo "============================================"
cd "$SANDBOX_DIR"
if [ ! -d node_modules ]; then
  echo "node_modules missing - running npm install..."
  npm install --no-audit --no-fund
fi
npx vite build
# Vite outputs flyon.css + flyon.js into $SANDBOX_DIR/sim/. Copy them into
# $SIM_DIR so the served simulator has them even when `make wasm` below is
# skipped (drone.html up to date). Without this step a stale check or a
# partial make-wasm run leaves sim/ without the CSS/JS -> 404 in the browser.
# Ensure $SIM_DIR exists first: after `make clean` the whole sim/ directory is
# removed, and `make wasm` (which normally creates it via the em++ link step)
# runs *after* this copy, so a plain `cp` would fail on the missing target dir.
mkdir -p "$SIM_DIR"
cp -p "$SANDBOX_DIR/sim/flyon.css" "$SIM_DIR/flyon.css"
cp -p "$SANDBOX_DIR/sim/flyon.js"  "$SIM_DIR/flyon.js"
echo ""

# Build the WASM simulator if it's missing or stale
# Stale if any source is newer than drone.html, or if the generated assets
# are incomplete (html exists but wasm/js are missing -> interrupted build).
# Note: osc.html is the Emscripten shell --shell-file source for drone.html,
# so a change to it alone must re-run em++ (otherwise the old drone.html,
# with its old class-attribute set, is what the browser loads).
if [ ! -f "$SIM_DIR/drone.html" ] || [ ! -f "$SIM_DIR/drone.wasm" ] || [ ! -f "$SIM_DIR/drone.js" ] || \
   [ "$SCRIPT_DIR/wasm.cc" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/header.c" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/osc.h" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/Makefile" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SCRIPT_DIR/config.mk" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SANDBOX_DIR/osc.html" -nt "$SIM_DIR/drone.html" ]; then
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
#
# Do NOT `cd "$SIM_DIR"` before exec: if the server runs while `make clean`
# removes (and `make wasm` recreates) $SIM_DIR, the process's cwd becomes
# invalid and the Python runtime can stall on os.getcwd(). Instead, pin the
# cwd to $SCRIPT_DIR (stable — never removed by make) and pass $SIM_DIR to the
# handler as its served directory (see COIHandler.__init__ below).
cd "$SCRIPT_DIR"
PORT="${1:-8000}"
BIND="${2:-0.0.0.0}"
echo "Headers:   COOP/COEP enabled (for SharedArrayBuffer)"
echo ""
exec python3 - "$PORT" "$BIND" "$SIM_DIR" <<'PYEOF'
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1])
bind = sys.argv[2]
directory = sys.argv[3]

class COIHandler(SimpleHTTPRequestHandler):
    # Pin the served directory explicitly so the handler does not fall back to
    # os.getcwd() (the base __init__ does that when `directory` is not passed,
    # and a stale/removed cwd — e.g. after `make clean` + `make wasm` while the
    # server is running — would crash every request with FileNotFoundError).
    # Pass `directory` to super().__init__ so the base class uses it instead of
    # calling os.getcwd() itself.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)
    # Enable cross-origin isolation so SharedArrayBuffer is available.
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

ThreadingHTTPServer((bind, port), COIHandler).serve_forever()
PYEOF
