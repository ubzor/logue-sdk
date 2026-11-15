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

// ---- Ring-mod (RMOD): A*B derived voice (parallel, MAIN-ducked) -------------
// The RMOD knob computes (wav_a * wav_b) * ring_mix and adds it to the final
// mix as a PARALLEL branch (like the chord), NOT inserted into the A+B
// saturator chain. Rationale: A*B is a spectral "difference" (the sum of the
// two wavetables' partials, shifted by +/- each other's frequencies) — it
// adds metallic "ring" and "beating" without disturbing the A+B fundamental.
// If RMOD were inserted into the A+B mix, at 100% the fundamental would
// vanish (A*B has no fundamental, only sidebands), which is not a drone.
// As a parallel voice, RMOD=100% adds a full-level metallic "ring" on top of
// the intact A+B tone.
//
// However, unlike the chord (an independent voice), RMOD is a DERIVED voice
// of A+B — it is literally the product of the two main wavetable outputs — so
// it is also scaled by the MAIN level knob (p.main_level, see the final mix
// in osc_mix.h): MAIN=0 silences the whole main-voice cluster (A+B + its
// FM/JIT/RMOD byproducts) at once. Before this, MAIN only ducked y_filt and
// RMOD>0 kept the metallic tone audible even with the "muted" main voice —
// the reported bug. FM A / FM B do not raise RMOD's amplitude: they modulate
// the A/B phasors (shape), so they only reshape the ring spectrum.
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
//   SUB1 * kMixSub < 0.5
//   SUB2 * kMixSub2 < 0.4
// against the driven A+B voice); the sub-voice cluster lands in the -12dBFS
// .. 0dBFS dynamic range without ever over-driving the final hard clip
// (which only triggers when the branches peak simultaneously).
// (kMixChordDirect was removed — the chord/interval concept is gone.)
  // A+B mixed output (pre-saturator) scale.
  static constexpr float    kMixOsc  = 0.5f;
  // Sub-octave sine (parallel) scale.
  static constexpr float    kMixSub  = 0.5f;
  // Second sub (f/4) scale.
  static constexpr float    kMixSub2 = 0.4f;
  // (kMixChordDirect was removed — the chord/interval concept is gone.)
  // (kMixRing for the RMOD parallel voice is defined in the RMOD block above,
  // next to kJitMaxCycles / kFm*, so the enrichment constants stay together.)

