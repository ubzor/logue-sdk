#!/bin/bash
# Build (if needed) and serve the WASM web simulator for drone NTS-1 mkII

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$SCRIPT_DIR/sim"

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
