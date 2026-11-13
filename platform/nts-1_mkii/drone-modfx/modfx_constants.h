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
static constexpr float kChorusLFOMin = 0.01667f; // 0.0167 Hz = 60 s period (TIME = 0)
static constexpr float kChorusLFOMax = 0.6f;   // 0.6 Hz   = 1.7 s period (TIME = 1)
static constexpr float kChorusLFOlnR = 3.58332f; // ln(kChorusLFOMax / kChorusLFOMin) = ln(36)

// FLANGER LFO: SAME time-curve as the chorus LFO (kChorusLFOMin/kChorusLFOlnR),
// so the flanger sweep and the chorus sweep move in lockstep at every TIME.
// (Previously the flanger ran at 0.02..2.0 Hz = 50..0.5 s, which did NOT match
// the chorus 0.0167..0.6 Hz = 60..1.7 s -- the flanger read as "faster".)
static constexpr float kFlangerLFOMin = 0.01667f; // 0.0167 Hz = 60 s period (TIME = 0) -- matches CHORUS
static constexpr float kFlangerLFOMax = 0.6f;     // 0.6 Hz (TIME = 1) -- matches CHORUS main-voice max
static constexpr float kFlangerLFOlnR = 3.58332f; // ln(0.6 / 0.01667) = ln(36) -- matches CHORUS

// ---- DEPTH (slot 1) -> per-stage modulation depth --------------------------
// p.depth is 0..1. Each stage scales its modulation amplitude by a stage
// constant so a full DEPTH gives a clearly audible (but not destructive)
// amount. Chorus delay-spread and flanger delay are squared (DEPTH^2) so low
// DEPTH is a gentle shimmer and full DEPTH is a pronounced sweep.
static constexpr float kChorusMaxSpreadSmp = 384.0f; // chorus LFO delay-spread (samples, ~8 ms) at full DEPTH
// CHORUS knob scaling: the knob maps to a MAX wet mix of 50% (kChorusWetMax). A
// full 50/50 stereo chorus with comb interference reads as mud/garble on the
// drone in mono and dulls the highs; capping at 50% keeps the chorus
// "wide but present" at 100% and a gentle thickening at typical 20-40%.
static constexpr float kChorusWetMax = 0.5f;   // CHORUS=1 -> 50% wet (max)

// ---- CHORUS (stereo chorus, Juno-60 style) --------------------------------
// Architecture follows peterall/junologue-chorus (MIT), adapted for the
// drone-modfx NTS-1 mkII unit. Two BBD chorus voices ("main" and "sub"), each
// a single REAL delay line driven by a triangle LFO. The sub-voice LFO is
// DETUNED against the main by kChorusLfoDetune (x1.0618) so the two voices share
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
// The CHORUS knob is the master level of this stage: 0 = wire (bypass),
// 1 = full chorus (the dry/wet blend above). This keeps the DRONE MOD unit's
// 7-slot shape (CHORUS is slot 3, same as FLANGER/TAPE).
//
// Delay line: 512 samples (next-pow2 of 5.4 ms x 48 kHz), ~27 KB SDRAM.
// Two delay lines (main + sub) = 54 KB. LFOs are q31 phase accumulators
// (no per-sample float state). LPFs are dsp::BiQuad (2 float state each).
// Total CHORUS state: 54 KB SDRAM + ~10 float (RAM).
static constexpr float kChorusDelayMinMs = 15.0f;  // main voice min delay (ms, target ~18 ms chorus)
static constexpr float kChorusDelayMaxMs = 20.0f;  // main voice max delay (ms)
static constexpr float kChorusSubDelayMinMs = 28.0f; // sub voice min delay (ms, target ~31 ms)
static constexpr float kChorusSubDelayMaxMs = 34.0f; // sub voice max delay (ms)
// Pre-computed delay ranges in SAMPLES (at kSr = 48000 Hz) -- the LFO sweeps
// within [MinSmp, MaxSmp]. constexpr so the per-block setup in
// modfx_process_impl does 0 float multiplies for the 4 range limits
// (saves ~8 float ops / block). Values are identical to the runtime
// expression kMs * 0.001f * kSr, so the audio is bit-identical.
static constexpr float kChorusMainDelayMinSmp = kChorusDelayMinMs  * 0.001f * kSr;
static constexpr float kChorusMainDelayMaxSmp = kChorusDelayMaxMs  * 0.001f * kSr;
static constexpr float kChorusSubDelayMinSmp  = kChorusSubDelayMinMs  * 0.001f * kSr;
static constexpr float kChorusSubDelayMaxSmp  = kChorusSubDelayMaxMs  * 0.001f * kSr;
static constexpr float kChorusLfoDetune = 1.0618f; // sub-voice LFO detune (x main) -> chorus pulsing
// TIME knob now sweeps the CHORUS LFO rate exponentially: rate = Min * exp(lnR * TIME).
// Range: 0.0167 Hz (60 s) .. 0.6 Hz (1.7 s). At TIME=0.5 ~0.1 Hz (10 s).
static constexpr float kChorusLfoRateMin = 0.05f;  // slowest LFO sweep (Hz, TIME=0)
// CMODE knob: 3 EQUAL bands (Formant-Vowel style) -> Juno voice blend.
//   band 0 (CMODE 0..341)   -> mode I    (main voice only)
//   band 1 (CMODE 342..683) -> mode I+II (both voices, classic Juno)
//   band 2 (CMODE 684..1023)-> mode II   (sub voice only)
// Equal thirds of the 0..1023 range so every value lands cleanly in a band.
static constexpr float kChorusPreLpfMinHz = 2000.f;  // pre-delay LPF min cutoff
static constexpr float kChorusPreLpfMaxHz = 23500.f; // pre-delay LPF max cutoff
static constexpr float kChorusPostLpfMinHz = 6000.f; // post-delay LPF min cutoff
static constexpr float kChorusPostLpfMaxHz = 23500.f;// post-delay LPF max cutoff
static constexpr uint32_t kChorusDelaySize = 512U;   // next-pow2(5.4ms * 48k) = 512 samples
// Juno chorus mode gains: {main_gain, sub_gain} for I, I+II, II
static constexpr float kChorusGainI[2]   = {1.0f, 0.0f};
static constexpr float kChorusGainI2[2]  = {0.7071f, 0.7071f}; // _3db
static constexpr float kChorusGainII[2]  = {0.0f, 1.0f}; // post-chorus LPF cutoff (Hz, Juno RC anti-alias)

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
// "level" stages (CHORUS / FLANGER / TAPE) wander on their own. DRIFT = 0 -> every level stays at its manual value (static,
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

