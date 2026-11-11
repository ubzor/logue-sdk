#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: modfx_constants.h
 *
 *  DRONE-MOD (modfx): static tuning constants for the drone modulation-effect
 *  unit: stage gains / ceilings / LFO curves / DRIFT budget.
 *
 *  IMPORTANT (check-und): the NTS-1 mkII FX runtime does NOT link libm into a
 *  unit, and the check-und target in the Makefile fails the build if any
 *  undefined (UND) libm symbol (powf / expf / sqrtf / logf / sinf / cosf /
 *  floorf ...) appears in the unit's dynamic symbol table. Every per-sample
 *  and K-rate computation in this unit therefore uses ONLY:
 *    * plain float / integer arithmetic, and
 *    * the hand-rolled one-pole / Taylor helpers (one_minus_expf, the sine
 *      LFO via a phase accumulator + Taylor sine).
 *  No transcendental calls in the audio path.
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
static constexpr float kEnseLFOMin = 0.02f;  // 0.02 Hz  (TIME = 0)
static constexpr float kEnseLFOMax = 0.6f;   // 0.6 Hz   (TIME = 1)
static constexpr float kEnseLFOlnR = 3.40120f; // ln(kEnseLFOMax / kEnseLFOMin)

static constexpr float kTremoLFOMin = 0.05f;  // 0.05 Hz
static constexpr float kTremoLFOMax = 0.5f;   // 0.5 Hz
static constexpr float kTremoLFOlnR = 2.29765f; // ln(kTremoLFOMax / kTremoLFOMin)

static constexpr float kFlngLFOMin = 0.30f;   // 0.30 Hz
static constexpr float kFlngLFOMax = 5.0f;    // 5.0 Hz
static constexpr float kFlngLFOlnR = 2.81341f; // ln(...)

// ---- DEPTH (slot 1) -> per-stage modulation depth --------------------------
// p.depth is 0..1. Each stage scales its modulation amplitude by a stage
// constant so a full DEPTH gives a clearly audible (but not destructive)
// amount. Chorus delay-spread and flanger delay are squared (DEPTH^2) so low
// DEPTH is a gentle shimmer and full DEPTH is a pronounced sweep.
static constexpr float kEnseMaxSpreadSmp = 384.0f; // chorus LFO delay-spread (samples, ~8 ms) at full DEPTH
// ENSE knob scaling: the knob maps to a MAX wet mix of 50% (kEnseWetMax). A
// full 50/50 stereo chorus with comb interference reads as mud/garble on the
// drone in mono and dulls the highs; capping at 50% keeps the ensemble
// "wide but present" at 100% and a gentle thickening at typical 20-40%.
static constexpr float kEnseWetMax = 0.5f;   // ENSE=1 -> 50% wet (max)
static constexpr float kTremoDepthMax    = 0.85f;  // tremolo depth: at TREMO=DEPTH=1, level dips to 15%
static constexpr float kFlngMaxDepthSmp  = 120.0f; // flanger delay (samples, ~2.5 ms) at DEPTH=1 (squared)

