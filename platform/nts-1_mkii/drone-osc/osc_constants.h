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

// ---- FM modulator C: phase-FM on the (A+B) SUM (the drone carrier) --------
// C is an INDEPENDENT low-frequency oscillator (the modulator) that bends the
// phase of the (A+B) sum (the carrier). Classic FM: C is NOT heard by itself,
// it only produces sidebands on the carrier. This is a "vibrato / motion"
// layer on top of the (untouched) cross-FM A+B timbre.
//
// FM_CR (rate): a LOG 0..1 Hz LFO-style knob. raw 0 = OFF (0 Hz, the sum is
// unmodulated), raw 1023 = 1 Hz (full vibrato). The log scale (3 decades, like
// the wave-LFO rate knobs) gives fine control near 0 and a full vibrato at the
// top. 1 Hz is the ceiling: above it, on a low drone note, a SAW/SQR modulator
// would start to be heard as its OWN tone (beating with the carrier), so 1 Hz
// keeps C firmly in the "vibrato" range rather than "buzzing".
  static constexpr float    kFmCRateMaxHz  = 1.0f;   // FM_CR raw 1023 -> 1 Hz
  // FM_CD (depth): LINEAR 0..100% -> 0..kFmCMaxCycles peak phase deviation on
  // the sum. 0.05 cycles (5% of the carrier) is a clearly-audible vibrato /
  // motion without going into the harsh "screech" region (a full >0.1-cycle
  // index is too aggressive for a slow drone vibrato). Applied as a PHASE
  // offset on the sum (sample-and-hold re-timed by C), so the carrier itself is
  // not blurred (unlike the kFmSmoothAlpha index low-pass).
  static constexpr float    kFmCMaxCycles  = 0.05f;  // FM_CD 100% -> 0.05 cycles
  // ---- FM modulator S (static teeth): a carrier-locked FM oscillator --------
  // S is an oscillator whose RATE is LOCKED to the (A+B) carrier: it runs at
  // N teeth per carrier cycle (FMST sets N, log-scaled kFmStaticMinTeeth..kFmStaticMaxTeeth).
  // Its output phase-FMs the (A+B) sum, drawing N "teeth" of phase deviation
  // per carrier cycle -- a STATIC harmonic comb (vibrato at a multiple of the
  // note frequency, so it never drifts out of tune with the note). This is the
  // static counterpart of C (which is a free-running low-frequency vibrato).
  //
  // FMST (teeth): LOG 0.05..kFmStaticMaxTeeth teeth per carrier cycle. raw 0 =
  // OFF (0 teeth, the sum is unmodulated -- regression-safe). The log scale
  // (4 decades) gives FINE control below 1 tooth (sub-octave: fm/fc < 1) up to
  // a dense comb at the top (16). Teeth < 1 = the S modulator runs SLOWER than
  // the carrier (fm/fc < 1) -- a deep, organ-like comb with sidebands that dip
  // below the carrier; 1 tooth = in-tune (fm/fc = 1).
  static constexpr float    kFmStaticMaxTeeth = 16.0f; // FMST raw 1023 -> 16 teeth/cycle
  static constexpr float    kFmStaticMinTeeth = 0.05f; // FMST raw 1   -> 0.05 teeth/cycle (sub-octave)
  // FMSA (amplitude): LINEAR 0..100% -> 0..kFmStaticMaxCycles peak phase
  // deviation (the "size"/height of each tooth). 0.05 cycles (5% of the
  // carrier) is a clearly-audible comb without harsh screech, matching the C
  // modulator's kFmCMaxCycles. At FMSA = 0 (or FMST = 0) S is off (bit-identical).
  static constexpr float    kFmStaticMaxCycles = 0.05f; // FMSA 100% -> 0.05 cycles peak

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