// ---- NOISE / NRATE (slots 6/7): granular noise-sweep (noise enrichment) -----
// An additive, LCG-driven "metallic rustle" / dust-flash generator on the
// INPUT of the MOD path (before CHORUS), so the noise itself passes through the
// chorus/flanger and reads as "rustle IN the wash" rather than a dry hiss on
// top. All-off by default (NOISE = 0 = dry wire, regression-safe).
//
// Signal chain of the branch (hand-rolled, no libm -- see the modfx.h block
// for the per-sample math):
//   fx_white() white noise  -> burst gate (exponential cadence, NRATE)
//   -> AM envelope (DRIFT walk channel 3, 0.5+0.5*walk)  -> 2-pole SVF
//   band-pass (cutoff swept by TIME)  ->  additive mix x NOISE (x 0.5 norm).
//
// Burst cadence (NRATE, slot 7): the AVERAGE gap between flashes, in
// FLASHES/SECOND (not Hz -- it is a Poisson process mean, like the OSC
// random-walk periods).  NRATE 0 -> 0.15 fl/s (rare, long lulls), NRATE 1 ->
// 8 fl/s (dense shimmer). Each gap is drawn EXACTLY like RandomWalk in
// drone-osc (osc_random_walk.h): -ln(U) * mean_samples, U in (0,1] from
// fx_rand() -- so the cadence "breathes" instead of ticking.
static constexpr float kNoiseBurstMinPerSec = 0.15f;              // rare flashes (NRATE = 0)
static constexpr float kNoiseBurstMaxPerSec = 8.0f;               // dense shimmer (NRATE = 1)
// ln(kNoiseBurstMaxPerSec / kNoiseBurstMinPerSec) = ln(53.33) = 3.977
static constexpr float kNoiseBurstLnR       = 3.977f;
// Per-burst DURATION (envelope attack/release, samples @ 48 kHz). The envelope
// is a one-pole toward (open ? 1 : 0): the attack alpha opens the burst in
// ~8 ms (kNoiseBurstAtkSmp), the release alpha decays it over ~45 ms
// (kNoiseBurstRelSmp), giving short "flash" envelopes rather than hard clicks.
static constexpr uint32_t kNoiseBurstAtkSmp = 384U;   // ~8 ms  attack
static constexpr uint32_t kNoiseBurstRelSmp = 2112U;  // ~45 ms release
// Precomputed per-sample one-pole alphas for the burst envelope:
//   alpha = 1 - exp(-1 / N_samples)
// Computed at compile time so the per-sample path is pure float math and no
// C++ guard variables (__cxa_guard_acquire/release) appear in the UND symbol
// table (those would break the Makefile check-und guard -- the FX firmware
// does not provide them). Values: 1-exp(-1/384) = 0.00259879, 1-exp(-1/2112)
// = 0.000473374.
static constexpr float kNoiseBurstAtkAlpha = 0.00259879f;
static constexpr float kNoiseBurstRelAlpha = 0.000473374f;
// Burst gain: a white-noise burst is already hot, so the branch is scaled by
// kNoiseBurstGain (the NOISE knob itself is a further 0..1 level on top).
static constexpr float kNoiseBurstGain = 0.6f;

