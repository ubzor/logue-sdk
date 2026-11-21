#pragma once
/*
 *  File: osc_procedural.h
 *
 *  Procedural wavetable generator + lookup for the DRONE unit.
 *
 *  A SINGLE procedurally-generated wavetable bank (8 morph frames x 256
 *  points, 16-bit fixed point) is a NEW parallel voice that feeds the
 *  common post-stage (FOLD/SATU/SVF/formant/comb) alongside the A+B main
 *  voice (see the `mix` line in osc_mix.h).
 *
 *  SPECTRUM SURFACE
 *  ----------------
 *  The 8 frames are 8 slices of ONE continuous spectral surface:
 *
 *      A[n](m) = env(n, DENS) * n^(-DECAY(m)) * (1 + SKEW*cos(pi*n))
 *                * (1 + ORG * rnd(n, seed(m)))
 *
 *  where n = 1..kPartials (partials), m = 0..7 (frame). Each frame is
 *  rendered to 256 samples by direct additive synthesis (a sum of 16
 *  sines, one per partial). The morph position (PMORPH) is a fractional
 *  frame index in [0, 7]: the audio path crossfades the two adjacent
 *  frames (bilinear: frame x phase), exactly like WaveTable::resolve_wave
 *  does for the Korg banks.
 *
 *  The surface is "closed -> open": frame 0 emphasizes the LOW partials
 *  (a warm, closed body), frame 7 the HIGH partials (an open, dense
 *  body). DECAY (PDECAY) sets the spectral tilt n^(-p) for ALL frames;
 *  the per-frame decay offset makes the morph sweep the tilt, so PMORPH
 *  alone audibly moves closed <-> open.
 *
 *  PARAMETERS (all applied K-rate in blockPrepass when a dirty flag is
 *  set; see Osc::procRegen / osc_mix.h):
 *      PMORPH -- morph position (runtime only, no regen)
 *      PDECAY -- spectral tilt p (0.5 glassy .. 4.0 deep)
 *      PDENS  -- partial count (2 .. 16, the LUT's usable band)
 *      PSKEW  -- even/odd partial modulation (-1..+1, form asymmetry)
 *      PORG   -- organic per-partial/per-frame amplitude jitter (0..1)
 *      PLEV   -- level of the procedural voice (runtime only, the mix
 *                gain in osc_mix.h; 0 = the voice is silent)
 *
 *  MEMORY
 *  ------
 *  frames_[8][256] int16 = 4 KB (BSS). Generation is one-time per dirty
 *  event (O(8 * 256 * 16) = 32768 sinf() calls ~= sub-millisecond on the
 *  STM32H725 hard-FPU, run at K-rate in blockPrepass, never in the audio
 *  path). The lookup is 2x int16 loads + 1 FMA per frame pair (no libm).
 *
 *  NO libm in the audio path: the lookup uses plain shifts/masks
 *  (256 = 1<<8). Generation uses the FastFloat helpers from
 *  utils/float_math.h (fasterpowf / fastsinfullf / fastcosfullf -- all
 *  inline, no libm symbols) but ONLY at K-rate (init / dirty events) --
 *  never the per-sample audio path.
 */

#include "utils/float_math.h" // fasterpowf, fastsinfullf, fastcosfullf

struct ProcTable
{
  // ---- Dimensions (constant) ------------------------------------------------
  static constexpr uint32_t kFrames = 8U;     // morph frames
  static constexpr uint32_t kPoints = 256U;   // points per frame (256 = 1<<8)
  static constexpr uint32_t kMask   = kPoints - 1U;
  static constexpr uint32_t kPartials = 16U;  // additive partials

  // ---- Frame data (int16, 4 KB) ---------------------------------------------
  int16_t frames_[kFrames][kPoints];

  // Generation parameters (mirrors of params_.proc_*, cached for regen).
  float decay = 2.0f;    // spectral tilt p (PDECAY)
  float dens  = 0.5f;    // normalized partial count (PDENS, 0..1)
  float skew  = 0.f;     // even/odd modulation (PSKEW, -1..+1)
  float org   = 0.f;     // organic jitter depth (PORG, 0..1)
  bool  dirty = true;    // regenerate on next blockPrepass

  // ---- Deterministic PRNG (splitmix32) --------------------------------------
  static uint32_t prng(uint32_t &state)
  {
    state += 0x9E3779B9U;
    uint32_t z = state;
    z = (z ^ (z >> 16)) * 0x85EBCA6BU;
    z = (z ^ (z >> 13)) * 0xC2B2AE35U;
    return z ^ (z >> 16);
  }

  // ---- Surface helpers -------------------------------------------------------
  // Normalized partial count (2..16) from the PDENS normalized value (0..1).
  static float partial_count(float dens)
  {
    return 2.f + 14.f * dens; // 2..16
  }

