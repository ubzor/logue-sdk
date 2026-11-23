#pragma once
#include <cstdint>
#include <cstddef>
/*
 *  File: osc_wte_wire.h
 *
 *  Wire format for the Wave-Table-Editor document (WTEDoc): a COMPACT,
 *  self-contained, versioned, CRC-checked binary that the web simulator
 *  compresses with deflate and the device (wasm sim, or the future ARM unit)
 *  inflates + decodes back into a WTEDoc.
 *
 *  WHY COMPRESSED
 *  A full WTEDoc is 6240 fields (16 waves x 390). The old flat float32 wire
 *  sent all of them = 24 960 bytes per edit. The document is sparse in
 *  practice (most waves: 1 source, 0 warps; most params at catalog default),
 *  so this format captures that sparsity with three layers:
 *    1. QUANTIZATION   — every float becomes 8/12-bit in a field-specific range.
 *    2. DEFAULT-OMISSION — a warp param == catalog default is not transmitted
 *                          (only a 9-bit "non-default" mask is).
 *    3. ENABLED-OMISSION — a disabled source/warp sends only its enable bit.
 *  The compact byte stream is then raw-deflate'd (miniz tinfl) ~10:1. End to
 *  end a typical doc (16 waves, 1 sine, 0 warps) is ~80–150 compressed bytes;
 *  a mid one ~300; worst (full) ~1.5–3 KB. All far below the old 24 960.
 *
 *  PACKET LAYOUT
 *      [ magic 3 B ][ version 1 B ][ flags 1 B ][ crc32 4 B LE ][ raw_len 2 B LE ][ deflate(compact) N B ]
 *    magic   = 0x57 0x54 0x01 ('W','T',WTE_WIRE_VERSION)
 *    version = WTE_WIRE_VERSION (decoder rejects anything else)
 *    flags   = bit0: payload is raw-deflate (1). Reserved for a future
 *              "catalog of N docs" bit.
 *    crc32   = CRC-32 (IEEE) of the compact (post-inflate) stream. Checked
 *              after inflate, before decode.
 *    raw_len = compact stream size (== what inflate must emit; bounds the
 *              inflate output so a corrupt length cannot overrun).
 *
 *  STORAGE MODEL (future, not wired up): the packet is SELF-CONTAINED (no
 *  external state, no cross-doc deltas), so a future "bank of N docs in
 *  NVRAM" stores N blobs back-to-back and decodes each independently. The
 *  NVRAM layer itself is a later task (the SDK in this repo has no storage
 *  API; today the packet is only transported and held in RAM).
 *
 *  ENCODE vs DECODE: ENCODE (compact + deflate) runs on the WEB page
 *  (wte_compress.ts, pako.deflate); the reference C++ encoder below
 *  (wte_wire_encode) is the canonical layout spec + round-trip test anchor.
 *  DECODE (inflate + uncompact) runs ON THE DEVICE (this file), shared by the
 *  wasm sim and, later, the ARM unit — the device always works from the
 *  compressed blob.
 *
 *  RESOURCES: no allocation. Inflate uses the no-alloc tinfl one-shot helper
 *  (common/compress/miniz_tinfl.h); the compact buffer + WTEDoc are caller-
 *  owned. Decode stack cost = one tinfl_decompressor (~8 KB) + one compact
 *  buffer (<= WTE_WIRE_COMPACT_MAX). Call site is the main-thread param
 *  channel (setWteDocument), never the audio path.
 */

#include "osc_wte_doc.h"
#include "../common/compress/miniz_tinfl.h"