// ---- Maximum-diversity burst parameters ------------------------------------
// Each flash is randomized on THREE independent axes so consecutive "knocks"
// never sound alike (the user's ask: "максимальное разнообразие"):
//   1. DURATION -- the burst stays open for a random window (below), not a
//      fixed 0.5 ms. Short ticks and longer "whooshes" mix.
//   2. LEVEL    -- each burst is scaled by a random per-burst gain in
//      [kNoiseBurstLevelMin, kNoiseBurstLevelMax] (quiet "pings" to loud
//      "claps"), so the envelope peak varies even at DRIFT = 0.
//   3. TONE     -- the band-pass CENTER wanders around the TIME knob value by
//      a random per-burst +/- half-octave (kNoiseBpWanderHalfOct), so each
//      knock rings at a slightly different pitch (the "metallic" spread).
// All three are drawn from fx_rand() at burst start (K-rate-ish: once per
// burst, not per sample), so the per-sample path stays pure float math.

// Burst DURATION (on-time) window, in samples @ 48 kHz. Drawn log-uniformly
// (via exp) between min/max so both short ticks and long whooshes appear.
static constexpr uint32_t kNoiseBurstOnMinSmp = 48U;    // ~1 ms  (sharp tick)
static constexpr uint32_t kNoiseBurstOnMaxSmp = 1200U;  // ~25 ms (soft whoosh)
// ln(kNoiseBurstOnMaxSmp / kNoiseBurstOnMinSmp) = ln(25) = 3.219
static constexpr float kNoiseBurstOnLnR = 3.219f;

// Per-burst LEVEL (gain) window. 0.25..1.0: most bursts are clearly audible,
// a minority are quiet "dust" pings. Drawn linearly from fx_rand().
static constexpr float kNoiseBurstLevelMin = 0.25f;
static constexpr float kNoiseBurstLevelMax = 1.0f;

// Per-burst TONE wander: the SVF cutoff is scaled by 2^(+/- wander * dev),
// dev in [-1, +1] drawn per burst. 0.5 = a half-octave each way (clear pitch
// spread, still musical against the drone). At 0 the tone is fixed at the
// TIME knob value (regression toward the pre-wander behavior).
static constexpr float kNoiseBpWanderHalfOct = 0.5f;

// Band-pass (2-pole SVF, state-variable; same topology as drone-osc osc_svf.h).
// Cutoff is swept by TIME (shared exp curve with the chorus LFO rate):
//   fc = kNoiseBpMinHz * exp(kNoiseBpLnR * TIME)
// so the "metallic" center of the rustle tracks the modulation tempo.
// NRATE 0 -> ~300 Hz (low, dusty), NRATE 1 -> ~8 kHz (bright, hissy).
// Q is fixed (no slot budget left): 1.5 -- a soft peak, clearly band-pass but
// not ringing. cc/ae are clamped exactly like osc_svf.h (poles in the unit
// circle across the whole 300 Hz .. 8 kHz range).
// Cutoff range is swept by TIME (NOT NRATE -- the burst CADENCE is NRATE):
// TIME 0 -> ~300 Hz (low, dusty), TIME 1 -> ~8 kHz (bright, hissy).
static constexpr float kNoiseBpMinHz = 300.f;
static constexpr float kNoiseBpMaxHz = 8000.f;
// ln(8000/300) = ln(26.67) = 3.284
static constexpr float kNoiseBpLnR   = 3.284f;
static constexpr float kNoiseBpQ     = 1.5f;
static constexpr float kNoiseBpCcMax = 0.92f;  // per-sample cc clamp (stability)
static constexpr float kNoiseBpCcMin = 0.001f; // per-sample cc clamp
static constexpr float kNoiseBpAeMin = 0.18f;  // per-sample ae clamp

// Additive mix level (NOISE): the knob's 0..1 is multiplied by this before
// adding to the dry signal. The NOISE v2 source (room tone 0.15 + grain 0.15/8
// + burst 0.367 + dips AM 0.5..1.0) has a peak envelope of ~0.75, so a 0.5 wet
// norm saturated at ~25% of the knob (noise_mix * source ~ 0.375). 0.08 gives
// a smooth 0..100% ramp where NRATE (the burst cadence) is actually audible
// across the whole knob range -- at 0.5 the "threshold" (raw >= ~5) is already
// at the old 0.375 peak and any higher value is inaudibly small.
static constexpr float kNoiseWetNorm = 0.08f;

// DRIFT walk channel 3 (index kDriftCount-1): the slow AM modulator of the
// noise branch. At DRIFT = 0 the channel is static at 0 -> AM = 0.5 (flat);
// DRIFT > 0 -> AM wanders 0..1 with the shared walk (tau 5..20 s). This is
// what makes the rustle "breathe" -- the DRIFT knob finally does something
// audible (it previously only had un-wired walk state).

