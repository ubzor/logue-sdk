#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_params.h
 *
 *  Runtime DSP parameter struct for the DRONE unit.
 *
 *  Holds the OscParamsData struct — the live DSP state, populated by
 *  apply_edit_param (in osc.h) from the raw 10-bit values of the currently
 *  selected parameter. Reset to defaults on init().
 *
 *  The audio-parameter enum (PARAM_SEL, PARAM_VAL, DETUNE..WAV2), the
 *  multiplexer constants (kEditParamCount, kEditParamFirst), and the display
 *  scaling constants (kDetuneMaxCents, kSatuMax, kRawMid, kRawMax) remain in
 *  the Osc class (osc.h) because they are tightly coupled to the multiplexer
 *  state (select_, edit_value_) and the apply_edit_param / setParameter /
 *  getParameterStrValue methods.
 *
 */

// ---- Runtime DSP parameters -------------------------------------------------
// The live DSP state, populated by apply_edit_param from the raw 10-bit values
// of the currently selected parameter. Reset to defaults on init().
struct OscParamsData
{
  float detune_cents;   // -100.f .. +100.f, knob A (osc B)
  float detune_a_cents; // -100.f .. +100.f, knob A (osc A)
  float drive;          // 0.f .. 3.f, knob B
  float rw_amount;    // 0.f .. 1.f, edit: random-walk depth
  float rw_rate;      // 0.f .. 1.f, edit: random-walk speed
  float sub_level;    // 0.f .. 1.f, edit: sub-octave (f/2) sine level
  float sub2_level;   // 0.f .. 1.f, edit: second sub (f/4) saturated level
  float fold_depth;   // 0.f .. 1.f, edit: pre-drive wavefolder depth
  float asym;         // -1.f .. +1.f, edit: asymmetric saturator (0 = symmetric)
  float wave_a;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc A wavetable index
  float wave_b;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc B wavetable index
  float chord_level;  // 0.f .. 1.f, edit: chord-voice level

  void reset()
  {
    detune_cents = 0.f;     // default: in tune
    detune_a_cents = 0.f;   // default: in tune
    drive = 2.f;            // default: moderate drive (50% of 0..3 range)
    rw_amount = 0.f;    // default: random walk off
    rw_rate = 0.5f;     // default: mid speed
    sub_level = 0.5f;   // default: half-level sub
    sub2_level = 0.f;   // default: second sub off (regression-safe)
    fold_depth = 0.f;   // default: wavefolder off (regression-safe)
    asym = 0.f;         // default: symmetric saturator (regression-safe)
    wave_a = 0.f;       // default: first wave of bank A
    wave_b = 0.f;       // default: first wave of bank A
    chord_level = 0.f;  // default: chord voice off (regression-safe)
  }

  OscParamsData() { reset(); }
};
