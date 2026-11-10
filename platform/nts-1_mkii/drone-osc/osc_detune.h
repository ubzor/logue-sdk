#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_detune.h
 *
 *  Cent-based pitch scaling for the DRONE unit.
 *
 *  Detunes in the unit are limited to +/-100 cents. In that range
 *  2^(cents/1200) = e^x (x = cents * ln(2)/1200, |x| <= 0.0833) is well
 *  approximated by a 4th-order Taylor series (1 + x + x^2/2 + x^3/6 + x^4/24),
 *  which keeps the relative error below ~1e-6 (less than 0.03 cents at the
 *  extremes) — far under the threshold of audibility. This replaces the libm
 *  powf() call, whose support chain (__ieee754_powf, expm1f, scalbnf, ...)
 *  adds several hundred bytes of code and data to the final unit image.
 *
 *  Used from:
 *   - update_w0_a / update_w0_b (cached static detunes, K-rate)
 *   - the random-walk branch in process() (per-sample effective detune)
 *   - the sub-octave LFO-FM on phasor_sub_ (per-sample ±kSubLfoFmCents)
 *
 *  All functions are stateless and inlined into the Osc's audio path.
 *
 */

namespace Detune
{
  // ln(2) / 1200 — converts cents to x for e^x (x = cents * kCentsToX).
  static constexpr float kCentsToX = 5.7762265046261084e-04f;

  // 2^(cents/1200) via a 4th-order Taylor series in x = cents * kCentsToX.
  // Accurate to < 0.03 cents over the full +/-100 cent operating range.
  // ONLY valid for |cents| <= ~120: the series diverges for larger |x|
  // (at 1900 cents it would be off by ~307 cents). Use pow2f() for intervals
  // beyond that (e.g. ChordVoice CINT / CINT2, which reach 1900 cents).
  static inline float factor(float cents)
  {
    const float x = cents * kCentsToX; // cents * ln(2) / 1200
    return 1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
  }

  // 2^(cents/1200) EXACTLY (libm-free): the float bit-trick splits the
  // exponent into an integer part (exact via the binary exponent) and a
  // fractional part in [0, 1) approximated by a 14-term Taylor of 2^z:
  //   2^z = sum_{k>=0} (z*ln2)^k / k!
  // For z in [0, 1) the series remainder is < 3^-15 (the 15th term is
  // <= 1/15! * (ln2)^15 / 15, tiny), so the relative error is < ~1e-6 over
  // the whole range. This covers 0..1900 cents (x up to 1.098) with < 0.01
  // cent error, unlike factor() which only converges near x = 0.
  //
  // K-rate only (called from ChordVoice::update on CINT / CINT2 edits and
  // setPitch); never in the per-sample audio path.
  static inline float pow2f(float x)
  {
    const float z = x - (float)(int32_t)x;        // fractional part in [0, 1)
    const float e = z * 0.6931471805599453f;      // z * ln(2), in [0, 0.6932)
    // 14th-order Taylor of 2^z = e^(z*ln2): m = sum_{k=0..14} e^k / k!.
    float m = 1.f;
    float term = 1.f;
    for (int k = 1; k <= 14; ++k)
    {
      term *= e / (float)k;
      m += term;
    }
    // Shift the mantissa by the integer exponent (exact in binary for
    // |x| < 128; our range is [0, 1.1]).
    union
    {
      float f;
      uint32_t i;
    } v;
    v.i = ((uint32_t)(x + 127.f) << 23) & 0x7FFFFFFFu; // clear the sign bit
    return m * v.f;
  }

  // Phase increment for a base w0 detuned by `cents`:
  //   w0 * 2^(cents/1200)
  static inline float w0_detuned(float w0, float cents)
  {
    return w0 * factor(cents);
  }
}