// ---- ENSEMBLE (slot 8): string-synth ensemble (RS-202 style) -----------------
// Architecture: quarterturn/teensy3-ensemble-chorus (MIT), adapted with
// hand-rolled float delay lines / LFOs (no CMSIS / Teensy-Audio dependency).
// One SHARED MONO delay line (kEnsembleLineLenSmp = 1024 smp = 21 ms), three
// read taps whose LFO phases are 120 deg apart (1/3-cycle spacing), summed and
// averaged (x 1/3). The LFO waveform is a BINARY-sine: 1.0*sin(w) +
// (1/8)*sin(10*w) -- a FULL-amplitude base rate with a gentle 10x harmonic
// that makes the tempo "breathe" (the RS-202's slow amplitude modulation
// inside the delay sweep). The base sine is NOT halved: the original Teensy
// LFO_RANGE=100 is an 8-bit unit (~0.098 full scale), so the original sweep is
// much narrower; here the base keeps full amplitude for a clear ensemble
// motion while the 1/8 harmonic stays smooth.
// Stereo: the R channel reads the SAME three taps shifted by kEnsemblePhase90
// (367 smp ~ 90 deg of the 1024-smp line) relative to L -- the two ears are
// always 90 deg apart on the line, a classic chorus stereo spread. A 1st-order
// LPF (kEnsembleLpfAlpha) per channel keeps the comb edges from reading
// metallic. The branch is ADDITIVE (like NOISE) and sits BEFORE the Juno
// chorus, so the ensemble "strings" pass through CHORUS / FLANGER "in the
// wash". ENSMBL = 0 -> dry wire (regression-safe, branch never computed).
//
// LFO rate: the BASE rate is TIME-swept 0.0167..0.6 Hz (the SAME exp curve as
// CHORUS / FLANGER: 60 s period at TIME=0 -> 1.7 s at TIME=1), so the ensemble
// moves in lockstep with the other stages. The 10x "breathing" harmonic then
// wobbles at 0.167..6 Hz (kept very gentle by the 1/16 weight, so it reads as
// a slow swell / subtle vibrato shimmer, not a flutter).
//
// SDRAM: one extra 1024-float mono line (kEnsembleLineLenSmp) is appended to
// the CHORUS / FLANGER lines below (kModSramBytes grows by 4096 bytes, total
// 36 KB -- well inside the NTS-1 modfx slot's 256 KB).
static constexpr uint32_t kEnsembleLineLenSmp = 1024U; // 21 ms @ 48 kHz (original Teensy: 1024)
static constexpr uint32_t kEnsemblePhase90    = 367U;  // 90-deg shift on the line (original: 367)
static constexpr float kEnsembleHarmonic = 10.0f;      // LFO harmonic multiplier (original: 20*pi / 2*pi)
// Weight of the 10x "breathing" harmonic, RELATIVE TO THE FULL base sine.
// The base sine has full amplitude 1.0 (see ensLfoStep). The original Teensy
// LFO_RANGE=100 was an 8-bit unit (100/1024 ~ 0.098), NOT 100% amplitude --
// the original delay sweep is much narrower and the breathing is gentle.
// The base rate is now the SAME slow curve as CHORUS / FLANGER (0.0167..0.6
// Hz), so the 10x harmonic (0.167..6 Hz) must stay clearly secondary: 1/16
// wobbles the instantaneous LFO rate by only ~ +/-12.5% around the base, so
// the tempo "breathes" as a slow swell / subtle vibrato shimmer instead of a
// flutter (1/6 or 1/8 read as a busy, fast flutter on the slow base).
static constexpr float kEnsembleHarmonicWeight = 0.0625f; // 1/16 (gentle breathing)
// Base LFO rate: SAME exp curve as CHORUS / FLANGER (0.0167 Hz = 60 s at
// TIME=0 -> 0.6 Hz = 1.7 s at TIME=1), so the ensemble sweep moves in
// lockstep with the chorus/flanger at every TIME. (Previously the ensemble ran
// 9x faster at the low end -- 0.15 Hz = 6.7 s -- which read as a short, busy
// flutter instead of the slow string-machine swell.)
static constexpr float kEnsembleLFOMin = 0.01667f;     // 60 s period (TIME = 0) -- matches CHORUS / FLANGER
static constexpr float kEnsembleLFOMax = 0.6f;         // 1.7 s period (TIME = 1) -- matches CHORUS / FLANGER
static constexpr float kEnsembleLFOlnR = 3.58332f;     // ln(kEnsembleLFOMax / kEnsembleLFOMin) = ln(36) -- matches CHORUS
static constexpr float kEnsembleWetNorm = 1.0f;        // final wet ceiling (matched to CHORUS; was 0.5 -> ensemble read ~2x quieter than CHORUS/FLANGER at the same knob value)
static constexpr float kEnsembleLpfAlpha = 0.3f;       // 1st-order LPF per channel (anti-metallic)
// Delay geometry: the LFO waveform w in [-1, 1] maps to delay =
// (center + spread * w) samples. The CENTER is fixed at 15 ms (720 smp, a
// string-machine / chorus-main delay zone, NOT the old 1.9 ms which read as a
// thin, "surface" shimmer). The SPREAD is DEPTH-driven (DEPTH^2, like the
// FLANGER): at DEPTH=1 the sweep spans 720 +/- 260 = 460..980 smp
// (9.6..20.4 ms, a full ensemble swell); at DEPTH=0 it is a near-static
// layer (only the LFO's 1/16 "breathing" remains). Previously the spread was
// a fixed 50 smp and DEPTH did nothing to the ensemble -- that is why the
// branch read as shallow and DEPTH-independent.
static constexpr float kEnsembleDelayCenterSmp = 720.f; // 15 ms center delay (fixed)
static constexpr float kEnsembleMaxSpreadSmp   = 260.f; // full DEPTH^2=1 spread (9.6..20.4 ms)

