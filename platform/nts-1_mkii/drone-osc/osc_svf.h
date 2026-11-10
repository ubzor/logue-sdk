#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_svf.h
 *
 *  Two-pole (12 dB/oct) state-variable filter for the DRONE unit.
 *
 *  A state-variable filter (SVF) integrates the input into a "velocity"
 *  state and integrates that into a "position" state. The position is a
 *  second-order low-pass. We crossfade the raw input with the LPF output
 *  (dry/wet) so the unit can "sweep" the timbre from bright (mostly raw
 *  signal) to dark (heavily low-passed) without a full filter mode selector.
 *
 *  The cutoff frequency is set at K-rate by the FCUT knob (log-mapped
 *  80 Hz .. 12 kHz) and is modulated per-sample by the shared random walk
 *  (RWFC knob): the effective cutoff wanders by +/-kRwCutoffHalfOctaves
 *  octaves around the knob value, so the drone's top end "breathes" on the
 *  same 0.1 s .. 10 s timescale as the detune / morph / drive walks.
 *  Resonance (Q) is a fixed knob (RES, 0.5 .. 2.5).
 *
 *  Stability: the per-sample cutoff coefficient is clamped to (0, 0.92] and
 *  the damping coefficient to [0.18, 1], which keeps the biquad's poles
 *  strictly inside the unit circle across the whole 80 Hz .. 12 kHz range
 *  (at 12 kHz the raw coefficient is ~0.98, clamped to 0.92; the resulting
 *  -3 dB point is ~9.5 kHz, a safe margin below Nyquist at 48 kHz).
 *
 *  No dynamic allocation: all state lives in this struct (owned by the Osc).
 */

#include "utils/float_math.h"

namespace Svf
{
  // ---- Constants ----------------------------------------------------------

  // Cutoff range (Hz): FCUT knob 0..100% maps logarithmically onto
  // [kCutoffMinHz, kCutoffMaxHz]. 80 Hz keeps the low end intact; 12 kHz is
  // a safe full-open point (well below Nyquist, no aliasing of the sweep).
  static constexpr float kCutoffMinHz = 80.f;
  static constexpr float kCutoffMaxHz = 12000.f;

  // Resonance range: RES knob 0..100% maps linearly to [0.5, 2.5]. 0.5 is a
  // gently rounded (over-damped) filter, 2.5 is a pronounced peak. Q is kept
  // below 3 to avoid the SVF's high-Q ringing instability at the top of the
  // cutoff range.
  static constexpr float kQMin = 0.5f;
  static constexpr float kQMax = 2.5f;

  // Random-walk cutoff modulation: RWFC knob 0..100% sets the depth, and the
  // normalized walk deviation is scaled to +/-kRwCutoffHalfOctaves octaves
  // around the FCUT knob value (1 half-octave = 6 dB). At RWFC = 100% the
  // cutoff wanders a full octave each way; lower settings give a subtle
  // shimmer. The modulation is multiplicative (2^(+/-half-oct * dev)) so it
  // is symmetric in dB and never crosses zero.
  static constexpr float kRwCutoffHalfOctaves = 1.f;

  // Per-sample coefficient clamps (see the stability note in the file header).
  static constexpr float kCcMin = 0.001f;  // lower clamp on the cutoff coeff
  static constexpr float kCcMax = 0.92f;   // upper clamp (keeps poles in the disc)
  static constexpr float kAeMin = 0.18f;   // lower clamp on the damping coeff
  static constexpr float kAeMax = 1.f;

  // ---- Coefficient setup (K-rate) -----------------------------------------

  // Cutoff Hz from a log-mapped knob value (0..1):
  //   fc = kCutoffMinHz * (kCutoffMaxHz/kCutoffMinHz)^knob
  static inline float cutoffHz(float knob)
  {
    return kCutoffMinHz * fasterpowf(kCutoffMaxHz / kCutoffMinHz, knob);
  }

  // Resonance Q from a linear knob value (0..1): [kQMin, kQMax].
  static inline float quality(float knob)
  {
    return kQMin + knob * (kQMax - kQMin);
  }

