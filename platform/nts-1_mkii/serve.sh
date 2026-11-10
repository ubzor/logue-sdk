#!/bin/bash
# Serve the WASM web simulator for the drone NTS-1 mkII units.
#
# Currently serves the drone-osc (oscillator) simulator. In the future this
# script will serve all 4 modules (drone-osc / drone-modfx / drone-delfx /
# drone-revfx) on a single page; the common serving root and the build helpers
# below are already structured so that only the module list and the landing
# page need to change when the multi-module UI is added.
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

# The module to serve now (oscillator). Its sim/ dir is the document root.
SERVE_MODULE="drone-osc"
SIM_DIR="$PLATFORM_DIR/$SERVE_MODULE/sim"

# ---- FlyonUI (Tailwind v4) assets (Vite) -------------------------------------
# Always rebuild (no stale-check): osc.html, app.css, entry.js, the vite config
# and the flyonui package can all drift independently. Vite is fast (~1 s).
echo "============================================"
echo "Building FlyonUI assets (Vite)..."
echo "============================================"
cd "$SANDBOX_DIR"
if [ ! -d node_modules ]; then
  echo "node_modules missing - running npm install..."
  npm install --no-audit --no-fund
fi
npx vite build
# Copy the built flyon.css + flyon.js into the served sim/ so the simulator
# has them even when the wasm build below is skipped (up to date).
mkdir -p "$SIM_DIR"
cp -p "$SANDBOX_DIR/sim/flyon.css" "$SIM_DIR/flyon.css"
cp -p "$SANDBOX_DIR/sim/flyon.js"  "$SIM_DIR/flyon.js"
# Vercel deploy config (COOP/COEP headers): regenerate so it is available even
# when the wasm build is skipped. sim/ is git-ignored and wiped on rebuild.
cat > "$SIM_DIR/vercel.json" <<'VERCELEOF'
{
  "headers": [
    {
      "source": "/(.*)",
      "headers": [
        { "key": "Cross-Origin-Opener-Policy", "value": "same-origin" },
        { "key": "Cross-Origin-Embedder-Policy", "value": "require-corp" },
        { "key": "Cross-Origin-Resource-Policy", "value": "same-origin" }
      ]
    }
  ]
}
VERCELEOF
echo ""

# ---- Build the WASM simulator if missing or stale -----------------------------
# Stale if any source is newer than drone.html, or if the generated assets are
# incomplete (html exists but wasm/js are missing -> interrupted build).
if [ ! -f "$SIM_DIR/drone.html" ] || [ ! -f "$SIM_DIR/drone.wasm" ] || [ ! -f "$SIM_DIR/drone.js" ] || \
   [ "$PLATFORM_DIR/$SERVE_MODULE/wasm.cc" -nt "$SIM_DIR/drone.html" ] || \
   [ "$PLATFORM_DIR/$SERVE_MODULE/header.c" -nt "$SIM_DIR/drone.html" ] || \
   [ "$PLATFORM_DIR/$SERVE_MODULE/osc.h" -nt "$SIM_DIR/drone.html" ] || \
   [ "$PLATFORM_DIR/$SERVE_MODULE/Makefile" -nt "$SIM_DIR/drone.html" ] || \
   [ "$PLATFORM_DIR/$SERVE_MODULE/config.mk" -nt "$SIM_DIR/drone.html" ] || \
   [ "$SANDBOX_DIR/osc.html" -nt "$SIM_DIR/drone.html" ]; then
  echo "============================================"
  echo "Building WASM simulator ($SERVE_MODULE)..."
  echo "============================================"
  (cd "$PLATFORM_DIR/$SERVE_MODULE" && make wasm)
  echo ""
fi

echo "============================================"
echo "Serving $SERVE_MODULE web simulator"
echo "============================================"
echo "URL:       http://localhost:${1:-8000}/drone.html"
echo "Directory: $SIM_DIR"
echo ""
echo "Press Ctrl+C to stop the server"
echo "============================================"

# Serve with a tiny Python HTTP server that adds the COOP/COEP headers the
# Wasm audio worklet build (-sWASM_WORKERS + -sSHARED_MEMORY) needs for
# SharedArrayBuffer (cross-origin isolation). Plain `python3 -m http.server`
# does not send them, so we use a custom handler.
#
# Do NOT cd into $SIM_DIR before exec: if the server runs while `make clean`
# removes (and `make wasm` recreates) $SIM_DIR, the process's cwd becomes
# invalid and the Python runtime can stall on os.getcwd(). Pin cwd to
# $PLATFORM_DIR (stable) and pass $SIM_DIR to the handler.
cd "$PLATFORM_DIR"
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
    # os.getcwd() (which a stale/removed cwd would break after make clean + wasm).
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