// ============================================================================
//  TAPWOB (edit param 7) -> tape wow/flutter pitch-mod (the 5th stage)
// ============================================================================
// TAPWOB is the ONLY stage that modulates PITCH (everything before it -- CHORUS /
// FLANGER / NOISE / ENSMBL -- modulates delay/level/spectrum, not frequency).
// Architecture: ONE shared MONO delay line (kTapeWobLineLenSmp, ~10.7 ms), two
// read taps (L/R) whose offsets are swept by TWO independent LFOs:
//   * WOW     -- slow (0.0167..0.6 Hz from TIME), amplitude up to 40 cents x
//                DEPTH^3, the "slow pitch sag" of a worn tape platter.
//   * FLUTTER -- fast (0.5..15 Hz from TIME), amplitude up to 12 cents x
//                DEPTH^2, the high-speed "wow" of a flapping capstan.
// The sum (wow + flutter) is converted cents -> fractional sample offset on the
// line and added to a fixed center delay (kTapeWobCenterSmp, ~5.3 ms). A
// 1st-order LPF per channel (cutoff 2..8 kHz, TAPWOB-swept) dulls the highs as
// the deck "wears". The branch is a DRY/WET replace (NOT additive like
// NOISE/ENSMBL): out = dry*(1-T) + wet*T, T = TAPWOB (full wet at TAPWOB=1).
// TAPWOB = 0 -> dry wire (regression-safe, never computed).
//
// Why AFTER CHORUS/FLANGER (and not before): wow/flutter on the FINAL, already
// modulated timbre reads as a tape worn OVER the whole FX chain (the classic
// "tape deck on the master bus" sound). Before the comb stages the wobble would
// be smeared by the chorus/flanger and read weaker. Sits last in the path, right
// before the makeup gain (see the audio path in modfx.h).
//
// TAPWOB uses the FIXED TIME / DEPTH knobs (like every other stage):
//   * TIME  -> LFO tempo: the wow/flutter rates follow the SAME shared exp
//     curve as CHORUS / FLANGER / ENSMBL, so at TIME=0 the tape "breathes" over
//     a very long period and at TIME=1 it churns fast -- "wow" stays slow
//     (max 0.6 Hz), "flutter" stays fast (max 15 Hz), both in lockstep with the
//     rest of the stack.
//   * DEPTH -> modulation depth: WOW is scaled as DEPTH^3 (cubic, much
//     shallower than the FLANGER's DEPTH^2) so low DEPTH stays a subtle pitch
//     shimmer; FLUTTER stays on DEPTH^2 (12 cents is already tame).
//   * TAPWOB -> DRY/WET LEVEL only (0 = off, 1 = full tape deck) + the LPF
//     "dullness" (2..8 kHz).
//
// The line is deliberately LONG (512 smp, doubled from the original 256) so the
// fractional read-tap velocity stays well under 1 sample/sample at peak LFO
// speed -- a short line made the mid-cycle read "skip" slots and click.
//
// cents -> samples: 1 cent = 1/1200 octave; a delay of D samples at 48 kHz holds
// phase D/f0. Shifting the read by delta samples shifts pitch by delta/D octaves
// = 1200*delta/D cents. We solve delta = cents/1200 * D (D = kTapeWobCenterSmp,
// the FIXED center delay, so the conversion is constant and cheap).
static constexpr uint32_t  kTapeWobLineLenSmp   = 512U;  // 10.7 ms @ 48 kHz (doubled from 256: halves the read-tap velocity so the fractional read never "skips" samples -> no clicks at high DEPTH)
static constexpr float     kTapeWobCenterSmp    = 256.f; // 5.3 ms center delay (fixed; half the line, the "tape" resting position)
static constexpr float     kTapeWobWowMinHz     = 0.01667f; // slow wow at TIME=0 (60 s period, == CHORUS min)
static constexpr float     kTapeWobWowMaxHz     = 0.6f;     // (TIME=1, == CHORUS max)
static constexpr float     kTapeWobWowLnR       = 3.58332f; // ln(0.6/0.01667) = ln(36) -- matches CHORUS
static constexpr float     kTapeWobFlutMinHz    = 0.5f;     // fast flutter at TIME=0 (2 s period)
static constexpr float     kTapeWobFlutMaxHz    = 15.f;     // (TIME=1, ~4x the wow rate -> "flutter" feel)
static constexpr float     kTapeWobFlutLnR      = 3.40120f; // ln(15/0.5) = ln(30)
// Wow uses a CUBIC depth curve (DEPTH^3, much shallower than the FLANGER's
// DEPTH^2) so low DEPTH stays a subtle pitch shimmer rather than an audible
// "wobble". Flutter stays on DEPTH^2 (12 cents is already tame).
//
// PEAK AMPLITUDES (tuned against the doubled 512-smp line): the read-tap
// excursions are kept small enough that, even at the peak LFO velocity
// (mid-cycle), the tap moves well under 1 sample/sample -- so the fractional
// linear read never "skips" a slot and produces no clicks, at any DEPTH.
// With center/1200 as the cents->sample scale (center=256 -> 1 cent = 0.213
// smp):
//   wow     40 cents x DEPTH^3 -> 8.5 smp peak at DEPTH=1, 3.4 smp at 75%
//   flutter 12 cents x DEPTH^2 -> 2.6 smp peak at DEPTH=1, 1.8 smp at 75%
//   combined peak-to-peak ~ 11 smp at DEPTH=1 -- ~2% of the 512-smp wave,
//   a clean musical wobble/flutter (not a gritty skip).
static constexpr float     kTapeWobWowCentsMax  = 40.f;  // full DEPTH^3=1 wow depth (cents peak)
static constexpr float     kTapeWobFlutCentsMax = 12.f;  // full DEPTH^2=1 flutter depth (cents peak)
static constexpr float     kTapeWobCents2Smp    = kTapeWobCenterSmp / 1200.f; // delta = cents * this
static constexpr float     kTapeWobWetMax       = 1.0f;  // full wet at TAPWOB = 1 (dry/wet replace)
static constexpr float     kTapeWobLPFMinHz     = 2000.f; // LPF cutoff (TAPWOB = 0, very dull)
static constexpr float     kTapeWobLPFMaxHz     = 8000.f; // (TAPWOB = 1, mostly transparent)

