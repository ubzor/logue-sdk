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

// ---- Phase-FM (FM1 / FM2): classic phase modulation --------------------------
// FM1/FM2 modulate the CARRIER PHASOR directly: the
// modulator's wavetable output (prev_wav_b_ for FM1, prev_wav_a_ for FM2) is
// multiplied by 0.5 and the depth (static part) and added to the
// carrier's per-sample phase increment. This produces the classic FM harmonic
// sidebands (a Bessel-expanded partial set). The
// depth is in CYCLES (peak phase deviation = kFmMaxCents / 2 at 100% knob,
// after the 0.5 normalization and the kFmCurveExp curve), so the sideband
// spread is frequency-independent (no low-end "howl"). The static FM index is
// NOT gated by RATE — turning FM on gives constant sidebands immediately.
// RFM_A/RFM_B add a random-walk "breathing" on top (RATE-gated). At FM=0 the
// FM is off and the stock sound is unchanged.
  // Maximum phase-FM index (peak phase deviation in cycles, before the 0.5
  // normalization) at 100% knob. kFmMaxCents / 2 = 0.05 cycles (5% of the
  // carrier) is a clearly-audible FM "color" without going into the harsh
  // "screech/whistle" region that a full index (>0.1 cycle) produces. The
  // knob sweeps 0..kFmMaxCents through the kFmCurveExp (quadratic) depth
  // curve, so the low end is a near-silent "breathing" that opens up smoothly
  // near the top.
  static constexpr float    kFmMaxCents   = 0.1f;        // full FM index (0..0.05 cycles)
  // One-pole low-pass coefficient (per sample) for the FM modulation INDEX.
  // A small alpha (~0.02 @48 kHz => ~0.3 ms time constant) rounds off the
  // fast sign-flips of the wavetable output that feed the FM index, so the
  // upper (harsh) sidebands are tamed while the slow "breathing" FM is kept.
  // Applied to the index itself (not the audio output), so the carrier is not
  // blurred. When FM = 0 the index is 0 and the filter is a no-op.
  static constexpr float    kFmSmoothAlpha = 0.02f;
  // Phase-FM depth curve: quadratic so the lower
  // part of the knob is a very subtle "breathing" and the harsh sidebands
  // only open up near the top.
  static constexpr float    kFmCurveExp    = 2.0f;

// ---- Phase jitter (JIT): random phase wobble on A/B -------------------------
// The JIT knob adds a per-sample random offset (white() - 0.5) * jit to each
// voice's phasor increment, "smearing" the carrier into a dense set of
// partials (the carrier's harmonics spread into a quasi-noise band). The
// effect is frequency-DEPENDENT (unlike FM, which is cycles-based and
// frequency-independent): at low notes the jitter is inaudible (a 0.005-cycle
// wobble on a 50 Hz carrier is a 0.25 Hz wobble, below audibility), while at
// high notes it becomes an audible "spread". This is the desired drone
// behavior — the low end stays clean, the top end "blooms". jit_cycles is the
// PEAK phase deviation in cycles (0..kJitMaxCycles); the per-sample offset is
// (white() - 0.5) * jit_cycles, so the RMS is jit_cycles / sqrt(12).
  // Maximum phase-jitter peak (cycles) at 100% knob. 0.005 = 0.5% of the
  // carrier cycle: at 500 Hz that is a +/-2.5 Hz wobble (clearly audible
  // "spread"), at 50 Hz a +/-0.25 Hz wobble (inaudible, stays clean).
  static constexpr float    kJitMaxCycles = 0.005f;

// ---- Ring-mod (RMOD): A*B parallel voice ------------------------------------
// The RMOD knob computes (wav_a * wav_b) * ring_mix and adds it to the final
// mix as a PARALLEL branch (like the chord), NOT inserted into the A+B
// saturator chain. Rationale: A*B is a spectral "difference" (the sum of the
// two wavetables' partials, shifted by +/- each other's frequencies) — it
// adds metallic "ring" and "beating" without disturbing the A+B fundamental.
// If RMOD were inserted into the A+B mix, at 100% the fundamental would
// vanish (A*B has no fundamental, only sidebands), which is not a drone.
// As a parallel voice, RMOD=100% adds a full-level metallic "ring" on top of
// the intact A+B tone.
  // Ring-mod parallel-voice scale. |wav_a * wav_b| <= 1, so the branch peak
  // is kMixRing * ring_mix (<= kMixRing). 0.5 matches kMixOsc so a full RMOD
  // sits near the A+B level (not louder, not quieter).
  static constexpr float    kMixRing = 0.5f;

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
  // (kMixRing for the RMOD parallel voice is defined in the RMOD block above,
  // next to kJitMaxCycles / kFm*, so the enrichment constants stay together.)

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
