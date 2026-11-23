// Flatten the editor document into the wire layout expected by wasm.cc
// setWteDocument(const float *doc, int count).
//
// Wire layout (row-major, one Float32Array):
//   per wave (16 x):
//     combine (1) | invert (1) | mirror (1)
//     per source slot (4 x):
//       enabled (1) | type (1) | harmonic (1) | phase (1) | gain (1) | bias (1)
//       psr_count (1)
//       per warp (psr_count x 8): type (1) | enabled (1) | p0..p8 (9)
//     out_count (1)
//     per warp (out_count x 8): type (1) | enabled (1) | p0..p8 (9)
// Total: 16 * (3 + 4 * (7 + 8*11 + 1) + 8*11) = 16 * 390 = 6240 floats.

import { WAVE_COUNT, SOURCES_PER_WAVE, WARP_META } from './types';

// Map TS source type name -> C++ index (0..29). The order in SOURCE_TYPES
// in types.ts must match the case order in osc_wte_render.h source_shape().
// We derive the index by name lookup.
const SOURCE_TYPE_ORDER = [
  'Sine','Triangle','Saw','Square','Pulse','Half Sine','Parabola','Bell',
  'S-Curve','Trapezoid','Stairs','Sine²','FM Sine','Sinc','Chirp','Wave Pkt',
  'Smooth Sq','Sharkfin','Spike','Noise','Cubed','Two Bumps','Folded',
  'Asym Tri','Pinch','PD Sine','Organ','Sync Saw','Bit Crush','Ring Sine',
];
function srcTypeIdx(name) {
  const i = SOURCE_TYPE_ORDER.indexOf(name);
  return i >= 0 ? i : 0;
}

// Map TS warp type name -> C++ index (0..14). Order matches WARP_TYPES.
const WARP_TYPE_ORDER = [
  'Wavefold','Hard Sync','FM','Waveshape','Bitcrush','Exponent',
  'CZ Reso','Bend','PWM','Asym','Formant','Spectral Tilt','Ring Mod',
  'Additive Shape','Math',
];
function warpTypeIdx(name) {
  const i = WARP_TYPE_ORDER.indexOf(name);
  return i >= 0 ? i : 0;
}

// Combine mode name -> index (0..5). Order matches COMBINE_MODES.
const COMBINE_ORDER = ['Sum','Multiply','Min','Max','AM','Wrap'];
function combineIdx(mode) {
  const i = COMBINE_ORDER.indexOf(mode);
  return i >= 0 ? i : 0;
}

// Waveshape mode name -> enum (0..3)
function waveshapeModeIdx(mode) {
  switch (mode) {
    case 'Tahn': return 0;
    case 'Atan': return 1;
    case 'Cubic': return 2;
    case 'Sin': return 3;
    default: return 0;
  }
}

// CZ Reso window name -> enum (0..2)
function czWindowIdx(win) {
  switch (win) {
    case 'Saw': return 0;
    case 'Tri': return 1;
    case 'Trap': return 2;
    default: return 0;
  }
}

// Serialize one warp (WarpItem) -> [type, enabled, p0..p8] (11 floats)
function serializeWarp(w) {
  const typeIdx = warpTypeIdx(w.type);
  const meta = WARP_META[w.type];
  const params = new Array(9).fill(0);
  if (meta && w.params) {
    for (let ki = 0; ki < meta.params.length && ki < 9; ki++) {
      const spec = meta.params[ki];
      const val = w.params[spec.key];
      if (val === undefined) continue;
      if (spec.kind === 'mode') params[ki] = waveshapeModeIdx(val);
      else if (spec.kind === 'window') params[ki] = czWindowIdx(val);
      else params[ki] = typeof val === 'number' ? val : 0;
    }
  }
  const arr = [typeIdx, w.enabled ? 1 : 0, ...params];
  return arr;
}

// Serialize one source slot (SourceConfig) -> floats
function serializeSource(src) {
  const typeIdx = srcTypeIdx(src.type);
  const base = [
    src.enabled ? 1 : 0,
    typeIdx,
    src.harmonic ?? 1,
    src.phase ?? 0,
    src.gain ?? 1,
    src.bias ?? 0,
  ];
  return base;
}

// Serialize the full document (WaveConfig[]) -> Float32Array
export function serializeWteDoc(waves) {
  const out = new Float32Array(16 * 390);
  let i = 0;
  for (let f = 0; f < WAVE_COUNT; f++) {
    const w = waves[f] || waves[0];
    // combine, invert, mirror
    out[i++] = combineIdx(w.combine);
    out[i++] = w.modifiers?.invert ? 1 : 0;
    out[i++] = w.modifiers?.mirror ? 1 : 0;
    for (let s = 0; s < SOURCES_PER_WAVE; s++) {
      const src = w.sources[s];
      // enabled, type, harmonic, phase, gain, bias
      out[i++] = src?.enabled ? 1 : 0;
      out[i++] = src ? srcTypeIdx(src.type) : 0;
      out[i++] = src?.harmonic ?? 1;
      out[i++] = src?.phase ?? 0;
      out[i++] = src?.gain ?? 1;
      out[i++] = src?.bias ?? 0;
      // psr_count + warps
      const psr = w.perSourceWarp?.[s] || [];
      const pc = Math.min(psr.length, 8);
      out[i++] = pc;
      for (let k = 0; k < pc; k++) {
        const wf = serializeWarp(psr[k]);
        for (let p = 0; p < 11; p++) out[i++] = wf[p];
      }
    }
    // out_count + output warps
    const ow = w.outputWarp || [];
    const oc = Math.min(ow.length, 8);
    out[i++] = oc;
    for (let k = 0; k < oc; k++) {
      const wf = serializeWarp(ow[k]);
      for (let p = 0; p < 11; p++) out[i++] = wf[p];
    }
  }
  return out;
}
