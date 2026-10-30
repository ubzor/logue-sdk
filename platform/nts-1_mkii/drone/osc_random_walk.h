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
 *  When the RATE AMT knob is non-zero, both oscillators' detunes wander
 *  around their knob values: RATE AMT sets the maximum deviation (quadratic
 *  mapping: knob^2 -> 0..+/-10 cents hard ceiling), and RATE SPD sets the
 *  "restlessness" — how often the targets change and how quickly the
 *  deviations track them (0.1 s fast, restless .. 10 s slow, lazy). The two
 *  voices wander with independent random targets on the same period, so the
 *  inter-voice beat frequency shifts over time (asymmetric chorus). With
 *  RATE AMT at 0 the detunes are static, preserving the original behavior.
 *
 *  The random-walk state is owned by the Osc (rw_dev_a_cents_, etc.); this
 *  struct provides the advance() method that updates the deviations per
 *  sample, and the setup() method that recomputes the tracking coefficient
 *  when the rate parameters change.
 *
 */

#include "utils/float_math.h" // fasterpowf()

struct RandomWalk
{
  // Maximum deviation (cents) at RATE AMT = 100%. The quadratic mapping in
  // setParameter (rw_amount = (amt/1023)^2) ensures the knob feels linear
  // while the walk depth stays gentle (0..+/-10 cents).
  static constexpr float kMaxCents = 10.f;

  // Random-walk period range: RATE SPD 0 -> kPeriodMinSec (0.1 s, fast/restless),
  // RATE SPD 1 -> kPeriodMaxSec (10 s, slow/lazy). The period is mapped
  // logarithmically: period = kPeriodMinSec * 100^rw_rate.
  static constexpr float kPeriodMinSec = 0.1f;
  static constexpr float kPeriodMaxSec = 10.f;

  // Sample rate (constant across all logue products).
  static constexpr float kSampleRate = 48000.f;

  // ---- State (owned by the Osc, passed by reference) ----

  float dev_a;      // current deviation (cents), osc A
  float dev_b;      // current deviation (cents), osc B
  float target_a;   // random target (cents), osc A
  float target_b;   // random target (cents), osc B
  float max_dev;    // hard ceiling (cents), = rw_amount * kMaxCents
  float k;          // tracking coefficient (per sample), recomputed on setup()
  uint32_t counter; // target update counter (samples)
  uint32_t interval; // target update interval (samples), recomputed on setup()

  // ---- Setup (called from Osc when rw params change or on init) ----

  // Recompute the tracking coefficient and target update interval from the
  // rate parameters. rw_rate is the linearized knob value (0..1), rw_amount
  // is the quadratic-mapped knob value (0..1).
  void setup(float rw_rate, float rw_amount)
  {
    max_dev = rw_amount * kMaxCents;
    const float period = kPeriodMinSec * fasterpowf(100.f, rw_rate);
    k = 1.f / (1.f + period * kSampleRate);
    interval = static_cast<uint32_t>(period * kSampleRate);
  }

  // ---- Audio path (called once per sample from Osc::process) ----

  // Advance the random-walk deviations by one sample. If amount is 0 (knob at
  // 0), the walk is disabled and the deviations stay at their current values
  // (the Osc uses the cached static detunes instead). Otherwise:
  //   - periodically regenerate random targets in [-max_dev, +max_dev]
  //   - smoothly track the targets: dev += (target - dev) * k
  //   - clamp to [-max_dev, +max_dev] (safety)
  void advance(float rw_amount, float (*white)())
  {
    if (rw_amount <= 0.f)
      return; // walk disabled

    // Update the targets periodically (independent per voice)
    if (++counter >= interval)
    {
      counter = 0;
      // Generate new random targets in [-max_dev, +max_dev]
      target_a = white() * max_dev;
      target_b = white() * max_dev;
    }

    // Smoothly track the targets: dev += (target - dev) * k
    dev_a += (target_a - dev_a) * k;
    dev_b += (target_b - dev_b) * k;

    // Safety clamps (should rarely trigger if targets are within bounds)
    if (dev_a > max_dev)
      dev_a = max_dev;
    else if (dev_a < -max_dev)
      dev_a = -max_dev;
    if (dev_b > max_dev)
      dev_b = max_dev;
    else if (dev_b < -max_dev)
      dev_b = -max_dev;
  }

  // Reset the random-walk state (center the walk, zero the targets).
  void reset()
  {
    dev_a = 0.f;
    dev_b = 0.f;
    target_a = 0.f;
    target_b = 0.f;
    counter = 0;
  }
};
