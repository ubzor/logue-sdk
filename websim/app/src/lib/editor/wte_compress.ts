// Wire-format compressor for the /editor document (the 16 WaveConfig recipes).
//
// WEB-SIDE mirror of drone-osc/osc_wte_wire.h. It must produce a byte-identical
// COMPACT stream to the C++ reference encoder (wte_wire_encode) so the device
// decoder (wte_wire_decode) round-trips exactly. The compact stream is
// raw-deflated (pako) and wrapped in the 11-byte packet header; the resulting
// Uint8Array is handed to the wasm `setWteDocument` binding.
//
// Layout (MSB-first bit packing, mirrors osc_wte_wire.h):
//   per wave (16): combine(3) | invert(1) | mirror(1)
//     per source slot (4): enabled(1); if enabled: type(5)|harmonic(8)|phase(8)|gain(8)|bias(8)
//       psr_count(3); per warp: type(4)|enabled(1)|mask(9) + q12 values for set bits
//     out_count(4); per warp: type(4)|enabled(1)|mask(9) + q12 values
//
// Quant (mirrors osc_wte_wire.h): harmonic 8b 0..16, phase 8b 0..1, gain 8b
// 0..2, bias 8b -1..1, warp params 12b in per-param [lo,hi) (wteParamRange). A
// warp param == its catalog default (post-quant) is omitted (mask bit 0).
//
// Packet header (11 B): magic 3B (0x57 0x54 0x01) | version 1B | flags 1B
// (bit0=deflate) | crc32 4B LE (over compact) | raw_len 2B LE (compact len).
// The device inflates the body with no-alloc miniz tinfl, verifies the CRC, and
// decodes the compact stream.

import { deflate } from 'pako';
import { WARP_META } from './types';

// ---- catalog constants (mirror types.ts / osc_wte_doc.h) -------------------
const WAVE_COUNT = 16;
const SOURCES_PER_WAVE = 4;
const MAX_WARPS_PER_STACK = 8;
const MAX_WARP_PARAMS = 9;
const WTE_WIRE_VERSION = 1;
const WTE_WIRE_FLAG_DEFLATE = 0x01;

// Catalog order (index == C++ type id). Mirror of WARP_TYPE_ORDER in
// serialize.ts and the WTE_WARP_* enum in osc_wte_doc.h.
const WARP_TYPE_ORDER = [
  'Wavefold', 'Hard Sync', 'FM', 'Waveshape', 'Bitcrush', 'Exponent',
  'CZ Reso', 'Bend', 'PWM', 'Asym', 'Formant', 'Spectral Tilt', 'Ring Mod',
  'Additive Shape', 'Math',
];
function warpTypeIdx(name: string): number {
  const i = WARP_TYPE_ORDER.indexOf(name);
  return i >= 0 ? i : 0;
}

// Combine mode name -> index (0..5). Mirror of COMBINE_ORDER / WTE_COMBINE_*.
const COMBINE_ORDER = ['Sum', 'Multiply', 'Min', 'Max', 'AM', 'Wrap'];
function combineIdx(mode: string): number {
  const i = COMBINE_ORDER.indexOf(mode);
  return i >= 0 ? i : 0;
}

// ---- quantization (mirror osc_wte_wire.h) -----------------------------------
function q8HarmonicQuant(v: number): number {
  v = Math.max(0, Math.min(16, v));
  return Math.round(v * (255 / 16));
}
function q8PhaseQuant(v: number): number {
  v = Math.max(0, Math.min(1, v));
  return Math.round(v * 255);
}
function q8GainQuant(v: number): number {
  v = Math.max(0, Math.min(2, v));
  return Math.round(v * (255 / 2));
}
function q8BiasQuant(v: number): number {
  v = Math.max(-1, Math.min(1, v));
  return Math.round(v * (255 / 2) + 127.5);
}
function q12ParamQuant(v: number, lo: number, hi: number): number {
  let t = hi > lo ? (v - lo) / (hi - lo) : 0;
  t = Math.max(0, Math.min(1, t));
  return Math.round(t * 4095);
}

