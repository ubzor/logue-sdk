#pragma once
#include <cstdint>
/*
 *  File: osc_wte_doc.h
 *
 *  Wave-Table-Editor document — the C++ data model of the web editor at
 *  /editor (websim/app/src/lib/editor/types.ts).
 *
 *  A document is 16 WAVEs (morph frames). Each wave is a RECIPE that turns
 *  up to four sources into a single waveform through a combine step and an
 *  ordered warp stack:
 *
 *      for i in 0..3 (enabled sources):
 *          s[i](t) = shape(t; harmonic, phase, gain, bias)   // per-source shape
 *                   -> perSourceWarp[i]                      // per-source warp stack
 *      combined(t) = combine(s0..s3)                         // one of 6 methods
 *                   -> modifiers: mirror (t -> 1-t), invert (v -> -v)
 *                   -> outputWarp                            // output warp stack
 *
 *  This header carries ONLY the document structures + the metadata catalogs
 *  (30 source shapes, 15 warp types with their parameter specs) that the
 *  renderer (osc_wte_render.h) and the generator (osc_wte.h) consume. It is
 *  the C++ mirror of the TypeScript model — keep it in sync with
 *  docs/editor-model.md and src/lib/editor/types.ts.
 *
 *  The warp "Math" expression parameter is DROPPED in the C++ port: the TS
 *  renderer treats Math as a passthrough (TODO there), so the C++ warp
 *  "Math" is a plain no-op — only its type/enabled flags are stored.
 */

// ---- Catalog constants (mirror types.ts) -----------------------------------

// Number of waves (morph frames) in the document.
static constexpr uint8_t WTE_WAVE_COUNT = 16U;
// Number of source slots per wave.
static constexpr uint8_t WTE_SOURCES_PER_WAVE = 4U;
// The 30 usable source shapes (index == type id, order matches types.ts).
static constexpr uint8_t WTE_SOURCE_COUNT = 30U;
// The 15 warp types (index == type id, order matches types.ts).
static constexpr uint8_t WTE_WARP_COUNT = 15U;
// The 6 combine methods.
static constexpr uint8_t WTE_COMBINE_COUNT = 6U;
// Maximum warps stored per stack. The TS model has no hard cap, but the C++
// document is a fixed-size struct (no allocation, table + doc in RAM):
// stacks longer than this are truncated on import (the editor UI in practice
// never builds more than a handful).
static constexpr uint8_t WTE_MAX_WARPS_PER_STACK = 8U;
// Maximum parameters per warp (Additive Shape has 9: '1'..'8' + 'Mix').
static constexpr uint8_t WTE_MAX_WARP_PARAMS = 9U;

// Combine methods (0..5).
enum : uint8_t
{
  WTE_COMBINE_SUM = 0U,
  WTE_COMBINE_MULTIPLY,
  WTE_COMBINE_MIN,
  WTE_COMBINE_MAX,
  WTE_COMBINE_AM,
  WTE_COMBINE_WRAP,
};

// Warp types (0..14), in the SAME order as WARP_TYPES in types.ts.
enum : uint8_t
{
  WTE_WARP_WAVEFOLD = 0U,
  WTE_WARP_HARD_SYNC,
  WTE_WARP_FM,
  WTE_WARP_WAVESHAPE,
  WTE_WARP_BITCRUSH,
  WTE_WARP_EXPONENT,
  WTE_WARP_CZ_RESO,
  WTE_WARP_BEND,
  WTE_WARP_PWM,
  WTE_WARP_ASYM,
  WTE_WARP_FORMANT,
  WTE_WARP_SPECTRAL_TILT,
  WTE_WARP_RING_MOD,
  WTE_WARP_ADDITIVE_SHAPE,
  WTE_WARP_MATH,
};

