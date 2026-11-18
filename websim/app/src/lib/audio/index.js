// Public entry of the audio wiring: re-exports setupWebAudioAndUI so the
// emscripten glue (static/index.js, patched by scripts/relink-glue.mjs) can
// dynamic-import ONE stable module. Kept separate from the page node, whose
// production build tree-shakes the wiring out (the page never references it
// in JS).
export { setupWebAudioAndUI } from './setup-web-audio.js';
