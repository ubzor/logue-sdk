#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_random_walk.h
 *
 *  Random-walk detune wandering for the DRONE unit (dual channel).
 *
 *  When the RATE AMT knob is non-zero, the two oscillators' detunes wander
 *  around their knob values:
 *    RATE AMT (depth)  — maximum deviation, quadratic mapping (knob^2 -> 0..1
 *                        normalized; the Osc scales it to +/-10 cents for the
 *                        detune voices),
 *    RATE SPD (period) — how often the random targets are regenerated
 *                        (0.1 s fast/restless .. 10 s slow/lazy, log mapping).
 *                        It controls ONLY the target-update period.
 *    RATE SMTH (smooth)— how quickly the deviations track the targets, as a
 *                        time constant (10 ms snappy .. 1.5 s glacial, log).
 *                        Keeping the time constant independent of the period is
 *                        what decouples the walk depth from SPEED (the old
 *                        code derived tracking from the period, so the audible
 *                        depth changed with SPEED).
 *  The two voices wander with independent random targets on the same period,
 *  so the inter-voice beat frequency shifts over time (asymmetric chorus).
 *  With RATE AMT at 0 the detunes are static, preserving the original
 *  behavior.
 *
 *  The walk state (dev_a / dev_b, normalized to [-1, +1]) is owned by the
 *  Osc; this struct provides advance() (updates the deviations per sample)
 *  and setup() (recomputes the ceiling / interval / tracking from the three
 *  RATE parameters). The Osc scales the normalized deviations at the point of
 *  use, so the same walk can later drive other parameters (wavetable morph,
 *  drive, ...) without changing this struct.
 *
 */

#include "utils/float_math.h" // fasterpowf (init-time LUT computation only)

struct RandomWalk
{
  // Random-walk period range: RATE SPD 0 -> kPeriodMinSec (0.1 s, fast/restless),
  // RATE SPD 1 -> kPeriodMaxSec (10 s, slow/lazy). The period is mapped
  // logarithmically: period = kPeriodMinSec * 100^rw_rate. It controls ONLY how
  // often the targets are regenerated — how quickly the deviations track them
  // is the SMOOTH knob (see kSmoothMinSec / kSmoothMaxSec and setup()).
  static constexpr float kPeriodMinSec = 0.1f;
  static constexpr float kPeriodMaxSec = 10.f;

  // Random-walk "smoothing" (time constant) range: RATE SMTH 0 -> kSmoothMinSec
  // (10 ms, snappy — the deviations jump to each new target quickly), RATE SMTH
  // 1 -> kSmoothMaxSec (1.5 s, glacial — the deviations barely reach a target
  // before it changes). The tracking coefficient is k = 1/(1 + tau*sr) where
  // tau = kSmoothMinSec * 100^smooth (log mapping, same shape as the period).
  //
  // Keeping the time constant INDEPENDENT of the period is what makes the walk
  // depth (RATE AMT) stable regardless of SPEED: with tau = period the old code
  // produced "plateaus" (fast tracking, then waiting) at slow periods and
  // "cut-off" (not reaching the target) at fast ones, so the audible depth
  // silently depended on SPEED.
  static constexpr float kSmoothMinSec = 0.010f;  // 10 ms
  static constexpr float kSmoothMaxSec = 1.5f;    // 1.5 s

  // Sample rate (constant across all logue products).
  static constexpr float kSampleRate = 48000.f;

  // ---- State (owned by the Osc, passed by reference) ----
  // The deviations (dev_a / dev_b) and targets (target_a / target_b) are
  // NORMALIZED to [-1, +1]. The Osc multiplies them by the per-parameter scale
  // (e.g. +/-10 cents for detune, +/-N tables for a wavetable morph) at the
  // point of use, so the same walk state can drive any parameter. max_dev is
  // the normalized depth ceiling (= rw_amount, 0..1); the actual scale is the
  // Osc's business (see RandomWalk::scale applied in Osc::process).
  float dev_a;       // current normalized deviation [-1, +1], osc A
  float dev_b;       // current normalized deviation [-1, +1], osc B
  float target_a;    // normalized random target [-1, +1], osc A
  float target_b;    // normalized random target [-1, +1], osc B
  float max_dev;     // normalized depth ceiling (0..1), = rw_amount
  float k;           // tracking coefficient (per sample), recomputed on setup()
  uint32_t counter;  // target update counter (samples)
  uint32_t interval; // target update interval (samples), recomputed on setup()
  // Cached RATE SPD / RATE SMTH values (0..1). setup() recomputes k/interval
  // only when these change (K-rate, not per-block), avoiding 2 fasterpowf
  // calls per block in Osc::process when the walk is active.
  float cached_rate = -1.f;    // -1 = uninitialized
  float cached_smooth = -1.f;

  // ---- Setup (called from Osc when rw params change or on init) ----

  // Recompute the depth ceiling (always), and the target-update interval +
  // tracking coefficient (only when rw_rate or rw_smooth changed).
  //
  // The 2 fasterpowf calls (period + tau) are the expensive part; caching the
  // last rw_rate/rw_smooth values avoids re-running them on every call when
  // only rw_amount changed (or when Osc::process calls setup() per block with
  // unchanged RATE parameters). The K-rate path (apply_edit_param) calls
  // setup() on every knob turn, so the cache is effective in both contexts.
  void setup(float rw_rate, float rw_amount, float rw_smooth)
  {
    max_dev = rw_amount; // normalized 0..1 (always update — cheap)

    // Recompute k / interval only when the period or smoothing changed.
    if (rw_rate != cached_rate || rw_smooth != cached_smooth)
    {
      cached_rate = rw_rate;
      cached_smooth = rw_smooth;
      const float period = kPeriodMinSec * fasterpowf(100.f, rw_rate);
      const float tau = kSmoothMinSec * fasterpowf(100.f, rw_smooth);
      k = 1.f / (1.f + tau * kSampleRate);
      interval = static_cast<uint32_t>(period * kSampleRate);
      if (interval < 1U)
        interval = 1U; // never zero-division / every-sample target churn
    }
  }

  // ---- Audio path (called once per sample from Osc::process) ----

  // Advance the random-walk deviations by one sample. If amount is 0 (knob at
  // 0), the walk is disabled and the deviations stay at their current values
  // (the Osc uses the cached static detunes instead). Otherwise:
  //   - periodically regenerate random targets in [-1, +1]
  //   - smoothly track the targets: dev += (target - dev) * k
  //   - clamp to [-1, +1] (safety; the Osc clamps the scaled result)
  void advance(float rw_amount, float (*white)())
  {
    if (rw_amount <= 0.f)
      return; // walk disabled

    // Update the targets periodically (independent per voice)
    if (++counter >= interval)
    {
      counter = 0;
      // Generate new random targets in [-1, +1]
      target_a = white();
      target_b = white();
    }

    // Smoothly track the targets: dev += (target - dev) * k
    dev_a += (target_a - dev_a) * k;
    dev_b += (target_b - dev_b) * k;

    // Safety clamps to the normalized range (should rarely trigger)
    if (dev_a > 1.f)
      dev_a = 1.f;
    else if (dev_a < -1.f)
      dev_a = -1.f;
    if (dev_b > 1.f)
      dev_b = 1.f;
    else if (dev_b < -1.f)
      dev_b = -1.f;
  }

  // Reset the random-walk state (center the walk, zero the targets).
  void reset()
  {
    dev_a = 0.f;
    dev_b = 0.f;
    target_a = 0.f;
    target_b = 0.f;
    counter = 0;
    cached_rate = -1.f;
    cached_smooth = -1.f;
  }
};