// Per-param [lo,hi) for quantization. MUST match wte_param_range() in
// osc_wte_wire.h and the editor slider ranges (types.ts WARP_META). `key` is
// the index of the param within the warp's catalog (0..param_count-1).
function wteParamRange(typeIdx: number, key: number): [number, number] {
  const t = WARP_TYPE_ORDER[typeIdx];
  switch (t) {
    case 'Wavefold':     return key === 0 ? [0, 4] : [-1, 1];
    case 'Hard Sync':    return [0, 4];
    case 'FM':           return key === 0 ? [0, 8] : [0, 4];
    case 'Waveshape':    return key === 0 ? [0, 4] : [0, 4];
    case 'Bitcrush':     return key === 0 ? [1, 16] : [0, 1];
    case 'Exponent':     return [0.1, 8];
    case 'CZ Reso':      return key === 0 ? [0, 1] : [0, 2];
    case 'Bend':         return [-1, 1];
    case 'PWM':          return [0, 1];
    case 'Asym':         return [-1, 1];
    case 'Formant':      return [-1, 1];
    case 'Spectral Tilt': return key === 0 ? [0, 2] : [0, 1];
    case 'Ring Mod':     return key === 0 ? [0, 8] : [0, 1];
    case 'Additive Shape': return key < 8 ? [-1, 1] : [0, 1];
    case 'Math':         return [0, 1];
    default:             return [0, 1];
  }
}

// ---- Bit writer (MSB-first, mirror BitWriter in osc_wte_wire.h) --------------
class BitWriter {
  buf: Uint8Array;
  cap: number;
  pos = 0; // bits written
  overflow = false;

  constructor(buf: Uint8Array, cap: number) {
    this.buf = buf;
    this.cap = cap;
  }

  put(v: number, n: number) {
    for (let i = 0; i < n; i++) {
      const byte = this.pos >> 3;
      if (byte >= this.cap) { this.overflow = true; return; }
      const bit = (v >> (n - 1 - i)) & 1; // MSB-first
      if (bit) this.buf[byte] |= 1 << (7 - (this.pos & 7));
      this.pos++;
    }
  }

  byteLen(): number { return (this.pos + 7) >> 3; }
}

// ---- CRC-32 (IEEE, bit-by-bit, mirror wte_crc32) -----------------------------
function crc32(data: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    c ^= data[i];
    for (let b = 0; b < 8; b++) c = (c >>> 1) ^ (0xedb88320 & (-(c & 1)));
  }
  return (c ^ 0xffffffff) >>> 0;
}

// ---- param helpers ----------------------------------------------------------
// Normalize a WarpItem's {key: value} params into a fixed p0..p8 array in
// catalog order (mirrors serializeWarp in serialize.ts). Missing keys take the
// catalog default. `typeIdx` selects the warp; `params` is the record.
function warpParamsArray(typeIdx: number, params: Record<string, number>): number[] {
  const name = WARP_TYPE_ORDER[typeIdx];
  const meta = WARP_META[name];
  const out = new Array(MAX_WARP_PARAMS).fill(0);
  if (meta && params) {
    for (let p = 0; p < meta.params.length && p < MAX_WARP_PARAMS; p++) {
      const key = meta.params[p].key;
      const v = params[key];
      out[p] = typeof v === 'number' ? v : meta.params[p].def;
    }
  }
  return out;
}

// A warp param is at default when its quantized value equals the catalog
// default's quantized value (mirror wte_param_is_default).
function paramIsDefault(typeIdx: number, key: number, v: number, metaParamDef: number): boolean {
  const [lo, hi] = wteParamRange(typeIdx, key);
  return q12ParamQuant(v, lo, hi) === q12ParamQuant(metaParamDef, lo, hi);
}