namespace wte_wire
{
// ---- Constants ---------------------------------------------------------------

// Packet format version (also in the 3-byte magic). Bump on layout change.
inline constexpr uint8_t WTE_WIRE_VERSION = 1U;

// flags byte: bit0 = payload is raw-deflate.
inline constexpr uint8_t WTE_WIRE_FLAG_DEFLATE = 0x01U;

// Header length: magic(3)+version(1)+flags(1)+crc32(4)+raw_len(2) = 11.
inline constexpr uint32_t WTE_WIRE_HDR_LEN = 11U;

// Max size of the compact (uncompressed) stream. Realistic docs are ~1–3 KB;
// this is the safety ceiling for the fixed decode buffer. The absolute
// theoretical worst (16 waves, each 4 sources + 16 warps, all 9 params
// non-default) is ~11 KB and would overflow this — but the editor UI cannot
// produce that (per-source stack cap is 8, and no user fills all 16 waves to
// capacity). If it ever must, raise this (it only sizes a decode buffer).
inline constexpr uint32_t WTE_WIRE_COMPACT_MAX = 8192U;

// Max compressed payload accepted. A valid compressed doc is always smaller
// than its compact form for our data; cap the input to the compact max so a
// corrupt/huge packet is rejected before inflate.
inline constexpr uint32_t WTE_WIRE_COMPRESSED_MAX = WTE_WIRE_COMPACT_MAX + 64U;

// ---- CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) --------------------------
// Bit-by-bit (no 1 KB table): the CRC is computed once per packet (< 8 KB),
// off the audio path, so the slower table-less form is fine and keeps the unit
// free of a read-only CRC table. Returns the standard 0xFFFFFFFF-xor'd value.
inline uint32_t wte_crc32(const uint8_t *data, size_t len)
{
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
  {
    c ^= data[i];
    for (int b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
  }
  return c ^ 0xFFFFFFFFu;
}

// ---- Bit writer / bit reader (MSB-first, like deflate) ------------------------

struct BitWriter
{
  uint8_t *buf;
  uint32_t cap; // bytes
  uint32_t pos; // bits written
  bool overflow;

  void init(uint8_t *b, uint32_t c) { buf = b; cap = c; pos = 0; overflow = false; }

  void put(uint32_t v, uint32_t n)
  {
    // MSB-first: write the n high bits of v, most-significant first (matching
    // BitReader::get and the deflate convention).
    for (uint32_t i = 0; i < n; i++)
    {
      uint32_t byte = pos >> 3;
      if (byte >= cap) { overflow = true; return; }
      uint32_t bit = (v >> (n - 1u - i)) & 1u;
      uint32_t off = 7u - (pos & 7u);
      if (bit) buf[byte] |= (uint8_t)(1u << off);
      pos++;
    }
  }

  uint32_t byte_len() const { return (pos + 7u) >> 3; }
};

struct BitReader
{
  const uint8_t *buf;
  uint32_t cap; // bytes
  uint32_t pos; // bits consumed
  bool ok;

  void init(const uint8_t *b, uint32_t c) { buf = b; cap = c; pos = 0; ok = true; }

  uint32_t get(uint32_t n)
  {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
    {
      uint32_t byte = pos >> 3;
      if (byte >= cap) { ok = false; return 0; }
      uint32_t off = 7u - (pos & 7u);
      v = (v << 1) | ((buf[byte] >> off) & 1u);
      pos++;
    }
    return v;
  }
};

// ---- Quantization ------------------------------------------------------------
// Field-specific 8/12-bit quant. Ranges mirror the editor model (types.ts) and
// the C++ renderer expectations (osc_wte_render.h).

// harmonic: 0..16, 8 bits.
inline float q8_harmonic_dequant(uint32_t q) { return (float)q * (16.0f / 255.0f); }
inline uint32_t q8_harmonic_quant(float v)
{
  if (v < 0.0f) v = 0.0f;
  if (v > 16.0f) v = 16.0f;
  return (uint32_t)((v * (255.0f / 16.0f)) + 0.5f);
}

// phase: 0..1, 8 bits.
inline float q8_phase_dequant(uint32_t q) { return (float)q * (1.0f / 255.0f); }
inline uint32_t q8_phase_quant(float v)
{
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  return (uint32_t)((v * 255.0f) + 0.5f);
}

// gain: 0..2, 8 bits.
inline float q8_gain_dequant(uint32_t q) { return (float)q * (2.0f / 255.0f); }
inline uint32_t q8_gain_quant(float v)
{
  if (v < 0.0f) v = 0.0f;
  if (v > 2.0f) v = 2.0f;
  return (uint32_t)((v * (255.0f / 2.0f)) + 0.5f);
}

// bias: -1..+1, 8 bits (offset 127.5).
inline float q8_bias_dequant(uint32_t q) { return ((float)q - 127.5f) * (2.0f / 255.0f); }
inline uint32_t q8_bias_quant(float v)
{
  if (v < -1.0f) v = -1.0f;
  if (v > 1.0f) v = 1.0f;
  return (uint32_t)((v * (255.0f / 2.0f)) + 127.5f + 0.5f);
}

// warp param: 12 bits, range from wte_param_range(). Enum params store their
// 0..N index (fits 12 bits); numeric params map to [lo..hi].
inline float q12_param_dequant(uint32_t q, float lo, float hi)
{
  const float t = (float)q * (1.0f / 4095.0f);
  return lo + t * (hi - lo);
}
inline uint32_t q12_param_quant(float v, float lo, float hi)
{
  float t = 0.0f;
  if (hi > lo) t = (v - lo) / (hi - lo);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return (uint32_t)(t * 4095.0f + 0.5f);
}

// Per-param [lo,hi) for quantization. Mirrors the editor slider ranges
// (types.ts WARP_META). Unknown (type,key) -> [0,1].
inline void wte_param_range(uint8_t type, uint8_t key, float *lo, float *hi)
{
  const WteWarpMeta &m = wte_warp_meta(type);
  *lo = 0.0f; *hi = 1.0f;
  if (key >= m.param_count) return;
  switch (type)
  {
    case WTE_WARP_WAVEFOLD:
      if (key == 0) { *lo = 0.0f; *hi = 4.0f; } else { *lo = -1.0f; *hi = 1.0f; }
      break;
    case WTE_WARP_HARD_SYNC: *lo = 0.0f; *hi = 4.0f; break;
    case WTE_WARP_FM:
      if (key == 0) { *lo = 0.0f; *hi = 8.0f; } else { *lo = 0.0f; *hi = 4.0f; }
      break;
    case WTE_WARP_WAVESHAPE:
      if (key == 0) { *lo = 0.0f; *hi = 4.0f; } else { *lo = 0.0f; *hi = 4.0f; }
      break;
    case WTE_WARP_BITCRUSH:
      if (key == 0) { *lo = 1.0f; *hi = 16.0f; } else { *lo = 0.0f; *hi = 1.0f; }
      break;
    case WTE_WARP_EXPONENT: *lo = 0.1f; *hi = 8.0f; break;
    case WTE_WARP_CZ_RESO:
      if (key == 0) { *lo = 0.0f; *hi = 1.0f; } else { *lo = 0.0f; *hi = 2.0f; }
      break;
    case WTE_WARP_BEND: *lo = -1.0f; *hi = 1.0f; break;
    case WTE_WARP_PWM: *lo = 0.0f; *hi = 1.0f; break;
    case WTE_WARP_ASYM: *lo = -1.0f; *hi = 1.0f; break;
    case WTE_WARP_FORMANT: *lo = -1.0f; *hi = 1.0f; break;
    case WTE_WARP_SPECTRAL_TILT:
      if (key == 0) { *lo = 0.0f; *hi = 2.0f; } else { *lo = 0.0f; *hi = 1.0f; }
      break;
    case WTE_WARP_RING_MOD:
      if (key == 0) { *lo = 0.0f; *hi = 8.0f; } else { *lo = 0.0f; *hi = 1.0f; }
      break;
    case WTE_WARP_ADDITIVE_SHAPE:
      if (key < 8) { *lo = -1.0f; *hi = 1.0f; } else { *lo = 0.0f; *hi = 1.0f; }
      break;
    case WTE_WARP_MATH: *lo = 0.0f; *hi = 1.0f; break;
    default: *lo = 0.0f; *hi = 1.0f; break;
  }
}

// A warp param is "at default" when its quantized value equals the catalog
// default's quantized value (so it need not be transmitted).
inline bool wte_param_is_default(uint8_t type, uint8_t key, float v)
{
  const WteWarpMeta &m = wte_warp_meta(type);
  if (key >= m.param_count) return true;
  float lo, hi;
  wte_param_range(type, key, &lo, &hi);
  return q12_param_quant(v, lo, hi) == q12_param_quant(m.params[key].def, lo, hi);
}

// ---- DECODE (device side) ----------------------------------------------------
// Inflate the compressed payload and decode the compact stream into `doc`.
// `payload`/`payload_len` is the FULL packet (header + compressed body).
// Returns true on success; on any failure (bad magic/version, crc mismatch,
// inflate error, decode overflow) returns false and leaves `doc` unmodified so
// the caller keeps the last good table.
inline bool wte_wire_decode(const uint8_t *payload, size_t payload_len, WTEDoc *doc)
{
  if (payload == nullptr || doc == nullptr) return false;
  if (payload_len < WTE_WIRE_HDR_LEN) return false;
  // ---- header ----
  if (payload[0] != 0x57u || payload[1] != 0x54u || payload[2] != WTE_WIRE_VERSION) return false;
  if (payload[3] != WTE_WIRE_VERSION) return false;
  const uint8_t flags = payload[4];
  const uint32_t crc = (uint32_t)payload[5] | ((uint32_t)payload[6] << 8) |
                       ((uint32_t)payload[7] << 16) | ((uint32_t)payload[8] << 24);
  const uint32_t raw_len = (uint32_t)payload[9] | ((uint32_t)payload[10] << 8);
  if (raw_len == 0u || raw_len > WTE_WIRE_COMPACT_MAX) return false;
  const uint32_t comp_len = (uint32_t)(payload_len - WTE_WIRE_HDR_LEN);
  if (comp_len > WTE_WIRE_COMPRESSED_MAX) return false;
  const uint8_t *body = payload + WTE_WIRE_HDR_LEN;

  // ---- inflate (device side) ----
  uint8_t compact[WTE_WIRE_COMPACT_MAX];
  if (flags & WTE_WIRE_FLAG_DEFLATE)
  {
    size_t out_len = raw_len;
    size_t got = tinfl_decompress_mem_to_mem(compact, out_len, body, comp_len, 0 /* raw deflate */);
    if (got != raw_len) return false;
  }
  else // no-deflate fallback: body is the raw compact stream
  {
    if (comp_len < raw_len) return false;
    for (uint32_t i = 0; i < raw_len; i++) compact[i] = body[i];
  }

  // ---- CRC check (over the compact stream) ----
  if (wte_crc32(compact, raw_len) != crc) return false;

  // ---- decode compact -> WTEDoc ----
  BitReader r;
  r.init(compact, raw_len);
  for (uint32_t f = 0; f < WTE_WAVE_COUNT; f++)
  {
    WTEWaveDoc *w = &doc->waves[f];
    w->combine = (uint8_t)r.get(3);
    w->invert = (uint8_t)r.get(1);
    w->mirror = (uint8_t)r.get(1);
    for (uint32_t s = 0; s < WTE_SOURCES_PER_WAVE; s++)
    {
      WTESourceDoc *src = &w->sources[s];
      src->enabled = (uint8_t)r.get(1);
      if (src->enabled)
      {
        src->type = (uint8_t)r.get(5);
        src->harmonic = q8_harmonic_dequant(r.get(8));
        src->phase = q8_phase_dequant(r.get(8));
        src->gain = q8_gain_dequant(r.get(8));
        src->bias = q8_bias_dequant(r.get(8));
      }
      else
      {
        src->type = 0; src->harmonic = 1.0f; src->phase = 0.0f; src->gain = 1.0f; src->bias = 0.0f;
      }
      uint32_t pc = r.get(3);
      if (pc > WTE_MAX_WARPS_PER_STACK) pc = WTE_MAX_WARPS_PER_STACK;
      w->psr_cnt[s] = (uint8_t)pc;
      for (uint32_t k = 0; k < pc; k++)
      {
        WTEWarpDoc *wr = &w->psr[s][k];
        wr->type = (uint8_t)r.get(4);
        wr->enabled = (uint8_t)r.get(1);
        uint32_t mask = r.get(9);
        wr->param_count = WTE_MAX_WARP_PARAMS;
        const WteWarpMeta &m = wte_warp_meta(wr->type);
        for (uint32_t p = 0; p < WTE_MAX_WARP_PARAMS; p++)
        {
          if (p < m.param_count && (mask & (1u << p)))
          {
            float lo, hi;
            wte_param_range(wr->type, (uint8_t)p, &lo, &hi);
            wr->params[p] = q12_param_dequant(r.get(12), lo, hi);
          }
          else
          {
            wr->params[p] = (p < m.param_count) ? m.params[p].def : 0.0f;
          }
        }
      }
    }
    uint32_t oc = r.get(4);
    if (oc > WTE_MAX_WARPS_PER_STACK) oc = WTE_MAX_WARPS_PER_STACK;
    w->out_cnt = (uint8_t)oc;
    for (uint32_t k = 0; k < oc; k++)
    {
      WTEWarpDoc *wr = &w->out[k];
      wr->type = (uint8_t)r.get(4);
      wr->enabled = (uint8_t)r.get(1);
      uint32_t mask = r.get(9);
      wr->param_count = WTE_MAX_WARP_PARAMS;
      const WteWarpMeta &m = wte_warp_meta(wr->type);
      for (uint32_t p = 0; p < WTE_MAX_WARP_PARAMS; p++)
      {
        if (p < m.param_count && (mask & (1u << p)))
        {
          float lo, hi;
          wte_param_range(wr->type, (uint8_t)p, &lo, &hi);
          wr->params[p] = q12_param_dequant(r.get(12), lo, hi);
        }
        else
        {
          wr->params[p] = (p < m.param_count) ? m.params[p].def : 0.0f;
        }
      }
    }
    if (!r.ok) return false;
  }
  return true;
}

// ---- ENCODE (reference / web-mirror) -----------------------------------------
// Produce the compact stream for `doc` into `compact` (>= WTE_WIRE_COMPACT_MAX
// bytes). Returns the compact length, or 0 on overflow. Canonical layout spec;
// the web encoder (wte_compress.ts) must match it byte-for-byte (verified by
// the round-trip harness).
inline uint32_t wte_wire_encode(const WTEDoc *doc, uint8_t *compact, uint32_t compact_cap)
{
  BitWriter w;
  w.init(compact, compact_cap);
  for (uint32_t f = 0; f < WTE_WAVE_COUNT; f++)
  {
    const WTEWaveDoc &wd = doc->waves[f];
    w.put(wd.combine, 3);
    w.put(wd.invert, 1);
    w.put(wd.mirror, 1);
    for (uint32_t s = 0; s < WTE_SOURCES_PER_WAVE; s++)
    {
      const WTESourceDoc &src = wd.sources[s];
      w.put(src.enabled ? 1u : 0u, 1);
      if (src.enabled)
      {
        w.put(src.type, 5);
        w.put(q8_harmonic_quant(src.harmonic), 8);
        w.put(q8_phase_quant(src.phase), 8);
        w.put(q8_gain_quant(src.gain), 8);
        w.put(q8_bias_quant(src.bias), 8);
      }
      uint32_t pc = wd.psr_cnt[s];
      if (pc > WTE_MAX_WARPS_PER_STACK) pc = WTE_MAX_WARPS_PER_STACK;
      w.put(pc, 3);
      for (uint32_t k = 0; k < pc; k++)
      {
        const WTEWarpDoc &wr = wd.psr[s][k];
        w.put(wr.type, 4);
        w.put(wr.enabled ? 1u : 0u, 1);
        uint32_t mask = 0;
        const WteWarpMeta &m = wte_warp_meta(wr.type);
        for (uint32_t p = 0; p < m.param_count && p < WTE_MAX_WARP_PARAMS; p++)
          if (!wte_param_is_default(wr.type, (uint8_t)p, wr.params[p]))
            mask |= (1u << p);
        w.put(mask, 9);
        for (uint32_t p = 0; p < m.param_count && p < WTE_MAX_WARP_PARAMS; p++)
          if (mask & (1u << p))
          {
            float lo, hi;
            wte_param_range(wr.type, (uint8_t)p, &lo, &hi);
            w.put(q12_param_quant(wr.params[p], lo, hi), 12);
          }
      }
    }
    uint32_t oc = wd.out_cnt;
    if (oc > WTE_MAX_WARPS_PER_STACK) oc = WTE_MAX_WARPS_PER_STACK;
    w.put(oc, 4);
    for (uint32_t k = 0; k < oc; k++)
    {
      const WTEWarpDoc &wr = wd.out[k];
      w.put(wr.type, 4);
      w.put(wr.enabled ? 1u : 0u, 1);
      uint32_t mask = 0;
      const WteWarpMeta &m = wte_warp_meta(wr.type);
      for (uint32_t p = 0; p < m.param_count && p < WTE_MAX_WARP_PARAMS; p++)
        if (!wte_param_is_default(wr.type, (uint8_t)p, wr.params[p]))
          mask |= (1u << p);
      w.put(mask, 9);
      for (uint32_t p = 0; p < m.param_count && p < WTE_MAX_WARP_PARAMS; p++)
        if (mask & (1u << p))
        {
          float lo, hi;
          wte_param_range(wr.type, (uint8_t)p, &lo, &hi);
          w.put(q12_param_quant(wr.params[p], lo, hi), 12);
        }
    }
  }
  if (w.overflow) return 0;
  return w.byte_len();
}

// ---- Packet framing ------------------------------------------------------------
// Write the 11-byte header before a (deflate'd) compact stream. `out` must have
// room for WTE_WIRE_HDR_LEN bytes; the caller fills out[11..] with the deflate
// output. raw_len/crc are set here from the compact stream.
inline void wte_wire_write_header(uint8_t *out, const uint8_t *compact, uint32_t compact_len, bool deflate)
{
  out[0] = 0x57u; out[1] = 0x54u; out[2] = WTE_WIRE_VERSION;
  out[3] = WTE_WIRE_VERSION;
  out[4] = deflate ? (uint8_t)WTE_WIRE_FLAG_DEFLATE : 0u;
  const uint32_t crc = wte_crc32(compact, compact_len);
  out[5] = (uint8_t)(crc & 0xFFu);
  out[6] = (uint8_t)((crc >> 8) & 0xFFu);
  out[7] = (uint8_t)((crc >> 16) & 0xFFu);
  out[8] = (uint8_t)((crc >> 24) & 0xFFu);
  out[9] = (uint8_t)(compact_len & 0xFFu);
  out[10] = (uint8_t)((compact_len >> 8) & 0xFFu);
}
} // namespace wte_wire