// ---- Output normalization (post-EG, pre-out) ----------------------------------
// The per-branch kMix* gains above are a PEAK BUDGET (they keep the worst-case
// sum under the final limiter), NOT a loudness target: with every knob at
// default the summed peak lands around 0.35-0.5, i.e. roughly -12 dBFS --
// clearly quieter than the platform's reference oscillators (which apply a
// ~+3 dB pre-tanh gain and read near 0 dBFS). kOutDrive is the makeup gain
// that lifts the whole mix back to a comparable level, and kOutSat feeds that
// lifted sum into the SAME tanh soft-clip the reference units use (see
// platform/nts-1_mkii/waves/osc.h: sig *= 1.4125375446227544f; fastertanh2f).
// tanh(1.4125 * x) ~= 10^(0.5/20) * x in its near-linear region, so the two
// constants compose into a single effective output gain of ~10^(13/20)
// (2.512, +8 dB) while the tanh rolls off smoothly above it instead of the
// old hard clip (no square-wave edge when all five branches peak together).
// Tune kOutDrive (not the kMix* budget) if A/B against other oscillators
// still reads too quiet / too loud.
  // Makeup gain for the whole mix (2.512 ~= 10^(8/20) = +8 dB).
  static constexpr float    kOutDrive = 2.512f;
  // Pre-tanh drive (same constant the reference waves unit uses).
  static constexpr float    kOutSat   = 1.4125375446227544f;

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
  // ---- Note glide (portamento) — GRIMM-style EXPOENTIAL ----------------------
  // When a new cluster note starts, its per-voice w0 does NOT jump: it slides
  // from the cluster's LAST note to its own pitch. The slide is EXPOENTIAL
  // (fast at first, then easing in) — the GRIMM GLIDE behaviour. The per-sample
  // step is a fixed Q16 fraction of the distance LEFT (like GRIMM's
  // vc_gl: d = k * x / 65536; x -= d), so the remaining distance shrinks
  // geometrically and the voice converges on the target. gl_k_ (Osc::gl_k_,
  // recomputed at K-rate from the GLIDE edit parameter) is that Q16 fraction:
  //   gl_k_ = kGlideNum / (t_ms / kGlideMsStep)   (clamped to [kGlideStepMin,1])
  // where t_ms is the GLIDE time (kGlideMinMs .. kGlideMaxMs, a quadratic
  // mapping of the knob) and kGlideMsStep (0.726 ms = one 32-sample control
  // block @48 kHz) is the step granularity. GLIDE = 0 (raw 0) disables the
  // glide (the note jumps straight to its pitch, regression-safe). The snap
  // threshold is kGlideSnap (1/4096-octave units, the same unit as gl_want_).
  static constexpr float    kGlideMinMs   = 10.f;    // raw 1   -> ~10 ms (a quick scoop)
  static constexpr float    kGlideMaxMs   = 2500.f;  // raw 1023-> ~2.5 s (a slow siren)
  static constexpr float    kGlideMsStep  = 0.726f;  // one 32-sample block @48 kHz
  static constexpr uint32_t kGlideNum     = 328516U; // 6.9 * 47618 (numerator, pre-scaled)
  static constexpr uint32_t kGlideStepMin = 1U;      // floor so the glide always finishes
  static constexpr int32_t  kGlideSnap    = 11;      // |x| < 11 (1/4096-octave) -> snap (~0.03 st)
  // (kGlideSamples was removed: the fixed 1 s LINEAR glide is gone, replaced by
  //  the knob-set EXPOENTIAL GRIMM glide above.)
  // 2^i LUT for the GLIDE pitch computation (osc_mix.h). Indexed by (i + 12)
  // for i ∈ [-12, +12] (±12 semitones). A `static const` LOCAL inside the
  // audio callback was a data race on first-touch initialization under the
  // WASM build's -matomics / SharedArrayBuffer (two voices / two blocks could
  // first-read the table concurrently before it was fully initialized), which
  // intermittently produced a corrupt vw0 -> silence on the 2nd/3rd note.
  // Making it a namespace-scope constexpr (this header) guarantees single,
  // pre-thread initialization.
  static constexpr float kTwoPow[25] = {
      0.000244140625f, // 2^-12
      0.00048828125f,  // 2^-11
      0.0009765625f,   // 2^-10
      0.001953125f,    // 2^-9
      0.00390625f,     // 2^-8
      0.0078125f,      // 2^-7
      0.015625f,       // 2^-6
      0.03125f,        // 2^-5
      0.0625f,         // 2^-4
      0.125f,          // 2^-3
      0.25f,           // 2^-2
      0.5f,            // 2^-1
      1.f,             // 2^0
      2.f,             // 2^1
      4.f,             // 2^2
      8.f,             // 2^3
      16.f,            // 2^4
      32.f,            // 2^5
      64.f,            // 2^6
      128.f,           // 2^7
      256.f,           // 2^8
      512.f,           // 2^9
      1024.f,          // 2^10
      2048.f,          // 2^11
      4096.f           // 2^12
  };



  // (kChordFadeSamples / kChordCrossSamples were removed — the chord/interval
  //  concept is gone; chord interval fades/crossfades no longer exist.)

 // ---- Rnd re-roll morph (BPM-synced smooth transition) -----------------------
 // When a Rnd re-roll fires (All / Part / Orbi), the affected parameters do NOT
 // jump to their new values: they morph from the OLD raw value to the NEW one,
 // each with its own START DELAY (stagger, so the params do not all move at
 // once) and its own SMOOTHSTEP curve (3t^2 - 2t^3) over the morph duration.
 // The morph is clocked by the tempo (setTempo -> tempo_bpm_, default 120):
 //   * total morph length = kRndMorphBeats beats (default 120 BPM -> 8 s,
 //     i.e. 4 bars of 4/4). A new fire while a morph is running re-anchors the
 //     in-flight params to the CURRENT interpolated value (no jump).
 //   * stagger: the per-parameter start delay is a random 0..N subdivisions of
 //     the morph length (N = kRndMorphStaggerDivs), so the params "roll in"
 //     one after another across the whole phrase.
 // Discrete params (WA1/2/3, INT/INT2) morph in their own native space for
 // free: a fractional wavetable index is crossfaded by resolve_wave() (already
 // interpolates between adjacent tables). (chord cents glide was removed — the
 // chord/interval concept is gone.)
 // The breathing (RATE walk streams) is untouched: its targets regenerate on
 // their own (0.1..10 s) periods, so morphing RATE/SMTx is inaudible.
 // The morph duration / stagger are computed ONCE at fire time from the
 // current tempo (a tempo change mid-morph does not re-aim an in-flight
 // morph -- it only affects the NEXT fire).
 // These constants are self-contained (no enum/class references), so this
 // block may live in osc_constants.h, which does NOT include osc.h.
  static constexpr uint32_t kRndMorphBeats       = 1U * 4U;  // 1 bar of 4/4 (2 s @ 120 BPM)
  static constexpr uint32_t kRndMorphStaggerDivs = 16U;      // random start-delay granularity (0..15 of the morph length)
  static constexpr float    kRndMorphBpmMin      = 40.f;     // tempo clamp (48 s morph = still slow but not absurd)
  static constexpr float    kRndMorphBpmMax      = 240.f;    // tempo clamp (1 s morph)

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

