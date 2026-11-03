#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_constants.h
 *
 *  Static tuning constants for the DRONE unit's Osc class (see the in-file
 *  comments under each osc_consts namespace block for full rationale).
 *
 *  These were formerly `static constexpr` members of the Osc class in osc.h.
 *  They are now grouped in a nested `inline namespace osc_consts` so the
 *  class body stays small (osc.h is the thin skeleton; the heavy method
 *  bodies live in osc_param_apply.h / osc_param_display.h / osc_mix.h).
 *
 *  They are plain namespace-scope constants (not members) so the out-of-line
 *  Osc method bodies (which are #included at the BOTTOM of osc.h, after the
 *  class is complete) can reference them as osc_consts::k*. The class body
 *  itself still uses the unqualified names via the `using` aliases inside Osc.
 *
 *  Everything is constexpr with no code/data emission, so moving them here
 *  has zero runtime/size cost.
 */

// NOTE: this file intentionally does NOT include osc.h. The constants below
// are self-contained (no class/enum references), and including osc.h here
// would create a circular include (osc.h includes osc_constants.h BEFORE
// defining `class Osc`). osc.h #includes this file, then pulls the constants
// into the class scope with `using namespace osc_consts;`.

// ---- Cross-modulation (classic dual-FM, the "FM synth" flavor) --------------
// Cross-FM is a SEPARATE mechanism from the random walk: where the walk
// wanders (slow, drifting targets over 0.1..10 s), cross-FM is a STATIC
// modulation -- osc B's wavetable output (wav_b) is multiplied by the XFM
// knob and fed into osc A's PHASOR (and symmetrically A into B for XFM2).
// Because the modulator oscillates at the audio frequency, this produces the
// characteristic FM harmonic sidebands (a classic dual-FM pair) rather than a
// slow beat. The depth is in wavetable INDEX units (not cents): wav_a is
// already a position across the 91-table bank, so adding
// (XFM_A * wav_b) directly modulates that position by up to
// kXfmMaxCents * 1.0 (wav_b is bounded in [-1, 1]).
//   XFM1 (XFM_A) -> B modulates A's phasor
//   XFM2 (XFM_B) -> A modulates B's phasor
// At both = 0 (default) the classic FM is OFF and the sound is unchanged.
// The modulation is applied in Osc::process() using the PREVIOUS sample's
// wav_b (a one-sample lookback), which keeps the phasor integration stable
// (no feedback of the current sample) and adds one sample of phase --
// inaudible at audio rates.
  // Maximum cross-FM index shift at 100% knob (wav_b is in [-1, 1], so the
  // phasor is modulated by +/-kXfmMaxCents tables). 10 tables is a gentle,
  // clearly-audible sideband spread without going into the dense aliasing
  // region of the banks; the knob sweeps 0..10 linearly from the raw value
  // (1023 -> 10.0).
  static constexpr float    kXfmMaxCents = 0.8f;        // full cross-FM index (0..0.4 cycles)

// ---- Sub-octave sine (low-end body) ------------------------------------------
// The sub voice is a fixed sine one octave below the base note (w0/2). It is
// slightly detuned below the true octave (-4 cents) so it does not comb-filter
// against the fundamental, and its PHASOR is FM'd by the platform shape-LFO
// (setShapeLfo, L-rate) at up to kSubLfoFmCents cents, giving a slow, organic
// "swell" to the low end. It is mixed in parallel with the saturator (after
// the A+B mix), so it adds body without feeding the drive stage.
  // Fixed detune of the sub-octave sine (cents). Slightly below -4 c so the
  // sub sits just under the fundamental instead of combing with it.
  static constexpr float    kSubDetuneCents = -4.f;
  // Maximum phase-FM depth applied to the sub phasor by the shape-LFO (cents).
  // The LFO value is (-1, 1), so the sub phasor is modulated by
  // lfo * kSubLfoFmCents * TWO_PI / 1200 radians/sample -> a gentle, slow
  // pitch wobble. 5 c keeps the low end musical (no warble).
  static constexpr float    kSubLfoFmCents = 5.f;

// ---- Final-mix gains ----------------------------------------------------------
// Worst-case peaks (see the budget comment in process()):
//   A+B saturator out * kMixOsc < 0.5
//   CHORD saturator out * kMixChordDirect < 0.6
//   SUB1 * kMixSub < 0.5
//   SUB2 * kMixSub2 < 0.4
// against the driven A+B voice); CHORD (kMixChordDirect) is ~0.8 so the
// three-voice cluster (A+B, chord, sub) lands in the -12dBFS .. 0dBFS
// dynamic range without ever over-driving the final hard clip (which only
// triggers when the branches peak simultaneously).
  // A+B mixed output (pre-saturator) scale.
  static constexpr float    kMixOsc  = 0.5f;
  // Sub-octave sine (parallel) scale.
  static constexpr float    kMixSub  = 0.5f;
  // Second sub (f/4) scale.
  static constexpr float    kMixSub2 = 0.4f;
  // Chord voice (parallel) scale. The chord runs through its OWN
  // folder+saturator pass, so this gain sits after the chord's saturator
  // (bounded in (-1, 1)). 0.8 makes the chord a comparable "partner" to the
  // peak of ~0.4-0.6 — comparable to A+B (kMixOsc=0.5 over two voices) and
  // the SUB1 sub (kMixSub=0.5 * 0.5 level). The chord's own CHRD knob scales
  // the input to its saturator, so users can ride it up or down from here.
  static constexpr float    kMixChordDirect = 0.8f;

