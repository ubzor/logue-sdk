#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: modfx_constants.h
 *
 *  DRONE-MOD (modfx): static tuning constants for the drone modulation-effect
 *  unit. All stage gains / ceilings / LFO curves / DRIFT budget + the
 *  FORMANT/COMB constants copied verbatim from drone-osc (so the formant/comb
 *  sound is bit-identical to what it was in the oscillator, just relocated to
 *  the MOD slot).
 *
 *  IMPORTANT (check-und): the NTS-1 mkII FX runtime does NOT link libm into a
 *  unit, and the check-und target in the Makefile fails the build if any
 *  undefined (UND) libm symbol (powf / expf / sqrtf / logf / sinf / cosf /
 *  floorf ...) appears in the unit's dynamic symbol table. Every per-sample
 *  and K-rate computation in this unit therefore uses ONLY:
 *    * plain float / integer arithmetic, and
 *    * the hand-rolled one-pole / Taylor helpers (one_minus_expf, the sine
 *      LFO via a phase accumulator + Taylor sine).
 *  No transcendental calls in the audio path. The formant comb delays are
 *  computed with a float division at K-rate (init / FORM/FVOL change) -- a
 *  division is FPU, not libm -- so that is fine.
 */

#include <cstdint>

namespace modfx_consts
{
// ---- Sample-rate anchor ----------------------------------------------------
// All LFO / delay constants are precomputed at init against the fixed 48 kHz
// rate (Processor::getSampleRate()). The unit is mono-in / stereo-out.
static constexpr float kSr = 48000.f;

// ============================================================================
//  TIME (slot 0) -> LFO frequency curves
// ============================================================================
// TIME maps 0..1023 -> 0..1 (p.time). Each modulation stage derives its LFO
// frequency from a stage-specific base curve so that, at a given TIME, all the
// LFOs sweep together (a "tempo" feel) but at different rates. The curve is a
// gentle exponential ramp built from a Taylor series of exp (no libm):
//   f = f_min * (f_max / f_min) ^ p  ==  f_min * exp(lnRatio * p)
// For |lnRatio * p| <= ~3.3 a 4th-order Taylor of exp is accurate to <1e-3
// (far below audibility for an LFO frequency). We precompute lnRatio per
// stage as a plain constant (computed offline, no runtime libm).
static constexpr float kEnseLFOMin = 0.05f;  // 0.05 Hz  (TIME = 0)
static constexpr float kEnseLFOMax = 1.2f;   // 1.2 Hz   (TIME = 1)
static constexpr float kEnseLFOlnR = 3.17805f; // ln(kEnseLFOMax / kEnseLFOMin)

static constexpr float kTremoLFOMin = 0.03f;  // 0.03 Hz
static constexpr float kTremoLFOMax = 0.8f;   // 0.8 Hz
static constexpr float kTremoLFOlnR = 3.27394f; // ln(...)

static constexpr float kFlngLFOMin = 0.30f;   // 0.30 Hz
static constexpr float kFlngLFOMax = 5.0f;    // 5.0 Hz
static constexpr float kFlngLFOlnR = 2.81341f; // ln(...)

// ---- DEPTH (slot 1) -> per-stage modulation depth --------------------------
// p.depth is 0..1. Each stage scales its modulation amplitude by a stage
// constant so a full DEPTH gives a clearly audible (but not destructive)
// amount. Flanger depth is squared (DEPTH^2 * kFlngMaxDepthMs) so low DEPTH
// is a gentle shimmer and full DEPTH is a pronounced sweep.
static constexpr float kEnseSpreadSmp    = 32.0f;  // max chorus LFO delay-spread (samples, full DEPTH)
static constexpr float kTremoDepthScale  = 0.85f;  // tremolo depth (0..85% of the split)
static constexpr float kFlngMaxDepthMs   = 3.0f;   // max flanger delay (ms) at DEPTH=1 (squared)

// ---- ENSEMBLE (stereo chorus) ----------------------------------------------
// Two detuned voices (x1.0 and x1.0618 = the golden-ratio conjugate, so they
// never re-align) through short modulated delays (~18 / ~31 ms). The delay is
// modulated by a slow LFO (freq from TIME, spread from DEPTH).
static constexpr float kEnseDelayBaseMs = 18.0f;   // voice 1 base delay (ms)
static constexpr float kEnseDelay2Ms    = 31.0f;   // voice 2 base delay (ms)
static constexpr float kEnseVoice2      = 1.0618f; // voice 2 detune multiplier

// ---- TREMOLO (harmonic/optical) --------------------------------------------
// A first-order LPF/HPF split; the amplitudes are modulated in antiphase by a
// slow LFO (freq from TIME, depth from DEPTH). The LOW band is barely touched
// (kTremoLowFloor) so the sub-voice stays a constant anchor (a "sub-anchor").
static constexpr float kTremoSplitHz  = 400.f;  // LPF/HPF crossover (Hz)
static constexpr float kTremoLowFloor = 0.35f;  // low-band floor (never below this)

// ---- FLANGER (thru-zero) ---------------------------------------------------
// HPF ~800 Hz, a very short modulated delay (0.2..2.5 ms) swept by an LFO
// (x3 rate, phase-shifted), feedback from DEPTH. Thru-zero: a small fixed
// delay keeps the comb "through zero" so there is no harsh null at 0.
static constexpr float kFlngHPFHz          = 800.f; // HPF cutoff (Hz)
static constexpr float kFlngDelayMinMs     = 0.2f;  // min flanger delay (ms)
static constexpr float kFlngDelayMaxMs     = 2.5f;  // max flanger delay (ms, at full DEPTH)
static constexpr float kFlngFbMax          = 0.6f;  // max feedback (< 1 = stable)

// ---- TAPE (wow + flutter) --------------------------------------------------
// Wow: an LCG random-walk on a medium delay (~3 ms) gives slow pitch wander
// (depth = DEPTH * kTapeWowCents cents). Flutter: LCG white noise (LPF'd)
// amplitude-modulates the level (depth = DEPTH * kTapeFlutterAmt).
static constexpr float kTapeWowDelaySmp = 144.0f;  // wow delay (samples, ~3 ms)
static constexpr float kTapeWowCents    = 25.f;    // max wow pitch deviation (cents, full DEPTH)
static constexpr float kTapeFlutterAmt  = 0.25f;   // max flutter level modulation (0..25%, full DEPTH)
static constexpr float kTapeLpfAlpha    = 0.05f;   // flutter noise LPF one-pole alpha (slow)

// ---- FORMANT (F1+F2 one-pole IIR combs, morph by FORM) ---------------------
// MOVED from drone-osc (enrichment-2). The FORM knob selects a VOWEL POSITION
// (0..1) across the five formant stops A-E-I-O-U; the FVOL knob sets the
// PARALLEL-ADD gain of the formant stage (0 = fully dry / transparent). Each
// vowel is modelled by TWO parallel one-pole IIR comb resonators (its F1 + F2
// formant frequencies, from kFormantStops: the comb delay = sr/(2*f), so the
// resonance peak sits at f/2 -- a musical sub-octave band). The K-rate path
// (recompute_formant_comb) linearly crossfades the (F1, F2) pair between the
// two nearest stops; the per-sample path (only when FLVL > 0) runs both combs
// and ADDS the (scaled) sum of their feedback colors ON TOP of the intact
// signal (parallel branch, NOT dry/wet). Both combs are ALWAYS stable (single
// feedback tap, |fb| < 1). Copied verbatim from drone-osc/osc_constants.h.
static constexpr float kFormantStops[5][2] = {
    { 730.f, 1090.f },   // A
    { 530.f, 1840.f },   // E
    { 270.f, 2290.f },   // I
    { 450.f, 1020.f },   // O
    { 300.f,  870.f },   // U
};
static constexpr float kFormantGain       = 1.0f;  // parallel-add gain (formant color on top)
static constexpr float kFormantFbMax      = 0.9f;  // shared feedback ceiling (FVOL * this, < 1 = stable)
static constexpr uint32_t kFormantDelayMaxSmp  = 256U; // ring-buffer size (pow-of-2)
static constexpr uint32_t kFormantDelayMinSmp  = 8U;   // delay clamp (samples)
static constexpr uint32_t kFormantDelayClampSmp = 250U; // delay clamp (samples, in-buffer)

// ---- COMB (IIR resonator) --------------------------------------------------
// MOVED from drone-osc (enrichment-2). The COMB knob sets the feedback
// coefficient of a one-pole IIR comb (a sample delay line with its output
// summed back around a feedback loop). A single feedback tap produces a
// resonant peak at ~1/delay-samples Hz and its harmonics -- a "wooden /
// cavernous" resonance that tints the signal. The delay is a FIXED mid-band
// length (kCombDelaySmp, ~5.2 ms -> ~190 Hz peak); the knob sweeps the
// feedback 0..kCombFbMax (clamped < 1 for stability). At COMB = 0 the branch
// is a pure wire (regression-safe). Copied verbatim from drone-osc.
static constexpr float kCombFbMax      = 0.9f;    // feedback ceiling (< 1 = stable)
static constexpr uint32_t kCombDelaySmp  = 250U;  // fixed delay (~5.2 ms -> ~190 Hz peak)
static constexpr uint32_t kCombDelayMaxSmp = 256U; // ring-buffer size (pow-of-2, >= delay)

// ---- DRIFT (slot 2): the shared self-motion ceiling ------------------------
// Like RATE in the oscillator: a single ceiling that gates how far the seven
// "level" stages (ENSE / TREMO / FLNG / TAPE / FORM / FLVL / COMB) wander on
// their own. DRIFT = 0 -> every level stays at its manual value (static,
// regression-safe). DRIFT > 0 -> a K-rate random-walk nudges each level
// within [manual * (1 - d), manual * (1 + d)] where d = DRIFT (0..1), using
// exponential target gaps (tempo from TIME) + one-pole tracking (5..20 s).
// The L1 BUDGET (kDriftBudget) keeps at most one or two layers dominant: the
// sum of the seven drift-amplitudes is clamped to kDriftBudget, so the
// wandering "percolates" between layers instead of all seven moving at once.
static constexpr float kDriftBudget      = 1.5f;  // L1 budget on the sum of the 7 drift amplitudes
static constexpr float kDriftWalkTauMin  = 5.0f;  // min one-pole tracking time constant (s)
static constexpr float kDriftWalkTauMax  = 20.0f; // max one-pole tracking time constant (s)
static constexpr float kDriftGapMinSec   = 2.0f;  // min exponential target-gap mean (s)
static constexpr float kDriftGapMaxSec   = 12.0f; // max exponential target-gap mean (s)

// ---- LCG (Park-Miller-Carta) -----------------------------------------------
// The DRIFT walk and the tape flutter/wow noise are driven by an INTEGER
// linear congruential generator (Park-Miller-Carta), NOT osc_white/osc_rand
// (those belong to the OSC runtime and are NOT available to an FX unit). The
// generator is pure 32-bit integer math: no libm, no state beyond a uint32_t,
// so check-und is satisfied. The same seed is used on the device and in the
// web simulator (drone-chain/wasm.cc uses the identical pmc_rand()).
static constexpr uint32_t kLcgA    = 16807u;   // multiplier
static constexpr uint32_t kLcgSeed = 83647u;   // non-zero seed

// ---- Input gain (unity, no attenuation) -------------------------------------
// The OSC output is already attenuated by 0.5x (-6 dB) in chain.h to compensate
// for stereo summation. MOD does NOT attenuate the input further (kModInputGain = 1.0).
static constexpr float kModInputGain = 1.0f;

// ---- Output makeup (0.9x headroom) -------------------------------------------
// A small makeup gain keeps the formant/comb parallel-add stages from reading
// quieter than dry. 0.9x leaves 1 dB of headroom.
static constexpr float kModMakeup = 0.9f;

// ---- Pre/post LPF for ENSE chorus (aliasing suppression, Juno-style) --------
// Pre-LPF: one-pole, ~2 kHz cutoff, applied to the input BEFORE the chorus
// delay lines. Softens the high end to emulate the Juno-60 chip's 12 dB/oct
// pre-sampler LPF (we use 6 dB/oct one-pole as a lightweight stand-in).
// Post-LPF: one-pole, ~6 kHz cutoff, applied to the chorus sum AFTER the
// delay lines. Suppresses aliasing artifacts from the fractional-delay read.
// Alpha = 1 - exp(-2*pi*fc/sr), precomputed at 48 kHz.
static constexpr float kEnsePreLpfHz  = 2000.f;
static constexpr float kEnsePostLpfHz = 6000.f;

} // namespace modfx_consts

// ---- libm-free helpers (shared by the K-rate and audio paths) --------------
// one_minus_expf(x) = 1 - e^x, 3rd-order Taylor. Valid for small |x| (|x| <=
// ~0.1). Used to build the K-rate one-pole alphas (1 - exp(-1/tau)) for the
// DRIFT tracking and the LFO rate coefficients, and per-sample for the tape
// flutter LPF. Same trick as drone-osc's one_minus_expf (osc.h).
inline float modfx_one_minus_expf(float x)
{
  const float x2 = x * x;
  return -x - x2 * 0.5f - x2 * x / 6.0f;
}

// expf(x) via a 4th-order Taylor series, valid for |x| <= ~3.5 (the largest
// |lnRatio * p| we pass is ~3.3 at the TIME extremes). Accuracy over that
// range is <1e-3 relative -- far below audibility for an LFO frequency. Used
// only at K-rate (LFO frequency setup), never per sample. No libm.
inline float modfx_expf(float x)
{
  const float x2 = x * x;
  return 1.f + x + x2 * 0.5f + x2 * x / 6.0f + x2 * x2 / 24.0f;
}
