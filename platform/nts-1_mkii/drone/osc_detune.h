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
  static inline float factor(float cents)
  {
    const float x = cents * kCentsToX; // cents * ln(2) / 1200
    return 1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
  }

  // Phase increment for a base w0 detuned by `cents`:
  //   w0 * 2^(cents/1200)
  static inline float w0_detuned(float w0, float cents)
  {
    return w0 * factor(cents);
  }
}
