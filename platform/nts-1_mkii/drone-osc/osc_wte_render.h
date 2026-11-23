#pragma once
/*
 *  File: osc_wte_render.h
 *
 *  C++ port of the web editor's wave renderer (websim/app/src/lib/wavetable/
 *  render.ts). Turns ONE WTEWaveDoc into a row of `points` samples in
 *  [-1, +1]. This is the 1:1 copy of the /editor mechanism: same 30 source
 *  shapes, same 15 warp types, same combine methods, same modifiers, same
 *  signal flow (source shape -> per-source warp stack -> combine -> invert ->
 *  output warp stack -> clip -> mirror), same Noise seeding (mulberry32).
 *
 *  Deterministic pure math; called from WteTable::regen() at K-rate ONLY
 *  (16 waves x 256 points x sources/warps ~= a few M flops — sub-millisecond
 *  on the STM32H725 hard FPU, and trivial in the web sim). NO libm in this
 *  file except the fast helpers from utils/float_math.h (osc_sinf,
 *  fasttanh2f, fasterpowf) — the same family the rest of the unit uses.
 *
 *  NOTE: the TS "Math" warp is a passthrough (TODO in render.ts) — the C++
 *  port keeps it a passthrough for 1:1 parity. The TS "CZ Reso" is a
 *  stateless approximation (v * (1 - reso*0.5)) — the C++ port keeps the
 *  SAME stateless approximation (window param stored but unused, exactly as
 *  in TS).
 */

#include "utils/float_math.h" // osc_sinf, fasttanf, fasterpowf, fastertanh2f
#include "osc_wte_doc.h"

#include <cstdint> // uint8_t, uint32_t, uint64_t, int16_t, int32_t

namespace wte
{

// ---- small helpers (mirror render.ts) -------------------------------------

// frac(x) = x - floor(x), for any finite x (positive inputs in practice).
  inline float fracf(float x)
  {
    const float i = static_cast<float>(static_cast<int32_t>(x));
    return x - i;
  }

