#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_sub2.h
 *
 *  Second sub voice (SUB2): f/4, very-soft-saturated, LPF'd, gated.
 *
 *  A sine at one quarter of the base frequency (f/4), run through a very
 *  soft tanh saturator (kDrive), then a one-pole LPF (cuts the saturator's
 *  harmonics so the voice stays in the low end), and a pitch gate (closes
 *  when the base frequency drops below kGateHz, i.e. f/4 < 30 Hz). The gate
 *  is smoothed first-order so switching notes never clicks.
 *
 *  The sub2 voice is mixed in parallel with the main saturator (it bypasses
 *  the main soft-clip on purpose: low-frequency sine through tanh would be
 *  heavily attenuated). Per-branch gain is chosen so the worst-case peak
 *  stays below 0.3 (see Osc::process).
 *
 */

#include "utils/float_math.h" // fastertanh2f()

struct Sub2Voice
{
  // Very-soft saturator drive: a light tanh at kDrive (~1.3) rounds the
  // sine and adds just enough odd harmonics for "body" without turning
  // it into a distorted bass. fastertanh2f (Padé [3/3]) is inaudibly
  // accurate and bounded in (-1, 1).
  static constexpr float kDrive = 1.3f;

  // One-pole LPF cutoff (Hz): trims the saturator's 3rd/5th harmonics so the
  // voice stays in the low end instead of muddying the mids.
  static constexpr float kLpfCutoffHz = 200.f;

  // Pitch-gate threshold (Hz): the gate closes when the base (saw) frequency
  // drops below 4 * kGateHz (≈120 Hz ≈ C3), i.e. when f/4 < kGateHz (30 Hz,
  // infrasonic). The gate opens/closes with a first-order smoothing time-constant
  // kGateTauSec.
  static constexpr float kGateHz = 30.f;
  static constexpr float kGateTauSec = 0.005f;

  // Sample rate (constant across all logue products).
  static constexpr float kSampleRate = 48000.f;

  // ---- State (per-sample, owned by the Osc) ----

  float lpf_state;  // one-pole LPF state (integrator)
  float gate;       // smoothed pitch-gate amplitude (0 = silenced, 1 = open)

  // Fixed coefficients (recomputed by the Osc in update_w0_sub2, never in the
  // audio path).
  float lpf_a;      // one-pole LPF coefficient (200 Hz)
  float gate_k;     // pitch-gate smoothing coefficient
  float gate_hz;    // base (saw) Hz at which f/4 == kGateHz

  // ---- Setup (called from Osc when the base pitch changes) ----

  // Recompute the fixed coefficients from the base phase increment w0.
  void update(float w0)
  {
    // one-pole LPF: a = 1 - exp(-2*pi*fc/sr)
    lpf_a = 1.f - expf(-6.28318530718f * kLpfCutoffHz / kSampleRate);
    // first-order gate: k = 1 - exp(-1/tau)
    gate_k = 1.f - expf(-1.f / (kGateTauSec * kSampleRate));
    // base frequency (Hz) at which the f/4 fundamental equals kGateHz
    gate_hz = 4.f * kGateHz;
    (void)w0; // reserved for future pitch-dependent coefficients
  }

  // ---- Audio path (called once per sample from Osc::process) ----

  // Compute the sub2 sample from the f/4 phasor (0..1) and the base phase
  // increment w0 (used for the pitch gate). Returns 0 if level is 0.
  float process(float phasor_sub2, float w0, float level)
  {
    if (level <= 0.f)
      return 0.f; // voice off

    // f/4 sine in -1..1 (LUT)
    const float s2 = osc_sinf(phasor_sub2);

    // very soft saturator: a light tanh at kDrive (~1.3) rounds the
    // sine and adds just enough odd harmonics for "body" without turning
    // it into a distorted bass. fastertanh2f (Padé [3/3]) is inaudibly
    // accurate and bounded in (-1, 1).
    const float s2_sat = fastertanh2f(s2 * kDrive);

    // one-pole LPF (200 Hz): trims the saturator's 3rd/5th harmonics so the
    // voice stays in the low end instead of muddying the mids. First-order
    // low-pass: state += (in - state) * a; out = state.
    lpf_state += (s2_sat - lpf_state) * lpf_a;

    // pitch gate: fully open when the base (saw) frequency is above the
    // threshold (f/4 >= 30 Hz), closed below it (f/4 infrasonic). Smoothed
    // first-order so switching notes never clicks. w0 = f/sr, so the base
    // frequency in Hz is w0 * sr; compare against gate_hz (4*30).
    const float open_target = (w0 * kSampleRate > gate_hz) ? 1.f : 0.f;
    gate += (open_target - gate) * gate_k;

    return lpf_state * gate;
  }

  // Reset the sub2 voice state.
  void reset()
  {
    lpf_state = 0.f;
    gate = 0.f;
  }
};