// ============================================================================
//  WARP (tape edit 4) -> "magnetic instability" (The Kiss of Shame "Shame" port)
// ============================================================================
// WARP is the KoS "Shame" effect: a long delay line whose READ TAP is modulated
// by a cosine LFO, so the signal is "re-read" at a slowly wandering position ->
// the pitch/timbre wobbles with a random, non-repeating character (magnetic
// particle instability / tape stretch). Unlike TAPWOB (which modulates PITCH via
// a short 5.3 ms line) WARP is the SLOW "instability" layer: a 4096-smp (85 ms)
// shared MONO line, read at (center + cosine-LFO * depth).
//
// The LFO is a plain cosine (Taylor, no libm) whose RATE is set K-rate from the
// WARP knob (KoS stages: low WARP = slow 7 Hz small wobble, high WARP = fast 77 Hz
// deep wobble). After every full LFO cycle the rate gets a RANDOM kick
// (rateFluctuation = rand * rate * periodicity, fx_rand-driven) so the wobble
// never repeats -- the signature "tape is alive" feel. WARP = 0 -> off (dry wire).
static constexpr uint32_t  kWarpLineLenSmp    = 4096U;  // 85 ms @ 48 kHz (power-of-2, 16 KB)
static constexpr float     kWarpCenterSmp     = 2048.f; // read-center (half the line)
static constexpr float     kWarpLFOMinHz      = 7.0f;   // LFO rate at WARP=0 (KoS stage 1 rate)
static constexpr float     kWarpLFOMaxHz      = 77.0f;  // LFO rate at WARP=1 (KoS stage 2/3 rate)
static constexpr float     kWarpLFOlnR        = 2.39789f; // ln(77/7)
static constexpr float     kWarpMaxDepthSmp   = 30.0f;  // max LFO depth (halved from 60: full WARP reads as a gentle "magnetic sag", not a pitch wobble; KoS stage-2 max was 30)
static constexpr float     kWarpFluctMin      = 0.5f;   // periodicity at low WARP (KoS stage 1)
static constexpr float     kWarpFluctMax      = 0.25f;  // periodicity at high WARP (KoS stage 2)