// Waveshape modes (WTE_WARP_WAVESHAPE param 'Mode'): 0..3.
enum : uint8_t
{
  WTE_WAVESHAPE_TAHN = 0U,
  WTE_WAVESHAPE_ATAN,
  WTE_WAVESHAPE_CUBIC,
  WTE_WAVESHAPE_SIN,
};

// CZ Reso windows (WTE_WARP_CZ_RESO param 'Window'): 0..2.
enum : uint8_t
{
  WTE_CZ_WINDOW_SAW = 0U,
  WTE_CZ_WINDOW_TRI,
  WTE_CZ_WINDOW_TRAP,
};

// ---- Parameter specs ---------------------------------------------------------

// Declares one warp parameter: its key (index into WarpItem::params), kind
// ('number' / 'mode' / 'window') and its default. Numeric params are stored
// as raw float in WarpItem::params[0..8]. Enumerated params (Waveshape Mode,
// CZ Reso Window) store the enum value (0..3 / 0..2) in the same slot.
struct WteParamSpec
{
  float def;      // default value
  uint8_t kind;   // 0 = number, 1 = mode, 2 = window
};

// One warp-type catalog entry: its parameter count and specs, in the SAME
// key order the TypeScript WARP_META lists them. The renderer reads params
// positionally (p[0], p[1], ...) in this order.
struct WteWarpMeta
{
  uint8_t param_count;
  WteParamSpec params[WTE_MAX_WARP_PARAMS];
};

// Full catalog of the 15 warps with their parameter specs + defaults.
// Keep in sync with WARP_META in src/lib/editor/types.ts.
inline const WteWarpMeta &wte_warp_meta(uint8_t type)
{
  static const WteWarpMeta kMeta[WTE_WARP_COUNT] = {
      // Wavefold: Gain (0-4, 1), Bias (-1..1, 0)
      {2, {{1.f, 0}, {0.f, 0}}},
      // Hard Sync: Ratio (0-4, 1)
      {1, {{1.f, 0}}},
      // FM: Ratio (0-8, 1), Index (0-4, 0.5)
      {2, {{1.f, 0}, {0.5f, 0}}},
      // Waveshape: Mode (Tahn, enum), Drive (0-4, 1)
      {2, {{0.f, 1}, {1.f, 0}}},
      // Bitcrush: Bits (1-16, 8), Rate (0-1, 1)
      {2, {{8.f, 0}, {1.f, 0}}},
      // Exponent: Power (0.1-8, 1)
      {1, {{1.f, 0}}},
      // CZ Reso: Reso (0-1, 0.5), Window (Saw, enum)
      {2, {{0.5f, 0}, {0.f, 2}}},
      // Bend: Amount (-1..1, 0)
      {1, {{0.f, 0}}},
      // PWM: Width (0-1, 0.5)
      {1, {{0.5f, 0}}},
      // Asym: Amount (-1..1, 0)
      {1, {{0.f, 0}}},
      // Formant: Shift (-1..1, 0)
      {1, {{0.f, 0}}},
      // Spectral Tilt: Tilt (0-2, 0.5), Mix (0-1, 1)
      {2, {{0.5f, 0}, {1.f, 0}}},
      // Ring Mod: Freq (0-8, 1), Depth (0-1, 1)
      {2, {{1.f, 0}, {1.f, 0}}},
      // Additive Shape: 1..8 (-1..1, 0 each), Mix (0-1, 0.5)
      {9, {{0.f, 0}, {0.f, 0}, {0.f, 0}, {0.f, 0}, {0.f, 0}, {0.f, 0},
           {0.f, 0}, {0.f, 0}, {0.5f, 0}}},
      // Math: (no numeric params in the C++ port — passthrough no-op)
      {0, {{}}},
  };
  return kMeta[(type < WTE_WARP_COUNT) ? type : 0U];
}