  inline float clipf(float x, float lo, float hi)
  {
    return (x < lo) ? lo : (x > hi) ? hi : x;
  }

// tri(t) = 1 - 4*|frac(t) - 0.5|  (unit triangle, peak +1 at frac = 0.5)
  inline float trif(float t)
  {
    const float u = fracf(t) - 0.5f;
    const float au = (u < 0.f) ? -u : u;
    return 1.f - 4.f * au;
  }

// exp(-x) for x >= 0, via a 4th-order Taylor series in e^(-x). The values we
// actually need are x in [0, ~2.5] (the Wave Pkt / Two Bumps Gaussian widths
// at t in [0,1]); the series is accurate to <1e-5 relative over that range —
// far below audibility for a table regenerated at K-rate. No libm expf (it
// would be an unresolved dynamic symbol on the device; fasterexpf is a
// bit-trick that is poor for small |x|).
  inline float exp_neg_pos(float x)
  {
    const float x2 = x * x;
    const float e = 1.f - x + x2 * 0.5f - x2 * x / 6.f + x2 * x2 / 24.f;
    return (e < 1e-6f) ? 1e-6f : e;
  }

// atan approximation (Abramowitz-Stegun 4.4.33 / classic rational fit):
//   atan(x) ~= x * (1351 + 54*x^2) / (1459 + 8*x^2),  |x| <= ~1
// accurate to ~1e-4 over [-1,1]. For |x| > 1 we use atan(x) = pi/2 - atan(1/x)
// (the S-Curve / Smooth Sq inputs are tan(pi*(u-0.5)), which DIVERGES at the
// period edges — the TS version produces +/-inf there and atan(inf) = pi/2;
// we clamp the argument to +/-1e3 so the rational fit stays in range and the
// result is pi/2 - atan(1e-3) ~= pi/2 (the same limit)). This is K-rate-only
// (256 x 16 table regen), so even a slightly off curve is inaudible.
  inline float fastatanf(float x)
  {
    const float kPiOver2 = 1.5707963267948966f;
    if (x > 1000.f) return kPiOver2;
    if (x < -1000.f) return -kPiOver2;
    if (x > 1.f) return kPiOver2 - fastatanf(1.f / x);
    if (x < -1.f) return -kPiOver2 - fastatanf(1.f / x);
    const float x2 = x * x;
    return x * (1351.f + 54.f * x2) / (1459.f + 8.f * x2);
  }

// Deterministic 32-bit PRNG — the EXACT mulberry32 (bit-identical to the JS
// version in render.ts). 32-bit imul is done with 64-bit intermediates.
  inline float mulberry32f(uint32_t seed)
  {
    uint32_t a = seed;
    a = (a + 0x6D2B79F5U);
    uint32_t t = (a ^ (a >> 15));
    t = static_cast<uint32_t>(static_cast<uint64_t>(t) * static_cast<uint64_t>(a | 1U));
    t = (t + static_cast<uint32_t>(static_cast<uint64_t>(t ^ (t >> 7)) * 61U)) ^ t;
    t = (t ^ (t >> 14));
    return static_cast<float>(t) / 4294967296.f;
  }

// ---- The 30 source shapes (1:1 of SOURCE_FNS in render.ts) ------------------
// v(t, h, p) where t in [0,1), h = harmonic (1..16), p = phase (0..1).
// Index 0 = Sine ... 29 = Ring Sine (order == the SOURCE_TYPES list).
  inline float source_shape(uint8_t type, float t, float h, float p)
  {
    const float x = h * t + p; // the common phase argument
    switch (type)
    {
      case 0:  // Sine
        return osc_sinf(6.283185307179586f * x);
      case 1:  // Triangle
        return trif(x);
      case 2:  // Saw
        return 2.f * fracf(x) - 1.f;
      case 3:  // Square
        return (osc_sinf(6.283185307179586f * x) >= 0.f) ? 1.f : -1.f;
      case 4:  // Pulse
        return (fracf(x) < 0.5f) ? 1.f : -1.f;
      case 5:  // Half Sine
        {
          const float v = osc_sinf(6.283185307179586f * x);
          return (v > 0.f) ? v - 0.5f : -0.5f;
        }
      case 6:  // Parabola
        {
          const float u = fracf(x) - 0.5f;
          return 1.f - 8.f * u * u;
        }
      case 7:  // Bell
        {
          const float v = 2.f * fracf(x) - 1.f;
          const float av = (v < 0.f) ? -v : v;
          return 1.f - av * av;
        }
      case 8:  // S-Curve
        {
          const float u = fracf(x);
          const float c = 6.283185307179586f * (u - 0.5f);
          return (2.f / 3.141592653589793f) * fastatanf(c);
        }
      case 9:  // Trapezoid
        {
          const float u = fracf(x);
          if (u < 0.25f) return u * 4.f - 1.f;
          if (u < 0.75f) return 1.f;
          return 2.f - u * 4.f;
        }
      case 10: // Stairs
        return (2.f * fracf(4.f * fracf(x)) - 1.f) * 0.5f;
      case 11: // Sine²
        {
          const float v = osc_sinf(6.283185307179586f * x);
          return v * v - 0.5f;
        }
      case 12: // FM Sine
        return osc_sinf(6.283185307179586f * x + 0.5f * osc_sinf(2.f * 6.283185307179586f * x));
      case 13: // Sinc
        {
          const float v = 2.f * x - 1.f;
          if (v > -1e-6f && v < 1e-6f) return 1.f;
          return osc_sinf(3.141592653589793f * v) / (3.141592653589793f * v);
        }
      case 14: // Chirp
        return osc_sinf(6.283185307179586f * (x + 0.5f * x * x));
      case 15: // Wave Pkt
        {
          const float d = t - 0.5f;
          return exp_neg_pos(8.f * d * d) * osc_sinf(6.283185307179586f * x);
        }
      case 16: // Smooth Sq
        {
          const float u = fracf(x);
          const float c = 3.141592653589793f * (u - 0.5f);
          return (2.f / 3.141592653589793f) * fastatanf(8.f * fasttanfullf(c));
        }
      case 17: // Sharkfin
        {
          const float u = fracf(x);
          if (u < 0.5f)
            return 0.5f * fasterpowf(u * 2.f, 0.3f) - 0.5f;
          return 0.5f * fasterpowf((1.f - u) * 2.f, 3.f) - 0.5f;
        }
      case 18: // Spike
        {
          const float u = fracf(x);
          const float v = 2.f * u - 1.f;
          const float av = (v < 0.f) ? -v : v;
          return 1.f - fasterpowf(av, 8.f);
        }
      case 19: // Noise (overridden in render_wave with a seeded version)
        return 0.f;
      case 20: // Cubed
        {
          const float v = osc_sinf(6.283185307179586f * x);
          const float av = (v < 0.f) ? -v : v;
          return ((v < 0.f) ? -1.f : 1.f) * fasterpowf(av, 3.f);
        }
      case 21: // Two Bumps
        {
          const float u = fracf(x);
          return exp_neg_pos(32.f * (u - 0.25f) * (u - 0.25f)) +
                 exp_neg_pos(32.f * (u - 0.75f) * (u - 0.75f)) - 0.5f;
        }
      case 22: // Folded
        {
          const float u = fracf(x);
          const float v = 2.f * u - 1.f;
          const float av = (v < 0.f) ? -v : v;
          return 1.f - av;
        }
      case 23: // Asym Tri
        {
          const float u = fracf(x);
          if (u < 0.3f) return (u / 0.3f) * 2.f - 1.f;
          return ((0.7f - u) / 0.7f) * 2.f - 1.f;
        }
      case 24: // Pinch
        {
          const float v = osc_sinf(6.283185307179586f * x);
          const float av = (v < 0.f) ? -v : v;
          return ((v < 0.f) ? -1.f : 1.f) * fasterpowf(av, 0.5f);
        }
      case 25: // PD Sine
        {
          const float v = osc_sinf(6.283185307179586f * x);
          return osc_sinf(6.283185307179586f * x + 0.3f * v);
        }
      case 26: // Organ
        return (osc_sinf(6.283185307179586f * x) +
                0.5f * osc_sinf(2.f * 6.283185307179586f * x) +
                0.33f * osc_sinf(3.f * 6.283185307179586f * x)) / 1.83f;
      case 27: // Sync Saw
        return (2.f * fracf(2.f * x) - 1.f) - (2.f * fracf(x) - 1.f);
      case 28: // Bit Crush
        {
          const float v = osc_sinf(6.283185307179586f * x);
          return (float)(int32_t)(v * 8.f + (v < 0.f ? -0.5f : 0.5f)) * 0.125f;
        }
      case 29: // Ring Sine
        return osc_sinf(6.283185307179586f * x) * osc_sinf(3.f * 6.283185307179586f * x);
      default:
        return 0.f;
    }
  }

// ---- The 15 warp types (1:1 of WARP_FNS in render.ts) -----------------------
// warp(t, v, item) -> new value. Params are read positionally in the key order
// of wte_warp_meta(type): e.g. Wavefold p[0]=Gain, p[1]=Bias. Enum params
// (Waveshape Mode, CZ Reso Window) store their enum value in the same slot.
  inline float apply_warp(uint8_t type, float t, float v, const WTEWarpDoc &w)
  {
    const float *p = w.params;
    switch (type)
    {
      case WTE_WARP_WAVEFOLD:
      // 1 - |2*frac((v*Gain + Bias + 1)/2) - 1|
        {
          const float g = p[0];
          const float b = p[1];
          return 1.f - (2.f * fracf((v * g + b + 1.f) * 0.5f) - 1.f);
        }
      case WTE_WARP_HARD_SYNC:
      // sin(2*pi*Ratio*t)  (re-times the phase; value v is unused in TS)
        return osc_sinf(6.283185307179586f * p[0] * t);
      case WTE_WARP_FM:
      // sin(2*pi*(t + Index*sin(2*pi*Ratio*t)))
        return osc_sinf(6.283185307179586f * (t + p[1] * osc_sinf(6.283185307179586f * p[0] * t)));
      case WTE_WARP_WAVESHAPE:
        {
          const uint8_t mode = (uint8_t)(p[0] + 0.5f);
          const float drive = p[1];
          switch (mode)
          {
            case WTE_WAVESHAPE_TAHN:  return fastertanh2f(drive * v);
            case WTE_WAVESHAPE_ATAN:  return (2.f / 3.141592653589793f) * fastatanf(drive * v);
            case WTE_WAVESHAPE_CUBIC: return v * (1.f - (v * v) / 3.f);
            case WTE_WAVESHAPE_SIN:   return osc_sinf(1.5707963267948966f * v);
            default:                  return v;
          }
        }
      case WTE_WARP_BITCRUSH:
        {
          int32_t bits = (int32_t)(p[0] + 0.5f);
          if (bits < 1) bits = 1;
          if (bits > 16) bits = 16;
          const float levels = (float)(1 << (bits - 1));
          const float q = v * levels;
          return (float)(int32_t)(q + (q < 0.f ? -0.5f : 0.5f)) / levels;
        }
      case WTE_WARP_EXPONENT:
        {
          const float av = (v < 0.f) ? -v : v;
          return ((v < 0.f) ? -1.f : 1.f) * fasterpowf(av, p[0]);
        }
      case WTE_WARP_CZ_RESO:
      // TS is a STATELESS approximation: v * (1 - reso*0.5) (window unused).
      // Keep 1:1 (a real one-pole stateful filter would drift across the 256
      // sample sweep and break the deterministic table).
        return v * (1.f - p[0] * 0.5f);
      case WTE_WARP_BEND:
      // sin(2*pi*(t + Amount*sin(2*pi*t)*0.25))
        return osc_sinf(6.283185307179586f * (t + p[0] * osc_sinf(6.283185307179586f * t) * 0.25f));
      case WTE_WARP_PWM:
      // frac(t) < Width ? v : -v
        return (fracf(t) < p[0]) ? v : -v;
      case WTE_WARP_ASYM:
        {
          const float a = p[0];
          const float scaled = (v > 0.f) ? v * (1.f + a) : v * (1.f - a);
          const float m = (1.f + a > 1.f - a) ? (1.f + a) : (1.f - a);
          return scaled / (m < 1e-3f ? 1e-3f : m);
        }
      case WTE_WARP_FORMANT:
        return v * (1.f + p[0] * 0.5f);
      case WTE_WARP_SPECTRAL_TILT:
        return v * (1.f - p[1]) + v * (1.f - p[0] * 0.5f) * p[1];
      case WTE_WARP_RING_MOD:
        return v * (1.f - p[1] + p[1] * osc_sinf(6.283185307179586f * p[0] * t));
      case WTE_WARP_ADDITIVE_SHAPE:
        {
          float additive = 0.f;
          for (uint8_t k = 0; k < 8; k++)
            additive += p[k] * osc_sinf(6.283185307179586f * (float)(k + 1) * t);
          return v * (1.f - p[8]) + (v + additive) * p[8];
        }
      case WTE_WARP_MATH:
      // TS TODO: passthrough (no tokenizer in render.ts).
        return v;
      default:
        return v;
    }
  }

// ---- combine (1:1 of combineSources in render.ts) ---------------------------
// Disabled source slots are already filtered out by the caller (they do not
// reach this function): `values` holds only the ENABLED sources (0..4 items).
  inline float combine_sources(const float *values, uint8_t count, uint8_t mode)
  {
    if (count == 0) return 0.f;
    if (count == 1) return values[0];
    switch (mode)
    {
      case WTE_COMBINE_SUM:
      {
        float s = 0.f;
        for (uint8_t i = 0; i < count; i++) s += values[i];
        return s;
      }
      case WTE_COMBINE_MULTIPLY:
      {
        float m = 1.f;
        for (uint8_t i = 0; i < count; i++) m *= values[i];
        return m;
      }
      case WTE_COMBINE_MIN:
      {
        float m = values[0];
        for (uint8_t i = 1; i < count; i++) if (values[i] < m) m = values[i];
        return m;
      }
      case WTE_COMBINE_MAX:
      {
        float m = values[0];
        for (uint8_t i = 1; i < count; i++) if (values[i] > m) m = values[i];
        return m;
      }
      case WTE_COMBINE_AM:
      // s0 * (0.5 + 0.5*s1)
        return (count >= 2) ? (values[0] * (0.5f + 0.5f * values[1])) : values[0];
      case WTE_COMBINE_WRAP:
      {
        float s = 0.f;
        for (uint8_t i = 0; i < count; i++) s += values[i];
        return 2.f * fracf(s) - 1.f;
      }
      default:
        return values[0];
    }
  }

// ---- Main render (1:1 of renderWave in render.ts) ---------------------------
// Render ONE wave (WTEWaveDoc) into `row` of `points` samples in [-1, +1].
// frameIndex is the wave's index in the document (0..15) — it only feeds the
// Noise source's deterministic seed (seed = frameIndex*7919 + harmonic*131 +
// floor(t*256), exactly as in TS), so the Noise shape is stable per frame.
  inline void render_wave(const WTEWaveDoc &w, float *row, uint32_t points, uint32_t frameIndex)
  {
    for (uint32_t i = 0; i < points; i++)
    {
      const float t = (float)i / (float)(points - 1U);
      float srcVals[WTE_SOURCES_PER_WAVE];
      uint8_t n = 0;
      for (uint8_t s = 0; s < WTE_SOURCES_PER_WAVE; s++)
      {
        const WTESourceDoc &src = w.sources[s];
        if (!src.enabled) continue;
        float v;
        if (src.type == 19) // Noise (seeded, deterministic per frame/point)
        {
          const uint32_t seed = (frameIndex * 7919U) +
                                (static_cast<uint32_t>(src.harmonic + 0.5f) * 131U) +
                                static_cast<uint32_t>(t * 256.f);
          v = mulberry32f(seed) * 2.f - 1.f;
        }
        else
        {
          v = source_shape(src.type, t, src.harmonic, src.phase);
        }
        v = v * src.gain + src.bias;
        for (uint8_t k = 0; k < w.psr_cnt[s]; k++)
        {
          const WTEWarpDoc &wr = w.psr[s][k];
          if (!wr.enabled) continue;
          v = apply_warp(wr.type, t, v, wr);
        }
        srcVals[n++] = v;
      }
      float out = combine_sources(srcVals, n, w.combine);
      if (w.invert) out = -out;
      for (uint8_t k = 0; k < w.out_cnt; k++)
      {
        const WTEWarpDoc &wr = w.out[k];
        if (!wr.enabled) continue;
        out = apply_warp(wr.type, t, out, wr);
      }
      row[i] = clipf(out, -1.f, 1.f);
    }
  // Mirror: horizontal flip (t -> 1 - t), applied LAST (after the clip),
  // exactly as in render.ts.
    if (w.mirror)
    {
      for (uint32_t i = 0; i < points / 2U; i++)
      {
        const float tmp = row[i];
        row[i] = row[points - 1U - i];
        row[points - 1U - i] = tmp;
      }
    }
  }


} // namespace wte