// (The sub-octave / sub-2 "sub bass" constants -- kSubDetuneCents,
//  kSubLfoFmCents, kSubAnchorAlpha, kSubBreathMax/TauSec/Floor, kMixSub,
//  kMixSub2 -- and kSubW0Scale (below) are REMOVED: the SUB1 (f/2) / SUB2 (f/4)
//  sub voices are gone. The low end now lives INSIDE the WTE wavetable as
//  subharmonic partials (the f/4 source in wte_default_wave, osc_wte_doc.h),
//  so there is no separate bass branch, anchor, breath gain, or mix gain.)

// ---- Final-mix gains ----------------------------------------------------------
// Worst-case peaks (see the budget comment in process()): the A+B branch is
// the only main branch now (the sub/sub2 low-end branches are gone -- the low
// end is in the WTE wavetable), plus the RMOD derived voice.
// (kMixChordDirect was removed — the chord/interval concept is gone.)
  // A+B mixed output (pre-saturator) scale.
  static constexpr float    kMixOsc  = 0.5f;
  // (kMixSub / kMixSub2 removed with the sub voices -- the low end is in the
  //  WTE wavetable now.)
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
  // (kSubW0Scale removed with the sub voices -- the sub-octave phasor is gone;
  //  the low end now lives inside the WTE wavetable as subharmonic partials.)
  // (Note glide / portamento removed: kGlide* constants and the kTwoPow 2^i
  //  LUT are gone. Each cluster note now starts at its exact pitch with a
  //  random per-voice start phase — no glide, no per-sample w0 slide.)



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
  // WAV1 (WAVE) is shown as the bank letter A..F. WAV2 (deprecated) shows 0.
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
  // (Wavetable-morph stream (Timbre) RW_WAV / kRwMorphScaleTables removed:
  //  the intra-bank LFO sweep (WaveTable::resolve_wave + lfo_morph in
  //  osc_mix.h) now handles the timbre movement. The rw_[1] stream still
  //  drives the SVF cutoff (RWFC) — see kRwCutoffHalfOctaves below.)
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

// (The CONSTANT SIDECHAIN DUCK -- kDuckConstant / kDuckFollowTauSec /
//  kDuckFloor / kDuckScale -- is REMOVED with the SUB1/SUB2 sub voices: there
//  is no separate low-end branch to duck the MAIN voice against (the low end
//  now lives inside the WTE wavetable), so the main + RMOD mix is un-ducked.)

// -----------------------------------------------------------------------------
// Polyphonic-cluster LOUDNESS compensation (Variant A; see osc_mix.h)
//
// The cluster sums up to kVoiceCount notes into ONE shared post-stage. Without
// compensation N simultaneous notes add coherently (worst case ~xN) and, fed
// through fold/saturate at the same FOLD/SATU, clip and "shout". The summed
// PER-VOICE voice signals (sum_main / sum_ring) are divided by env_sum [one-pole
// smoothed, the kClusterCompAlpha one-pole] -> -6 dB/voice (linear 1/N makeup).
// A single note is EXACTLY unchanged (1/1 = 1). NOTE: the shared sub bass (the
// ONE SUB1/SUB2 pair) is NOT env_comp_ scaled — it is not a per-voice sum, so
// there is no N-fold growth to compensate; it uses env_sum_ only as the
// denominator of its env-weighted ANCHOR mean (see the "Shared sub bass" block
// in osc_mix.h).
// The per-voice A-H-R envelope is applied per-voice (each voice gated by its
// own v.eg.env in the accumulation loop), NOT as a common output envelope --
// this is what stops the 1->2 note boundary from dipping the whole cluster.
// kClusterCompAlpha is the one-pole coefficient for the smoothed 1/N (env_comp_):
// tau ~5 ms, fast enough to follow note count / the 2 s envelope without
// per-sample pumping, slow enough not to ring on a fast note-on burst.
// -----------------------------------------------------------------------------
  static constexpr float    kClusterCompAlpha = 0.0041658f; // 1 - exp(-1/(0.005s*48kHz)); one-pole tau ~5 ms (was 50 ms, too slow for note-on transients -> perceived as "quieter")
