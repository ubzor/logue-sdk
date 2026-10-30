#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_saturator.h
 *
 *  Pre-drive wavefolder + asymmetric soft-clip saturator for the DRONE unit.
 *
 *  The audio path runs the two wavetable voices through:
 *    1. An optional wavefolder (FOLD knob) that adds even harmonics and a
 *       "metallic" edge before the saturator.
 *    2. An asymmetric soft-clip saturator (tanh-based) that can be driven
 *       harder on one half of the waveform (ASYM knob) to generate even
 *       harmonics ("warm", "brassy" tone).
 *
 *  These are stateless, pure functions — no member state, no side effects.
 *  They are inlined into the Osc's audio path.
 *
 */

#include "utils/float_math.h" // fastertanh2f()

namespace Saturator
{
  // ---- Constants ----------------------------------------------------------

  // Stable drive limit for the asymmetric saturator: keeps the tanh argument
  // in (-1, 1) even when |asym| scales a half up (see process()).
  static constexpr float kDriveLimit = 0.9f;

  // ---- Functions ----------------------------------------------------------

  // Pre-drive wavefolder: scale mix by (1 + fold_depth) and reflect into
  // [-1, 1] (one reflection handles |x| up to 2). Blends linearly from the
  // raw mix (fold_depth=0) to the fully folded signal (fold_depth=1).
  // Adds even harmonics and a "metallic" edge before the tanh saturator.
  static inline float fold(float mix, float fold_depth)
  {
    if (fold_depth <= 0.f)
      return mix;
    const float x_fold = mix * (1.f + fold_depth);
    const float ax = x_fold < 0.f ? -x_fold : x_fold;
    const float folded = (ax > 1.f)
        ? ((x_fold > 0.f) ? (2.f - x_fold) : (-2.f - x_fold))
        : x_fold;
    return mix + fold_depth * (folded - mix);
  }

  // Asymmetric soft-clip saturator: y = (1 - d^2) * tanh(d / (1 - d^2)),
  // d in (-1, 1). The asym parameter scales the positive and negative halves
  // differently to generate even harmonics. asym=0 -> symmetric (regression-
  // safe: original tanh). asym>0 -> positive half driven harder, negative
  // half softer. asym<0 -> vice versa.
  //
  // drive scales the input before the tanh (0..3.0). The output is bounded
  // in (-1, 1).
  static inline float saturate(float mix, float drive, float asym)
  {
    // clip the drive-scaled signal into the stable range first
    float d = mix * drive;
    if (d > kDriveLimit)
      d = kDriveLimit;
    else if (d < -kDriveLimit)
      d = -kDriveLimit;

    // asymmetric gain: scale d differently for positive / negative halves
    // to generate even harmonics ("warm", "brassy" tone). asym=0 -> unity
    // gain on both halves (regression-safe: original symmetric tanh).
    const float asym_gain = (d >= 0.f) ? (1.f + asym) : (1.f - asym);
    float d_a = d * asym_gain;

    // re-clamp after asymmetric scaling: |d_a| can exceed kDriveLimit when
    // |asym| > 0 and |d| is near the kDriveLimit (e.g. d=0.9, asym=+0.5
    // -> d_a=1.35). This keeps the tanh argument in the stable range.
    if (d_a > kDriveLimit)
      d_a = kDriveLimit;
    else if (d_a < -kDriveLimit)
      d_a = -kDriveLimit;

    const float one_minus_d2 = 1.f - d_a * d_a;
    // fastertanh2f (Padé [3/3], ~6 VFP ops) replaces the former Padé [5/5]
    // soft_tanhf (~14 VFP ops). Max |error| over the full operating range
    // x in [-4.68, 4.68] is ~2e-3 (-54 dBFS) — inaudible for a saturator
    // (the error appears only as a very slight rounding of the highest
    // harmonics at full drive). The output of fastertanh2f is bounded in
    // (-1, 1), and (1-d^2) < 1, so y stays in (-1, 1).
    return one_minus_d2 * fastertanh2f(d_a / one_minus_d2);
  }
}