  // Per-sample SVF coefficient `cc` for a given cutoff frequency (Hz).
  // cc = 2*sin(w0) with w0 = 2*pi*fc/sr, clamped to (kCcMin, kCcMax].
  //
  // K-rate only (called from updateCutoff on FCUT / RES knob turns, never in
  // the audio path). We use fastsinf (Paul Mineiro's FastFloat, inlined, ~8
  // VFP ops, no lookup table) instead of libc sinf: for fc in 80..12000 Hz at
  // 48 kHz, w0 = 2*pi*fc/sr is in [0.0105, 1.571] rad, well inside the
  // [-pi, +pi] validity range of fastsinf, and the coefficient error from the
  // approximation is far below audibility at K-rate. This avoids pulling the
  // large soft-float libc sinf (~1 KB of .text) into the unit image.
  static inline float ccForHz(float fc, float sr)
  {
    const float w0 = 6.283185307179586f * fc / sr;
    float cc = 2.f * fastsinf(w0);
    if (cc < kCcMin) cc = kCcMin;
    else if (cc > kCcMax) cc = kCcMax;
    return cc;
  }

  // Damping coefficient `ae` from the resonance Q: ae = 1 - cc/(2*Q).
  // Clamped to [kAeMin, kAeMax] so the poles stay inside the unit circle even
  // when Q is high and the cutoff is near the top of its range.
  static inline float aeForQ(float q, float cc)
  {
    float ae = 1.f - cc / (2.f * q);
    if (ae < kAeMin) ae = kAeMin;
    else if (ae > kAeMax) ae = kAeMax;
    return ae;
  }

  // ---- State (per-sample, owned by the Osc) --------------------------------
  struct State
  {
    // Integrator states (position / velocity). Zeroed on init / reset.
    float vel = 0.f;   // band-pass state (velocity)
    float pos = 0.f;   // low-pass state (position)
    // K-rate coefficients (recomputed by updateCutoff, never in the audio path).
    float cc = 0.f;    // per-sample cutoff coefficient (0, 0.92]
    float ae = 1.f;    // per-sample damping coefficient [0.18, 1]
    // Walk cutoff scaling (per-sample, applied in process). cc_w is the
    // walk-modified cc (cc * 2^(+/-half-oct * dev)), clamped like cc.
    float cc_w = 0.f;

    // Recompute cc / ae from the FCUT / RES knob values and the sample rate.
    // Called at K-rate (apply_edit_param for FCUT / RES), never in the audio
    // path. `cc_w` is left as-is (it is recomputed per-sample in process).
    void updateCutoff(float fc, float q, float sr)
    {
      cc = ccForHz(fc, sr);
      ae = aeForQ(q, cc);
      if (cc_w == 0.f) cc_w = cc; // first-time init: start unmodulated
    }

    // Recompute ONLY the damping coefficient (ae) from Q, leaving the cutoff
    // coefficient (cc) untouched. Used by the RES knob path, which changes Q
    // but not the cutoff — this skips the (more expensive) cutoffHz/fastsinf
    // recompute. K-rate only.
    void updateResonance(float q)
    {
      ae = aeForQ(q, cc);
    }

    // Reset the integrator states (the coefficients are kept — they are
    // derived from the knobs, not from audio history).
    void reset()
    {
      vel = 0.f;
      pos = 0.f;
      cc_w = cc;
    }

    // Advance the filter by one sample.
    //   in  : input sample (the saturator output, before the sub mix)
    //   wet : dry/wet crossfade, 0..1 (0 = raw signal, 1 = full LPF)
    // The walk-modified cutoff coefficient cc_w is set by the caller (the Osc
    // computes it from cc and the random-walk deviation each sample). The raw
    // `in` is blended with the low-pass output: out = in*(1-wet) + lpf*wet.
    float process(float in, float wet)
    {
      // Standard SVF (Ladder, low-pass + band-pass):
      //   vel += cc * (in - pos - ae*vel)
      //   pos += cc * vel
      const float v = vel + cc_w * (in - pos - ae * vel);
      const float p = pos + cc_w * v;
      vel = v;
      pos = p;
      // Dry/wet: crossfade the raw input with the low-pass output.
      return in * (1.f - wet) + p * wet;
    }
  };
}