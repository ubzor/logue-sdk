#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_chord.h
 *
 *  Chord voice (third oscillator) for the DRONE unit.
 *
 *  A third wavetable oscillator running at a continuous interval above the
 *  base note. The CINT knob (0..100%) maps linearly to 0..1900 cents:
 *
 *    0%   -> 0 cents   (unison — blends with the main voices)
 *    37%  -> ~700 cents (perfect fifth, "open" drone)
 *    63%  -> ~1200 cents (octave, "clean" doubling)
 *    100% -> 1900 cents (octave + fifth, "full" drone)
 *
 *  In between: smooth glides through minor thirds, major thirds, sixths,
 *  etc. — any interval is reachable, no quantization.
 *
 *  The chord voice uses the WAV1 (osc A) wavetable so it blends with the
 *  main voice, not the detuned/chorus voice (osc B). It shares the same
 *  FOLD/SATU/ASYM timbre chain (mixed before the saturator). Random walk
 *  does NOT apply — the chord voice is a stable "anchor", not a beating
 *  voice.
 *
 *  Level (CHORD knob, 0..100%) is applied by the Osc with normalized gain:
 *    gain = kMixChord * lvl / (1 + lvl)
 *  so turning up the chord voice does not push the main 2-osc mix into
 *  the saturator's clipping region.
 *
 *  The interval is recomputed at K-rate (setParameter(CINT) / setPitch)
 *  using a 6th-order Taylor series for 2^(cents/1200) (see osc_detune.h
 *  for the same approach). The audio path is one multiply.
 *
 *  No dynamic allocation: all state lives in the struct (owned by Osc).
 *
 */

#include "osc_detune.h" // Detune::factor() — 2^(cents/1200) Taylor

struct ChordVoice
{
  // Maximum interval (cents) at CINT = 100%. 1900 = octave + perfect fifth.
  // 0% = 0 cents (unison), 100% = 1900 cents.
  static constexpr float kMaxCents = 1900.f;

  // Full-knob gain ceiling. The Osc applies:
  //   chord_gain = kMixChord * lvl / (1 + lvl)
  // so at lvl=1 (100%) the voice reaches kMixChord, and the main 2-osc
  // mix is never pushed into the saturator's clipping region.
  static constexpr float kMixChord = 0.3f;

  // ---- State (per-sample, owned by the Osc) ----
  float phasor;       // phasor state [0, 1)
  float w0;           // phase increment (recomputed at K-rate)
  float cents;        // current interval in cents (0..kMaxCents)

  // ---- Setup (K-rate: called from setPitch / setParameter(CINT)) ----
  // Recompute the phase increment from the base w0 and the interval in cents.
  // Uses Detune::factor() (4th-order Taylor, < 0.03 cent error over +/-100c;
  // at 1900c the error is ~0.5 cent — inaudible for a drone chord voice).
  void update(float base_w0, float cents_val)
  {
    cents = (cents_val < 0.f) ? 0.f : (cents_val > kMaxCents) ? kMaxCents : cents_val;
    w0 = base_w0 * Detune::factor(cents);
  }

  // ---- Audio path (called once per sample from Osc::process) ----
  // Advances the phasor by one sample. Returns false if the interval is 0
  // (unison — the caller can skip the wavetable lookup to save a lookup,
  // though at 0 cents the chord voice is inaudible anyway if CHORD=0).
  bool advance()
  {
    if (cents <= 0.f)
      return false;
    phasor += w0;
    if (phasor >= 1.f)
      phasor -= 1.f;
    return true;
  }

  // Reset the chord voice state.
  void reset()
  {
    phasor = 0.f;
    w0 = 0.f;
    cents = 0.f;
  }
};