// ============================================================================
//  HISS (tape edit 3) -> tape hiss (KoS Hiss, procedural: no WAV)
// ============================================================================
// The SDK has no precomputed pink/hiss sample (checked: only fx_white/fx_rand),
// so the hiss is SYNTHESIZED: fx_white() -> 1st-order HPF (kHissHpfFc) -> a
// quiet level. KoS reads a looped Hiss.wav with a two-position crossfade to
// avoid loop clicks; a white-noise HPF needs no loop (it IS the noise) and is
// inaudibly different on a drone bed. HISS = 0 -> off.
static constexpr float     kHissHpfFc         = 1500.f; // HPF cutoff: white -> "hiss" coloration
// Max hiss level (linear gain). KoS uses 0.005 (~ -46 dB) but that is INAUDIBLE
// in a drone bed; 0.08 (~ -22 dB) is the "audible hiss" level -- the HPF (1500 Hz)
// already shapes it to "hiss" color, so a 0.08 linear gain is the right "loud"
// target. 0.05..0.10 is the sensible window; 0.08 sits mid.
// Raised from 0.08 (−22 dB) to 0.25 (−12 dB): at the old ceiling the hiss was a
// few dB of RMS over a full-scale drone and read as "there but not audible".
// 0.25 is an auditable hiss bed; the per-sample HISS branch ALSO attenuates the
// dry signal by (1 − hiss·0.4) so it reads as "tape on the master bus" rather
// than a parallel noise layer (the KoS Hiss class does the same:
// signalLevel = 1 − hissLevel).
static constexpr float     kHissMaxGain       = 0.25f;  // ~ -12 dB max (auditable over the drone)
// Dry-attenuation factor applied at HISS>0: dry_scale = 1 − hiss·0.4. At HISS=1
// the dry is ducked to 0.6 (−4.4 dB) so the hiss reads as "tape", not as a
// parallel additive noise layer. 0.4 keeps the duck subtle enough that the
// drone does not "pump" at low HISS.
static constexpr float     kHissDryDuck       = 0.4f;