// ---- Enrichment-2: formant / comb / fold2 / ducking ---------------------------
// New DSP blocks added in the enrichment-2 pass. All OFF by default (the
// corresponding params_ fields are 0), so the stock sound is bit-identical
// until the user turns one on. No dynamic allocation: every array is a fixed
// member (the comb ring buffer is kCombDelayMaxSmp floats = 1 KB of RAM).
// (Tape wow/flutter were removed — the synchronous whole-voice pitch wander
//  was inaudible against the drone's inherent DETUNE beating.)

// ---- Comb / resonator (feedback through a short delay) -----------------------
// The COMB knob sets the feedback coefficient of a one-pole IIR comb (a sample
// delay line with its output summed back around a feedback loop). A single
// feedback tap produces a resonant peak at ~1/delay-samples Hz and its
// harmonics -- a "wooden / cavernous" resonance that tints the main A+B voice.
// The delay is a FIXED mid-band length (kCombDelaySmp, ~5.2 ms -> ~190 Hz peak);
// the knob sweeps the feedback 0..kCombFbMax (clamped < 1 for stability). At
// COMB = 0 the branch is a pure wire (regression-safe). The ring buffer is a
// fixed member array sized to kCombDelayMaxSmp (never dynamically allocated).
  static constexpr float    kCombFbMax     = 0.9f; // feedback ceiling (< 1 = stable)
  static constexpr uint32_t kCombDelaySmp  = 250U; // fixed delay (~5.2 ms -> ~190 Hz peak)
  static constexpr uint32_t kCombDelayMaxSmp = 256U; // ring-buffer size (pow-of-2, >= delay)

