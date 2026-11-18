#!/usr/bin/env bash
# serve.sh — сборка и запуск SvelteKit-симулятора DRONE (drone-chain, Вариант B).
#
# Единая точка запуска: перенесена из platform/nts-1_mkii/serve.sh (тот удалён)
# + добавлено убийство процесса на порту перед стартом.
#
#   0. Убивает любой процесс, слушающий $PORT (предыдущий сервер).
#   1. Пересобирает WASM (drone-chain/sim), если DSP/glue исходники старше
#      артефактов (make wasm) и синхронизирует glue+wasm в app/static/.
#   2. Пересобирает SvelteKit app, если исходники/конфиг старше build/client.
#   3. relink-glue.mjs (re-copy static/ -> build/client/, убрать stale .br/.gz).
#   4. Запускает `node build` на $HOST:$PORT (adapter-node).
#
# Usage:
#   ./serve.sh [PORT] [BIND]      # default: 8000 0.0.0.0

set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
# platform/nts-1_mkii sits two levels up from websim/app (repo root/platform/...).
PLATFORM_DIR="$(cd "$APP_DIR/../../platform/nts-1_mkii" && pwd)"
SERVE_MODULE="drone-chain"
SIM_DIR="$PLATFORM_DIR/$SERVE_MODULE/sim"
M="$PLATFORM_DIR/$SERVE_MODULE"

# Bind address / port: CLI args win over env; adapter-node honors HOST/PORT env.
PORT="${1:-${PORT:-8000}}"
HOST="${2:-${HOST:-0.0.0.0}}"

# ---- 0. Kill anything already listening on $PORT -------------------------
find_port_pids() {
  local pids=""
  pids=$(ss -ltnp 2>/dev/null | grep ":${PORT}" | grep -oP 'pid=\K[0-9]+' | sort -u || true)
  if [ -z "$pids" ]; then
    pids=$(fuser "${PORT}/tcp" 2>/dev/null | tr -s ' ' | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -u || true)
  fi
  if [ -z "$pids" ]; then
    pids=$(lsof -ti "tcp:${PORT}" 2>/dev/null | sort -u || true)
  fi
  if [ -z "$pids" ]; then
    pids=$(ps -eo pid,comm,args 2>/dev/null | grep 'node build' | grep -v grep | awk '{print $1}' | sort -u || true)
  fi
  echo "$pids"
}
PIDS="$(find_port_pids)"
if [ -n "$PIDS" ]; then
  echo "serve: killing PID(s) on :$PORT -> $PIDS"
  # shellcheck disable=SC2086
  kill $PIDS 2>/dev/null || true
  sleep 1
  PIDS2="$(find_port_pids)"
  if [ -n "$PIDS2" ]; then
    echo "serve: force-killing survivors on :$PORT -> $PIDS2"
    # shellcheck disable=SC2086
    kill -9 $PIDS2 2>/dev/null || true
    sleep 1
  fi
else
  echo "serve: port :$PORT is free"
fi

# ---- 1. Build the SvelteKit app if missing or stale -----------------------
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

# ---- 2. Build the WASM simulator if missing or stale -----------------------
# Any change to the chain or to the sibling DSP modules (drone-osc/modfx/
# delfx/revfx headers) must rebuild the worklet.
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
  app_stale=1
fi

# ---- 3. Build the SvelteKit app (if stale) --------------------------------
if [ "$app_stale" -eq 1 ]; then
  echo "============================================"
  echo "Building SvelteKit app (websim/app)..."
  echo "============================================"
  cd "$APP_DIR"
  if [ ! -d "$APP_DIR/node_modules" ]; then
    echo "serve: node_modules missing - running npm install..."
    npm install --no-audit --no-fund
  fi
  # Sync the emscripten glue + wasm into the app's static/ so the built client
  # serves them from the document root (/index.js, /index.wasm).
  mkdir -p "$APP_DIR/static"
  cp -p "$SIM_DIR/index.js" "$APP_DIR/static/index.js"
  cp -p "$SIM_DIR/index.wasm" "$APP_DIR/static/index.wasm"
  npm run build
  node "$APP_DIR/scripts/relink-glue.mjs"
  echo ""
fi

# ---- 4. Start the server --------------------------------------------------
echo "============================================"
echo "Serving $SERVE_MODULE web simulator (SvelteKit)"
echo "============================================"
echo "URL:       http://localhost:${PORT}/"
echo "App:       $APP_DIR/build (adapter-node)"
echo "Wasm:      $SIM_DIR"
echo "Press Ctrl+C to stop the server"
echo "============================================"

# COOP/COEP headers for the wasm audio worklet (SharedArrayBuffer) come from
# the app's server hooks (src/hooks.server.ts). adapter-node honors HOST/PORT.
cd "$APP_DIR"
exec env PORT="$PORT" HOST="$HOST" node build