// ============================================================================
//  NOISE v2 (noise edit 0) -> granular rustle (KoS HurricaneSandy "age" port)
// ============================================================================
// The NOISE branch is upgraded from the old single-band-passed white-flash to a
// KoS-HurricaneSandy-style granular rustle: a PINK-noise source (procedural, 3
// one-pole LPF) feeds a small GRANULATOR (multi-voice linear-environment grains
// with random pointer/offset, the classic MacPod scheme -- written from scratch,
// not copied from STK) -> HPF + LPF band-shape. An AM "dips" curve (random
// envelope points, 1 s domain) makes the rustle BREATHE, and periodic noise
// bursts DUCK the dry signal (the KoS sigEnv) for the "tape scrape" moment.
//
// PINK source: the Voss / F.Vedel 3-filter white->pink. Each one-pole LPF is
//   y[n] = a*x[n] + (1-a)*y[n-1]; the three outputs subtracted give ~ -3 dB/oct.
// Coefficients (a) are fixed at the standard 0.997 / 0.985 / 0.950 (sample-
// rate independent for our 48 kHz). 0 KB, 9 ops/sample.
static constexpr float     kPinkA1            = 0.997f;
static constexpr float     kPinkA2            = 0.985f;
static constexpr float     kPinkA3            = 0.950f;
static constexpr uint32_t  kPinkRingLenSmp    = 8192U;  // grain source ring (power-of-2, 32 KB)
static constexpr uint32_t  kGrainVoices       = 8U;     // concurrent grains (KoS uses 10; 8 = cheaper, same bed)
static constexpr float     kGrainDurationMs   = 8.0f;   // grain length (KoS base 5 ms; 8 ms reads fuller)
static constexpr float     kGrainRampPct      = 50.0f;  // linear ramp % of the grain (KoS 50)
static constexpr float     kGrainOffsetMs     = 8.0f;   // random pointer offset window (KoS 50 ms -> scaled down)
static constexpr float     kGrainDelayMs      = 20.0f;  // gap between grains at NRATE=0 (KoS 20 ms)
static constexpr float     kGrainRandomFactor = 0.97f;  // random spread (KoS 0.97)
static constexpr float     kGrainHpFc         = 50.f;   // grain HPF (KoS 50 Hz)
static constexpr float     kGrainLpFc         = 2000.f; // grain LPF (KoS 2000 Hz)
// Raised from 0.15 to 0.45: at the old gain p.noise=0.2 was inaudible against the
// drone bed and p.noise>0.5 all read as "the same loud rustle" -- the knob felt
// like a switch. 0.45 makes p.noise=0.2 clearly audible and keeps a smooth
// growth curve up to 1.0 (the branch is still fully gated at NOISE=0, so the
// regression-safe "off" behavior is unchanged).
static constexpr float     kGrainGain         = 0.45f;  // rustle bed level under NOISE (raised for smooth ramp)
// AM "dips": N random points over a 1 s domain, 1.0 down to (1-extremity),
// linearly interpolated and re-seeded every second (fx_rand). The KoS
// EnvelopeDips (15 pts, extremity 0.5, 1 s). We use 16 pts (pow-2-free, cheap).
static constexpr uint32_t  kDipsCount         = 16U;
static constexpr float     kDipsDomainSmp     = 48000.f; // 1 s @ 48 kHz
// Raised from 0.5 (dips 1.0 -> 0.5) to 0.85 (dips 1.0 -> 0.15): the old shallow
// envelope meant the rustle "breathing" was inaudible, so turning the NOISE
// knob felt like a switch. Deeper dips give a visible/audible level swing as
// you sweep NOISE, reinforcing the "smooth ramp" feel. The re-seed cadence
// (1 s) is unchanged, so the breathing stays musical, not noisy.
static constexpr float     kDipsExtremity     = 0.85f;   // dips from 1.0 to 0.15 (deeper breathing, smooth ramp)
static constexpr float     kDipsPointRand     = 0.5f;    // point-count randomness (KoS 0.5)
// Noise-burst "scrape": a fast attack/release envelope on a white-noise burst
// (KoSSand noiseEnv: attack ~14%, peak ~38%, release ~43% of a 350-smp window).
// The DRY signal is ducked by (1 - duckDepth*burstEnv) during the burst (KoS
// sigEnv). Cadence (how often bursts fire) is NRATE-driven (see kNoiseBurstMean*).
static constexpr float     kBurstAtkFrac      = 0.143f;  // attack end (fraction of burst window)
// Burst peak: raised from 0.367 to 0.55 so each scrape is clearly audible and
// the NRATE cadence (how often they fire) is the dominant cue, not buried in
// the rustle bed. At 0.367 the burst energy was comparable to the grain, so
// NRATE was hard to A/B.
static constexpr float     kBurstPeak         = 0.55f;   // burst level at peak (louder scrape -> NRATE audible)
static constexpr float     kBurstRelFrac      = 0.428f;  // release end
// Burst window: raised from 16 smp (~0.3 ms -- an inaudible "tick") to 128 smp
// (~2.7 ms -- a perceptible "scrape", in the same order of magnitude as the KoS
// ~7 ms window). At 16 smp the burst energy was so short that the NRATE cadence
// (0.15..8 fl/s) did not change the perceived loudness at all, so NRATE read as
// "does nothing". At 128 smp the scrape is felt as a real tape-scrape and the
// 0.15 -> 8 fl/s cadence is clearly audible across the NRATE range.
static constexpr float     kBurstWindowSmp    = 128.f;   // burst window (samples, ~2.7 ms scrape, audible)
// Dry-duck depth: reduced from 0.8 to 0.5. At 16 smp a 0.8 dry-duck was
// inaudible (the burst was over before the ear could notice the dip). At 128
// smp a 0.8 duck over a 2.7 ms window at up to 8 fl/s would read as a "pump";
// 0.5 keeps the dip perceptible (the NRATE cue) without pumping.
static constexpr float     kBurstDuckDepth    = 0.5f;    // dry-signal duck depth at burst peak (0.5 = perceptible, no pump)
static constexpr float     kRoomToneLpfFc     = 80.f;    // low-freq room-tone LPF (KoS lowFreqGrainNoise)
static constexpr float     kRoomToneLevel     = 0.15f;   // room-tone level under NOISE (KoS 0.15)

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
static constexpr uint32_t kChorusLineLenSmp = 2048U;  // 34 ms @ 48 kHz (target 18/31 ms chorus); next-pow2
static constexpr uint32_t kFlangerLineLenSmp = 1024U; //  21 ms max flanger delay + margin (per channel)
// CHORUS: 2 MONO delay lines (Juno main + sub voices), 2048 smp each.
// FLANGER: 4 MONO delay lines (bucket-brigade), 1024 smp each.
static constexpr uint32_t kModLineBytesPerPair =
    2U * (kChorusLineLenSmp + kFlangerLineLenSmp) * sizeof(float);
// CHORUS: 2 mono lines x 2048 x 4 = 16384 bytes
// FLANGER: 4 mono lines x 1024 x 4 = 16384 bytes
// Total: 32768 bytes (~32 KB, well under 256 KB).
static constexpr uint32_t kModSramBytes =
    2U * kChorusLineLenSmp * sizeof(float) +        // 2 CHORUS delay lines (main + sub, mono each)
    kFlangerLineCount * kFlangerLineLenSmp * sizeof(float) + // 4 FLANGER mono lines
    kEnsembleLineLenSmp * sizeof(float) +            // 1 ENSEMBLE mono line
    kTapeWobLineLenSmp * sizeof(float) +             // 1 TAPWOB mono line
    kWarpLineLenSmp * sizeof(float) +                // 1 WARP mono line (magnetic instability)
    kPinkRingLenSmp * sizeof(float);                 // 1 PINK-noise grain source ring
// = 2*2048*4 + 4*1024*4 + 1024*4 + 512*4 + 4096*4 + 8192*4
// = 16384 + 16384 + 4096 + 2048 + 16384 + 32768 = 92064 bytes (~90 KB, well under 256 KB)

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