  // Partial count at a given normalized frame m01 (0..1). The morph sweeps
  // the count: frame 0 uses ~half the count (closed), frame 7 the full count
  // (open, dense).
  float partial_count_at(float m01)
  {
    const float full = partial_count(dens);
    return full * (0.5f + 0.5f * m01);
  }

  // Spectral tilt at a given normalized frame m01 (0..1): base DECAY plus a
  // per-frame offset so the MORPH moves closed -> open (frame 0 = tilt +1.5
  // -> darker rolloff, frame 7 = tilt -1.5 -> brighter).
  float decay_at(float m01)
  {
    return decay + 1.5f * (1.f - m01);
  }

  // ---- Generation ------------------------------------------------------------
  // Re-render all 8 frames from the cached params (decay/dens/skew/org).
  // Called from Osc::procRegen() at K-rate when `dirty` is set. Uses libm
  // (fasterpowf / fastsinfullf / fastcosfullf from utils/float_math.h) --
  // safe here (K-rate, never the audio path).
  void regen()
  {
    const float N = static_cast<float>(kFrames - 1);
    for (uint32_t f = 0; f < kFrames; f++)
    {
      const float m01 = (N > 0.f) ? (static_cast<float>(f) / N) : 0.f;
      const float p_count = partial_count_at(m01);
      const float p_decay = decay_at(m01);
      uint32_t seed = 0xA11CEU + f * 7919U; // per-frame deterministic seed
      float jamp[kPartials];
      for (uint32_t n = 0; n < kPartials; n++)
      {
        // rnd in [-1, +1)
        jamp[n] = (static_cast<float>(prng(seed) & 0xFFFFFFU) /
                   static_cast<float>(0xFFFFFFU)) * 2.f - 1.f;
      }
      int16_t *fr = frames_[f];
      for (uint32_t i = 0; i < kPoints; i++)
      {
        const float ph2pi = 6.283185307179586f *
                            static_cast<float>(i) / static_cast<float>(kPoints);
        float acc = 0.f;
        for (uint32_t n = 1; n <= kPartials; n++)
        {
          const float nf = static_cast<float>(n);
          // Partial window: 1 inside the count, 3-point taper at the edge
          // (avoids a hard aliasing cliff when PDENS changes).
          float win = 1.f;
          if (nf > p_count - 2.f)
          {
            win = p_count - nf + 1.f;
            if (win < 0.f) win = 0.f;
          }
          if (win <= 0.f) continue;
          const float amp = fasterpowf(nf, -p_decay) * win;
          const float sk = 1.f + skew * fastcosfullf(M_PI * nf);
          const float og = 1.f + org * jamp[n - 1];
          acc += amp * sk * og * fastsinfullf(nf * ph2pi);
        }
        // Scale to int16 (headroom ~2.5x, clamp guards overflow).
        int32_t v = (int32_t)(acc * 16000.f);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        fr[i] = (int16_t)v;
      }
    }
    dirty = false;
  }

  // ---- Audio-path lookup ------------------------------------------------------
  // Resolve the procedural wave sample at phase ph (0..1) for a fractional
  // morph position pos (0..kFrames-1). Bilinear: crossfade the two adjacent
  // frames (by frac of pos), each frame linearly interpolated in phase.
  // Returns [-1, +1] (int16 / 32768). Pure shifts/masks + FMAs -- no libm,
  // safe in the per-sample audio path.
  static float lookup(const int16_t (&frames)[kFrames][kPoints], float pos, float ph)
  {
    if (pos < 0.f) pos = 0.f;
    const float maxpos = static_cast<float>(kFrames - 1);
    if (pos > maxpos) pos = maxpos;
    const uint32_t i = (uint32_t)pos;
    const float frac = pos - static_cast<float>(i);

    float pf = ph * static_cast<float>(kPoints);
    if (pf < 0.f) pf = 0.f;
    else if (pf >= static_cast<float>(kPoints)) pf -= static_cast<float>(kPoints);
    const uint32_t pi = (uint32_t)pf;
    const float pfr = pf - static_cast<float>(pi);
    const uint32_t pi1 = (pi + 1U) & kMask;

    const int16_t *fa = frames[i];
    const float sa = static_cast<float>(fa[pi]) * (1.f - pfr) +
                     static_cast<float>(fa[pi1]) * pfr;

    float result = sa;
    if (frac > 0.001f && i + 1U < kFrames)
    {
      const int16_t *fb = frames[i + 1U];
      const float sb = static_cast<float>(fb[pi]) * (1.f - pfr) +
                       static_cast<float>(fb[pi1]) * pfr;
      result = sa * (1.f - frac) + sb * frac;
    }
    return result * (1.f / 32768.f);
  }
};