// ---- Formant filter (5 parallel band-pass biquads, vowel morph) --------------
// The FORM knob selects a VOWEL POSITION (0..1) across the five formant stops
// A-E-I-O-U; the FVOL knob sets the PARALLEL-ADD gain of the formant stage
// (0 = fully dry / transparent, regression-safe). Each vowel is modelled by
// TWO parallel one-pole IIR comb resonators (its F1 + F2 formant frequencies,
// from kFormantStops: the comb delay = sr/(2*f), so the resonance peak sits at
// f/2 -- a musical sub-octave band). The K-rate path (recompute_formant_comb)
// linearly crossfades the (F1, F2) pair between the two nearest stops, so
// morphing FORM slides the vowel smoothly A -> U; the per-sample path (only
// when FVOL > 0) runs both combs on the pre-sub main output and ADDS the
// (scaled) sum of their feedback colors ON TOP of the intact main signal
// (parallel branch, NOT dry/wet -- a dry/wet blend would mute the drone,
// since a formant band carries only a sliver of its energy). F2 is what makes
// the vowels distinct (I/E high F2 = bright, U/O low F2 = warm). The color
// is scaled by kFormantGain so a full FVOL=100% cannot overdrive the final
// soft-clip. Both combs are ALWAYS stable (single feedback tap, |fb| < 1).
  // Formant stops as [F1 Hz, F2 Hz] for A, E, I, O, U (approx. speech values).
  static constexpr float    kFormantStops[5][2] = {
      { 730.f, 1090.f },   // A
      { 530.f, 1840.f },   // E
      { 270.f, 2290.f },   // I
      { 450.f, 1020.f },   // O
      { 300.f,  870.f },   // U
  };
  static constexpr float    kFormantGain  = 1.0f;  // parallel-add gain (formant color on top of main)

// ---- Sidechain ducking (sub-bass -> main voice, the drone's "pump") ---------
// CONSTANT sidechain duck (formerly the DCK knob, removed from the editable
// set): the drone's body lives in the low end, so the low end must always read
// as the STABLE anchor. An envelope follower tracks the |SUB1 + SUB2 output|
// energy (a cheap one-pole abs-peak follower, computed every sample) and the
// follower then DUCKS the MAIN voice (A+B + RMOD). The sub-bass becomes the
// constant "anchor" and the main tone "sinks" out of the way of the low end,
// the classic sidechain "pump" in reverse: dense SUB1/SUB2 push the main tone
// down a few dB so the low end owns the spectrum.
//
// The duck AMOUNT is a fixed constant (kDuckConstant = 0.5) -- a clearly
// audible sidechain. The duck gain is floored at kDuckFloor (0.7, ~-3 dB) so
// the main tone only "sinks", it never disappears (a drone must keep its pitch
// anchor), and the follower's one-pole time constant (kDuckFollowTauSec,
// ~75 ms attack/release) sets how fast the sink releases -- slow enough to
// read as a smooth "breath" on the low end instead of a clicky gate.
  static constexpr float    kDuckConstant     = 0.5f;   // fixed duck amount (50%)
  static constexpr float    kDuckFollowTauSec = 0.075f; // follower time constant (~75 ms)
  static constexpr float    kDuckFloor        = 0.7f;   // duck gain floor (~-3 dB, never silent)
  static constexpr float    kDuckScale        = 1.0f;   // follower -> duck-gain scaling

// -----------------------------------------------------------------------------
// Polyphonic-cluster LOUDNESS compensation (Variant A; see osc_mix.h)
//
// The cluster sums up to kVoiceCount notes into ONE shared post-stage. Without
// compensation N simultaneous notes add coherently (worst case ~xN) and, fed
// through fold/saturate at the same FOLD/SATU, clip and "shout". The summed
// voice signals (sum_main/sub/sub2/ring) are divided by sqrt(env_sum) [one-pole
// smoothed, the kClusterCompAlpha one-pole] -> -3 dB/voice, the standard
// polyphonic-makeup rule. A single note is EXACTLY unchanged (sqrt(1)=1).
// The per-voice A-H-R envelope is applied per-voice (each voice gated by its
// own v.eg.env in the accumulation loop), NOT as a common output envelope --
// this is what stops the 1->2 note boundary from dipping the whole cluster.
// kClusterCompAlpha is the one-pole coefficient for the smoothed 1/sqrt(N)
// (env_comp_): tau ~50 ms, fast enough to follow note count / the 2 s envelope
// without per-sample pumping, slow enough not to ring on a fast note-on burst.
// -----------------------------------------------------------------------------
  static constexpr float    kClusterCompAlpha = 0.0041658f; // 1 - exp(-1/(0.005s*48kHz)); one-pole tau ~5 ms (was 50 ms, too slow for note-on transients -> perceived as "quieter" with GLIDE>0)