// ---- Pitch / frequency constants ---------------------------------------------
  // Platform sample rate (NTS-1 mkII: 48 kHz). Hard-coded: the Processor base
  // class is a forward-declared abstract interface (processor.h) in this build
  // whose getSampleRate() definition lives in the platform runtime the unit
  // does not link against. If the platform ever changes sample rate, update
  // this constant.
  static constexpr float    kSampleRate   = 48000.f;
  // Reference frequency for the MIDI note -> w0 conversion (A4 = 440 Hz).
  static constexpr float    kRefHz        = 880.f;           // A4
  // 2*pi for phasor increments and the sub sine.
  static constexpr float    kTwoPi        = 6.283185307179586f;
  // Precomputed scale factor for the sub-octave phasor:
  //   w0_sub_ = w0_ * kSubW0Scale  (== w0/2 * 2^(kSubDetuneCents/1200))
  // so the audio path does not recompute 2^(-4/1200) on every setPitch().
  static constexpr float    kSubW0Scale   = 0.4988460883f; // 0.5 * 2^(-4/1200)

// ---- Display scaling (raw 10-bit <-> DSP float) -------------------------------
// Single source of truth for the ranges each editable parameter maps onto,
// used by BOTH apply_edit_param() (raw -> DSP float) and
// getParameterStrValue() (raw -> display string). Keep in sync with the
// web simulator (websim/osc.html formatParamValue) and the default raw
// values in header.c / edit_value_ below.
  // Detune is shown as integer cents (-10..+10) with a '+' for positive.
  // ASYM / RW_* / FOLD / CHRD / SUB1 / SUB2 are shown as 0..100%. SATU is
  // shown as a bare 0..300 number (raw*300/1023, 100 == 1.0x gentle drive).
  // WAV1 / WAV2 / WAV3 are shown as bare 0..(kWavesTotal-1) numbers.
  static constexpr float    kDetuneMaxCents = 10.f;  // sum-voice detune: +/-10 c
  static constexpr float    kSatuMax        = 3.f;   // 0..3.0 drive level
  static constexpr int32_t  kRawMid         = 511;   // raw 10-bit value == 0%
  static constexpr int32_t  kRawMax         = 1023;  // raw 10-bit value == 100%

// ---- Random-walk wandering modulation (see osc_random_walk.h) ----------------
// The random walk drives a per-stream "wander amount" (rw_rate[i]/rw_smooth[i])
// that modulates three independent streams. Each stream's deviation (dev_a/dev_b,
// normalized [-1, +1]) is scaled here (at the audio path) onto a real DSP
// parameter. The scaling constants below define the MAXIMUM effect at 100%
// depth (RW_WAV / RW_DRV / RW_FLD = 100%); the shared RATE knob (rw_amount,
// quadratic-mapped) is the overall ceiling for every stream, so RATE = 0
// disables all wandering regardless of the per-target knobs.
  // Detune stream (Tone): the walk moves A and B by +/-kRwDetuneScaleCents
  // cents on top of the static DETUNE knob. The two voices get independent
  // random targets, so the inter-voice beat frequency drifts over time (an
  // asymmetric chorus).
  static constexpr float    kRwDetuneScaleCents = 10.f; // +/-10 cents at RW depth 100%
  // Wavetable-morph stream (Timbre): the walk nudges the A and B wavetable
  // morph positions by +/-kRwMorphScaleTables tables (clamped to the 0..90
  // bank range in Osc::process), so the timbre slowly drifts through the
  // banks A-F.
  static constexpr float    kRwMorphScaleTables = 10.f; // +/-10 tables at RW_WAV 100%
  // Drive stream (Density): the walk moves the saturator drive by
  // +/-kRwDriveScale (clamped to 0..kSatuMax in the audio path), and the
  // wavefolder depth by +/-kRwFoldScale (clamped to 0..1 in the audio path),
  // so the "density" / harmonic bite of the drone breathes over time.
  static constexpr float    kRwDriveScale = 0.5f;       // +/-0.5 drive at RW_DRV 100%
  static constexpr float    kRwFoldScale = 0.5f;        // +/-0.5 fold at RW_FLD 100%