// ---- ENSEMBLE (stereo chorus, Juno-60 style) --------------------------------
// Architecture follows peterall/junologue-chorus (MIT), adapted for the
// drone-modfx NTS-1 mkII unit. Two parallel BBD chorus voices ("main" and
// "sub"), each a single REAL delay line (dsp::DelayLine) driven by two
// triangle LFOs (dsp::SimpleLFO) at fixed rates 0.513 Hz and 0.863 Hz.
// Each LFO reads the delay at two fractional taps (t and 1-t) and sums them
// with per-mode gains -> a smooth, wide, non-vibrato chorus wash.
// A 1st-order LPF (dsp::BiQuad::setFOLP) runs pre- and post-delay on each
// channel (emulating the Juno's passive RC anti-alias).
//
// TIME knob -> 3 chorus modes (I, I+II, II) with timbre sweep within each:
//   mode 0 (I):   main voice only  (delay_gain = {1.0, 0.0})
//   mode 1 (I+II): both voices     (delay_gain = {0.707, 0.707})
//   mode 2 (II):  sub voice only   (delay_gain = {0.0, 1.0})
// Within a mode, the LPF cutoff sweeps (brighter clockwise) via TIME frac.
//
// DEPTH knob -> dry/wet crossfade (orthonormal: dry=sqrt(1-x), wet=sqrt(x))
// so total level stays ~constant. DEPTH=0 -> pure dry (wire), DEPTH=1 ->
// 50/50 dry/wet.
//
// The ENSE knob is the master level of this stage: 0 = wire (bypass),
// 1 = full chorus (the dry/wet blend above). This keeps the DRONE MOD unit's
// 7-slot shape (ENSE is slot 3, same as TREMO/FLNG/TAPE).
//
// Delay line: 512 samples (next-pow2 of 5.4 ms x 48 kHz), ~27 KB SDRAM.
// Two delay lines (main + sub) = 54 KB. LFOs are q31 phase accumulators
// (no per-sample float state). LPFs are dsp::BiQuad (2 float state each).
// Total ENSE state: 54 KB SDRAM + ~10 float (RAM).
static constexpr float kEnseDelayMinMs = 15.0f;  // main voice min delay (ms, target ~18 ms chorus)
static constexpr float kEnseDelayMaxMs = 20.0f;  // main voice max delay (ms)
static constexpr float kEnseSubDelayMinMs = 28.0f; // sub voice min delay (ms, target ~31 ms)
static constexpr float kEnseSubDelayMaxMs = 34.0f; // sub voice max delay (ms)
static constexpr float kEnseLfoRate1 = 0.513f;   // LFO 1 rate (Hz, Juno BBD, when TIME at mid)
static constexpr float kEnseLfoRate2 = 0.863f;   // LFO 2 rate (Hz, Juno BBD, when TIME at mid)
// TIME knob now sweeps the ENSE LFO rate exponentially: rate = Min * exp(lnR * TIME).
// At TIME=0.5 the LFO1 lands near the classic Juno 0.513 Hz. Range ~0.05..~4.5 Hz.
static constexpr float kEnseLfoRateMin = 0.05f;  // slowest LFO sweep (Hz, TIME=0)
static constexpr float kEnseLfoRateLnR = 4.5f;   // exp span (e^4.5 ~= 90x)
// CMODE knob: 3 EQUAL bands (Formant-Vowel style) -> Juno voice blend.
//   band 0 (CMODE 0..341)   -> mode I    (main voice only)
//   band 1 (CMODE 342..683) -> mode I+II (both voices, classic Juno)
//   band 2 (CMODE 684..1023)-> mode II   (sub voice only)
// Equal thirds of the 0..1023 range so every value lands cleanly in a band.
static constexpr float kEnsePreLpfMinHz = 2000.f;  // pre-delay LPF min cutoff
static constexpr float kEnsePreLpfMaxHz = 23500.f; // pre-delay LPF max cutoff
static constexpr float kEnsePostLpfMinHz = 6000.f; // post-delay LPF min cutoff
static constexpr float kEnsePostLpfMaxHz = 23500.f;// post-delay LPF max cutoff
static constexpr uint32_t kEnseDelaySize = 512U;   // next-pow2(5.4ms * 48k) = 512 samples
// Juno chorus mode gains: {main_gain, sub_gain} for I, I+II, II
static constexpr float kEnseGainI[2]   = {1.0f, 0.0f};
static constexpr float kEnseGainI2[2]  = {0.7071f, 0.7071f}; // _3db
static constexpr float kEnseGainII[2]  = {0.0f, 1.0f}; // post-chorus LPF cutoff (Hz, Juno RC anti-alias)

// ---- TREMOLO (optical / harmonic) ------------------------------------------
// Classic optical tremolo: a single AM depth knob. The level dips
// (1 - TREMO*DEPTH*kTremoDepthMax) at the LFO trough and stays at unity at the
// LFO peak -- the drone's sub-anchor rides in the "on" half of the cycle.
// (The old LPF/HPF antiphase split was replaced by pure AM: it ate the bass.)
static constexpr float kTremoPhaseShiftR = 0.25f; // R LFO phase offset (0..1 of a cycle)

// ---- FLANGER (thru-zero) ---------------------------------------------------
// One-pole short delay (1..~120 samples, from DEPTH^2) swept by an LFO (x3
// rate, phase-shifted per channel for a stereo comb), + a feedback tap. The
// minimum delay keeps the comb "through zero" so there is no harsh null at 0.
static constexpr float kFlngDelayMinSmp  = 1.0f;  // min flanger delay (samples)
static constexpr float kFlngDelayMaxSmp  = 120.f; // max flanger delay (samples, ~2.5 ms, at full DEPTH)
static constexpr float kFlngFbMax        = 0.6f;  // max feedback (< 1 = stable), scaled by DEPTH
static constexpr float kFlngPhaseShiftR  = 0.5f;  // R LFO phase offset (half-cycle -> stereo comb)