// Serialize one warp (WarpItem) into the bit stream: type(4) | enabled(1) |
// param_mask(9) | q12 values for non-default params. Mirrors the warp write in
// wte_wire_encode. `warp` is a WarpItem ({type, enabled, params}).
function writeWarp(w: BitWriter, warp: any) {
  const typeIdx = warpTypeIdx(warp?.type);
  const enabled = !!(warp && warp.enabled);
  w.put(typeIdx, 4);
  w.put(enabled ? 1 : 0, 1);
  const name = WARP_TYPE_ORDER[typeIdx];
  const meta = WARP_META[name];
  const paramCount = meta ? meta.params.length : 0;
  const p = warpParamsArray(typeIdx, warp?.params || {});
  let mask = 0;
  for (let i = 0; i < paramCount && i < MAX_WARP_PARAMS; i++) {
    if (!paramIsDefault(typeIdx, i, p[i], meta.params[i].def)) mask |= (1 << i);
  }
  w.put(mask, 9);
  for (let i = 0; i < paramCount && i < MAX_WARP_PARAMS; i++) {
    if (mask & (1 << i)) {
      const [lo, hi] = wteParamRange(typeIdx, i);
      w.put(q12ParamQuant(p[i], lo, hi), 12);
    }
  }
}

// ---- main entry -------------------------------------------------------------
// Compress the editor document (waves: the 16 WaveConfig recipes) into the wire
// packet. Returns a Uint8Array (header + raw-deflate compact stream) ready for
// `setWteDocument`. The waves array must have exactly WAVE_COUNT entries (the
// editor's doc.waves); shorter/longer is padded/truncated.
export function compressWteDoc(waves: any[]): Uint8Array {
  const compact = new Uint8Array(8192);
  const w = new BitWriter(compact, compact.length);

  for (let f = 0; f < WAVE_COUNT; f++) {
    const wave = waves[f] || null;
    w.put(combineIdx(wave ? wave.combine : 'Sum'), 3);
    w.put(wave && wave.invert ? 1 : 0, 1);
    w.put(wave && wave.mirror ? 1 : 0, 1);

    for (let s = 0; s < SOURCES_PER_WAVE; s++) {
      const src = wave ? (wave.sources?.[s] || null) : null;
      const enabled = !!(src && src.enabled);
      w.put(enabled ? 1 : 0, 1);
      if (enabled) {
        w.put(warpTypeIdx(src.type), 5);
        w.put(q8HarmonicQuant(src.harmonic), 8);
        w.put(q8PhaseQuant(src.phase), 8);
        w.put(q8GainQuant(src.gain), 8);
        w.put(q8BiasQuant(src.bias), 8);
      }
      const psr = (src && src.psr) || [];
      const pc = Math.min(psr.length, MAX_WARPS_PER_STACK);
      w.put(pc, 3);
      for (let k = 0; k < pc; k++) writeWarp(w, psr[k]);
    }

    const out = (wave && wave.out) || [];
    const oc = Math.min(out.length, MAX_WARPS_PER_STACK);
    w.put(oc, 4);
    for (let k = 0; k < oc; k++) writeWarp(w, out[k]);
  }

  if (w.overflow) throw new Error('wte_compress: compact buffer overflow');
  const compactLen = w.byteLen();
  const compactFinal = compact.subarray(0, compactLen);

  // Raw-deflate (no zlib header), matching the device's tinfl raw inflate.
  // windowBits -15 = raw deflate; level 9 = best (same as the C++ self-test's
  // Z_BEST_COMPRESSION). The device verifies the CRC32 over the compact stream
  // (not the deflate stream itself) as the integrity check.
  const raw = deflate(compactFinal, { level: 9, windowBits: -15 });

  // ---- packet header (11 bytes) ----
  const packet = new Uint8Array(11 + raw.length);
  packet[0] = 0x57; packet[1] = 0x54; packet[2] = WTE_WIRE_VERSION;
  packet[3] = WTE_WIRE_VERSION;
  packet[4] = WTE_WIRE_FLAG_DEFLATE;
  const c = crc32(compactFinal);
  packet[5] = c & 0xff;
  packet[6] = (c >> 8) & 0xff;
  packet[7] = (c >> 16) & 0xff;
  packet[8] = (c >> 24) & 0xff;
  packet[9] = compactLen & 0xff;
  packet[10] = (compactLen >> 8) & 0xff;
  packet.set(raw, 11);
  return packet;
}