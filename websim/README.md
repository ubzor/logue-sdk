# how to set up web audio simulator

``
git submodule update --init
``
to get emsdk, and then follow the official instruction: https://emscripten.org/docs/getting_started/downloads.html

``
cd tools/emsdk
./emsdk install latest
./emsdk activate latest
``

# how to compile to wasm and test in browser
For the DRONE family the only web simulator is the unified chain page:
instead of `make install` (which creates a hardware-specific unit file), run

``
cd platform/nts-1_mkii/drone-chain && make wasm
``

This builds the full OSC -> MOD -> DEL -> REV audio path into ONE wasm audio
worklet (drone-chain/sim/index.html). Serve it with
`platform/nts-1_mkii/serve.sh` (http://localhost:8000/), which adds the
COOP/COEP headers the SharedArrayBuffer build needs.

This directory is the shared shell/project for that simulator:
- `index.html` — the page shell (linked into drone-chain/sim/index.html);
- `dsp/*.c` — the LUT/stub sources the chain wasm links in;
- `entry.js` / `app.css` + `vite*.js` — the FlyonUI asset build (flyon.css/js).
The per-unit standalone pages (osc.html / fx.html) are gone.