// ---- TAPE (wow + flutter) --------------------------------------------------
// Wow: a random-walk (LCG) modulates the length of a ~3 ms one-pole delay ->
// slow pitch wander (depth = DEPTH * kTapeWowAmt of the delay). Flutter: an
// LCG white-noise one-pole modulates the level (depth = DEPTH *
// kTapeFlutterAmt). Both act only while TAPE > 0 (wet blend, 0 = wire).
static constexpr float kTapeWowBaseSmp  = 144.0f;  // wow delay base (samples, ~3 ms)
static constexpr float kTapeWowAmt      = 0.60f;   // wow depth: max delay deviation at full DEPTH
static constexpr float kTapeFlutterAmt  = 0.25f;   // flutter: max level modulation at full DEPTH
static constexpr float kTapeLpfAlpha    = 0.05f;   // flutter noise LPF one-pole alpha (slow)

// ---- DRIFT (slot 2): the shared self-motion ceiling ------------------------
// Like RATE in the oscillator: a single ceiling that gates how far the four
// "level" stages (ENSE / TREMO / FLNG / TAPE) wander on their own. DRIFT = 0 -> every level stays at its manual value (static,
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
// The OSC output (mono) is upmixed to L/R by Chain::process with NO
// attenuation (a plain duplicate, so the per-channel level equals the mono
// level; the chain's output is written straight through to the 2-channel
// worklet output, so there is no stereo summation to compensate for). MOD
// does NOT attenuate the input either: kModInputGain = 1.0.
static constexpr float kModInputGain = 1.0f;

// ---- Output makeup (0.9x headroom) -------------------------------------------
// A small makeup gain keeps the modulation stages from reading quieter than
// dry. 0.9x leaves 1 dB of headroom.
static constexpr float kModMakeup = 0.9f;

// ---- SDRAM layout (real delay lines) ----------------------------------------
// A genuine chorus / flanger / tape stage needs REAL delay lines, not one-pole
// filters: a one-pole is a lowpass (that is exactly why the old "modulated
// one-pole delays" read as mutes/ducking instead of mod-FX). The NTS-1 modfx
// slot provides 256 KB of allocatable external memory (see the platform
// README "Supported Modules" table). getBufferSize() reports this; unit.cc
// allocates it and init() slices it into the line buffers. The web chain
// (drone-chain/chain.h) slices the same size automatically.
static constexpr uint32_t kEnseLineLenSmp = 2048U;  // 34 ms @ 48 kHz (target 18/31 ms chorus); next-pow2
static constexpr uint32_t kFlngLineLenSmp = 192U;  //   4 ms max flanger delay + margin (per channel)
static constexpr uint32_t kTapeLineLenSmp = 384U;  //   8 ms tape wow delay + margin (per channel)
// ENSE: 2 MONO delay lines (Juno main + sub voices), 2048 smp each.
// FLNG: 1 interleaved L/R pair (192 smp/channel). TAPE: 1 L/R pair (384 smp).
static constexpr uint32_t kModLineBytesPerPair =
    2U * (kEnseLineLenSmp + kFlngLineLenSmp + kTapeLineLenSmp) * sizeof(float);
// ENSE: 2 mono lines x 2048 x 4 = 16384 bytes
// FLNG: 1 pair x 2 x 192 x 4 = 1536 bytes
// TAPE: 1 pair x 2 x 384 x 4 = 3072 bytes
// Total: 20992 bytes (~20.5 KB, well under 256 KB).
static constexpr uint32_t kModSramBytes =
    2U * kEnseLineLenSmp * sizeof(float) +        // 2 ENSE delay lines (main + sub, mono each)
    1U * 2U * kFlngLineLenSmp * sizeof(float) +   // 1 FLNG L/R pair
    1U * 2U * kTapeLineLenSmp * sizeof(float);    // 1 TAPE L/R pair
// = 2*2048*4 + 2*192*4 + 2*384*4 = 16384 + 1536 + 3072 = 20992 bytes (~20.5 KB)

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