// Build the default params for a warp type from the catalog (mirror of
// defaultWarpParams in types.ts).
inline void wte_default_warp_params(uint8_t type, float *out, uint8_t n)
{
  const WteWarpMeta &m = wte_warp_meta(type);
  const uint8_t cnt = (n < m.param_count) ? n : m.param_count;
  for (uint8_t i = 0; i < cnt; i++)
    out[i] = m.params[i].def;
  for (uint8_t i = cnt; i < n; i++)
    out[i] = 0.f;
}

// ---- Document structures -----------------------------------------------------

// One source within a wave. Always present (4 slots per wave);
// enabled == 0 means "this slot is empty" (the renderer skips it).
struct WTESourceDoc
{
  uint8_t enabled;  // 0/1
  uint8_t type;     // 0..29 (WTE source shape)
  float harmonic;   // 1..16 (dominant partial / repetition factor)
  float phase;      // 0..1 (phase offset, fraction of a period)
  float gain;       // 0..2 (amplitude multiplier)
  float bias;       // -1..+1 (vertical DC offset)
};

// A warp instance. Order in the stack = order of application (top to
// bottom, like the TS WarpItem list). params holds at most
// WTE_MAX_WARP_PARAMS floats in wte_warp_meta(type) key order; enum params
// (Waveshape Mode / CZ Reso Window) store their enum value in the same slot.
struct WTEWarpDoc
{
  uint8_t enabled;   // 0/1
  uint8_t type;      // 0..14 (WTE_WARP_*)
  uint8_t param_count; // valid params (0..WTE_MAX_WARP_PARAMS)
  float params[WTE_MAX_WARP_PARAMS];
};

// One wave = one morph frame of the wavetable (a full recipe).
struct WTEWaveDoc
{
  WTESourceDoc sources[WTE_SOURCES_PER_WAVE];
  uint8_t combine;    // 0..5 (WTE_COMBINE_*)
  uint8_t invert;     // 0/1: flip the combined signal vertically
  uint8_t mirror;     // 0/1: mirror the combined signal horizontally (t -> 1-t)
  WTEWarpDoc psr[WTE_SOURCES_PER_WAVE][WTE_MAX_WARPS_PER_STACK]; // per-source warp stacks
  uint8_t psr_cnt[WTE_SOURCES_PER_WAVE];                        // warp count per source (0..8)
  WTEWarpDoc out[WTE_MAX_WARPS_PER_STACK];                      // output warp stack
  uint8_t out_cnt;                                              // output warp count (0..8)
};

// The full editor document: 16 waves.
struct WTEDoc
{
  WTEWaveDoc waves[WTE_WAVE_COUNT];
};

// ---- Factories (defaults) -----------------------------------------------------

// A wave with a single enabled sine source (harmonic = index+1 so the 16
// default waves read as a closed->open harmonic morph), Sum combine, no
// modifiers, and empty warp stacks (mirror of createDefaultWave in
// store.svelte.ts).
inline void wte_default_wave(uint8_t index, WTEWaveDoc *w)
{
  for (uint8_t s = 0; s < WTE_SOURCES_PER_WAVE; s++)
  {
    WTESourceDoc *src = &w->sources[s];
    src->enabled = (s == 0U) ? 1U : 0U;
    src->type = 0U; // Sine
    src->harmonic = (s == 0U) ? static_cast<float>(index + 1U) : 1.f;
    src->phase = 0.f;
    src->gain = 1.f;
    src->bias = 0.f;
  }
  w->combine = WTE_COMBINE_SUM;
  w->invert = 0U;
  w->mirror = 0U;
  for (uint8_t s = 0; s < WTE_SOURCES_PER_WAVE; s++)
    w->psr_cnt[s] = 0U;
  w->out_cnt = 0U;
}

// All 16 default waves (mirror of createDefaultWaves).
inline void wte_default_doc(WTEDoc *doc)
{
  for (uint8_t i = 0; i < WTE_WAVE_COUNT; i++)
    wte_default_wave(i, &doc->waves[i]);
}