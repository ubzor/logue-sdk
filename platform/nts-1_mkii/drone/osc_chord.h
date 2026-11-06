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
 *  Level (CHORD knob, 0..100%) is applied by the Osc with a LINEAR gain
 *    gain = p.chord_level (0..1)
 *  then shaped by the chord's OWN folder+saturator pass (driven by the
 *  chord's own CHSAT/CHFLD knobs — independent of the A+B SATU/FOLD; ASYM is
 *  shared) and scaled by Osc::kMixChordDirect in the final mix (the chord is
 *  mixed in parallel with the A+B output so the MAIN knob ducks only A+B).
 *
 *  The interval is recomputed at K-rate (setParameter(CINT) / setPitch)
 *  using a 6th-order Taylor series for 2^(cents/1200) (see osc_detune.h
 *  for the same approach). The audio path is one multiply.
 *
 *  No dynamic allocation: all state lives in the struct (owned by Osc).
 *
 */

#include "osc_detune.h" // Detune::factor() — 2^(cents/1200) Taylor
// fast_floorf() (modular phasor wrap in advance()) is declared in osc.h; this
// header is always #included from osc.h after fast_floorf is defined, so no
// <math.h> is needed here (we must avoid libm floorf to stay free of unresolved
// dynamic symbols — see fast_floorf in osc.h).

struct ChordVoice
{
  // Maximum interval (cents) at CINT = 100%. 1900 = octave + perfect fifth.
  // 0% = 0 cents (unison), 100% = 1900 cents.
  static constexpr float kMaxCents = 1900.f;

  // (Historical) Full-knob gain ceiling from when the chord shared the A+B
  // saturator: chord_gain was kMixChord * lvl / (1 + lvl), so the chord could
  // not push the shared mix into clipping. That path is gone — the chord now
  // has its OWN saturator and uses a LINEAR gain (chord_gain = p.chord_level),
  // scaled by Osc::kMixChordDirect in the final mix. This constant is no longer
  // used by the audio path; kept only so external references don't break.
  static constexpr float kMixChord = 0.3f;

  // ---- State (per-sample, owned by the Osc) ----
  float phasor;       // phasor state [0, 1)
  float w0;           // phase increment (recomputed at K-rate)
  float cents;        // current interval in cents (0..kMaxCents)

  // ---- Setup (K-rate: called from setPitch / setParameter(CINT)) ----
  // Recompute the phase increment from the base w0 and the interval in cents.
  // Uses Detune::pow2f() (exact 2^x, libm-free) so the interval is correct
  // across the FULL 0..1900 cent range: the old 4th-order Taylor (factor())
  // only converges near 0 cents and was off by ~307 cents at 1900 c, which
  // made the upper end of the CINT / CINT2 knobs sound a minor sixth flat.
  void update(float base_w0, float cents_val)
  {
    cents = (cents_val < 0.f) ? 0.f : (cents_val > kMaxCents) ? kMaxCents : cents_val;
    w0 = base_w0 * Detune::pow2f(cents * Detune::kCentsToX);
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
    // Modular wrap (x - fast_floorf(x)) instead of a single subtract: keeps the
    // phasor in [0, 1) for ANY finite input. The chord voice is not
    // cross-FM-modulated today (w0 is always in (0, 1)), so this is a no-op
    // in practice — but it makes the invariant explicit and future-proof
    // against any future modulation that could push the phasor outside
    // [0, 1) (mirroring the main-voice phasor wrap in osc.h).
    phasor -= fast_floorf(phasor);
    return true;
  }

  // Reset the chord voice state. Only the phasor / cached phase increment are
  // cleared here — the CINT interval (cents) now lives in
  // OscParamsData::chord_cents (osc_params.h), so the single place to change
  // the CINT default is OscParamsData::reset().
  void reset()
  {
    phasor = 0.f;
    w0 = 0.f;
  }
};