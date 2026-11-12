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
static constexpr float kEnseLFOMin = 0.01667f; // 0.0167 Hz = 60 s period (TIME = 0)
static constexpr float kEnseLFOMax = 0.6f;   // 0.6 Hz   = 1.7 s period (TIME = 1)
static constexpr float kEnseLFOlnR = 3.58332f; // ln(kEnseLFOMax / kEnseLFOMin) = ln(36)

// FLANGER LFO: SAME time-curve as the ENSE chorus LFO (kEnseLFOMin/kEnseLFOlnR),
// so the flanger sweep and the chorus sweep move in lockstep at every TIME.
// (Previously the flanger ran at 0.02..2.0 Hz = 50..0.5 s, which did NOT match
// the chorus 0.0167..0.6 Hz = 60..1.7 s -- the flanger read as "faster".)
static constexpr float kFlangerLFOMin = 0.01667f; // 0.0167 Hz = 60 s period (TIME = 0) -- matches ENSE
static constexpr float kFlangerLFOMax = 0.6f;     // 0.6 Hz (TIME = 1) -- matches ENSE main-voice max
static constexpr float kFlangerLFOlnR = 3.58332f; // ln(0.6 / 0.01667) = ln(36) -- matches ENSE

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

// ---- ENSEMBLE (stereo chorus, Juno-60 style) --------------------------------
// Architecture follows peterall/junologue-chorus (MIT), adapted for the
// drone-modfx NTS-1 mkII unit. Two BBD chorus voices ("main" and "sub"), each
// a single REAL delay line driven by a triangle LFO. The sub-voice LFO is
// DETUNED against the main by kEnseLfoDetune (x1.0618) so the two voices share
// one heartbeat and slowly drift in/out of phase -> classic chorus pulsing.
// Each LFO reads the delay at two fractional taps (t and 1-t) and sums them
// with the CMODE-selected per-voice gains -> a smooth, wide chorus wash.
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
// 7-slot shape (ENSE is slot 3, same as FLANGER/TAPE).
//
// Delay line: 512 samples (next-pow2 of 5.4 ms x 48 kHz), ~27 KB SDRAM.
// Two delay lines (main + sub) = 54 KB. LFOs are q31 phase accumulators
// (no per-sample float state). LPFs are dsp::BiQuad (2 float state each).
// Total ENSE state: 54 KB SDRAM + ~10 float (RAM).
static constexpr float kEnseDelayMinMs = 15.0f;  // main voice min delay (ms, target ~18 ms chorus)
static constexpr float kEnseDelayMaxMs = 20.0f;  // main voice max delay (ms)
static constexpr float kEnseSubDelayMinMs = 28.0f; // sub voice min delay (ms, target ~31 ms)
static constexpr float kEnseSubDelayMaxMs = 34.0f; // sub voice max delay (ms)
static constexpr float kEnseLfoDetune = 1.0618f; // sub-voice LFO detune (x main) -> chorus pulsing
// TIME knob now sweeps the ENSE LFO rate exponentially: rate = Min * exp(lnR * TIME).
// Range: 0.0167 Hz (60 s) .. 0.6 Hz (1.7 s). At TIME=0.5 ~0.1 Hz (10 s).
static constexpr float kEnseLfoRateMin = 0.05f;  // slowest LFO sweep (Hz, TIME=0)
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

// ---- FLANGER (thru-zero, multi-line / bucket-brigade) -----------------------
// FOUR mono delay lines, each with a SMALL feedback (kFlangerFbMax = 0.15, Q ~1.18),
// average into a smooth thru-zero comb WASH. The small feedback smooths the
// comb peaks (reduces the "metallic" spikes); the 4 lines spread the comb
// gratings (via per-line LFO phase offsets) so the wash is smooth. No "locking"
// at the LFO turnaround (the 4 de-tuned lines do not align).
//   - 4 mono lines of kFlangerLineLenSmp each (4 x 1024 smp = 16 KB).
//   - center delay 48..360 smp (1..7.5 ms) set by DEPTH^2; each line's LFO
//     sweeps the read tap +/-20% x DEPTH^2 around the center.
//   - per-line LFO phase offsets (kFlangerLinePhase[4]) spread the comb peaks.
//   - feedback = FLANGER x kFlangerFbMax (re-injected into the line, capped 0.15).
static constexpr uint32_t kFlangerLineCount = 4U;   // number of parallel mono delay lines
static constexpr float kFlangerDelayMinSmp  = 48.0f;  // min flanger delay (48 smp = 1.0 ms)
static constexpr float kFlangerDelayMaxSmp  = 360.f; // max flanger delay (360 smp = 7.5 ms, at full DEPTH)
static constexpr float kFlangerFbMax        = 0.15f; // max feedback (small -> low Q ~1.18, smooths comb peaks)
// Per-line LFO phase offsets (fraction of a cycle). KEEP ALL ZERO: de-tuning
// the 4 lines creates a 4x-LFO beat between L/R ("R/L alternating every 4 s")
// which reads as a shimmer. The smooth wash comes from the 4 lines AVERAGING
// the comb, not from de-tuning. Stereo comes ONLY from the R +0.5 cycle shift.
static constexpr float kFlangerLinePhase[4] = { 0.00f, 0.00f, 0.00f, 0.00f };

// ---- DRIFT (slot 2): the shared self-motion ceiling ------------------------
// Like RATE in the oscillator: a single ceiling that gates how far the four
// "level" stages (ENSE / FLANGER / TAPE) wander on their own. DRIFT = 0 -> every level stays at its manual value (static,
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
static constexpr uint32_t kFlangerLineLenSmp = 1024U; //  21 ms max flanger delay + margin (per channel)
// ENSE: 2 MONO delay lines (Juno main + sub voices), 2048 smp each.
// FLANGER: 4 MONO delay lines (bucket-brigade), 1024 smp each.
static constexpr uint32_t kModLineBytesPerPair =
    2U * (kEnseLineLenSmp + kFlangerLineLenSmp) * sizeof(float);
// ENSE: 2 mono lines x 2048 x 4 = 16384 bytes
// FLANGER: 4 mono lines x 1024 x 4 = 16384 bytes
// Total: 32768 bytes (~32 KB, well under 256 KB).
static constexpr uint32_t kModSramBytes =
    2U * kEnseLineLenSmp * sizeof(float) +        // 2 ENSE delay lines (main + sub, mono each)
    kFlangerLineCount * kFlangerLineLenSmp * sizeof(float);  // 4 FLANGER mono lines
// = 2*2048*4 + 4*1024*4 = 16384 + 16384 = 32768 bytes (~32 KB)

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
