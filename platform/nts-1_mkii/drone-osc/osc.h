#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc.h
 *
 *  DRONE -- dual wavetable drone generator for NTS-1 mkII.
 *
 *  This header is the THIN SKELETON of the Osc class. The heavy method bodies
 *  were factored out (see the #includes at the bottom of this file) so the
 *  class definition stays small and each concern lives in its own file:
 *
 *    osc_constants.h       -- the static tuning constants (global scope)
 *    osc_param_apply.h     -- Osc::apply_edit_param()  (raw 10-bit -> DSP)
 *    osc_param_display.h   -- Osc::getParameterStrValue() (raw -> display)
 *    osc_rnd.h             -- Rnd re-roll + morph engine (rndArm/rndTick/rndMorph*)
 *    osc_lifecycle.h       -- Osc::init/reset/noteOn/noteOff/allNoteOff/setPitch
 *    osc_randomize.h       -- Osc::randomizeAll/Partial/Orbi + stableSync
 *    osc_mix.h             -- Osc::process()           (per-sample audio path)
 *
 *  The remaining building blocks are helper structs in their own headers
 *  (osc_waves.h, osc_saturator.h, osc_svf.h, osc_random_walk.h,
 *  osc_envelope.h, osc_sub2.h, osc_chord.h, osc_detune.h, osc_lfo_sim.h),
 *  and the live DSP state lives in OscParamsData (osc_params.h).
 *
 *  Sound overview: two wavetable phasors (A/B) at the base note with a static
 *  symmetric detune (DETU), a pre-drive wavefolder (FOLD) and an asymmetric
 *  soft-clip saturator (SATU/ASYM), a third "chord" voice (CHRD/CINT) with its
 *  own drive/fold (CHSAT/CHFLD), sub-octave (SUB1) and sub-2 (SUB2) voices, an
 *  internal 12 dB/oct SVF (FCUT/RESO), and four independent random-walk streams
 *  (Tone/Timbre/Density/Breath) plus static phase-FM (FM1/FM2) and their
 *  random-walk breathing (RFMA/RFMB). No dynamic
 *  allocation: all state lives in members of this class.
 *
 */

#include "processor.h"
#include "unit_osc.h"
#include "utils/float_math.h" // fastertanh2f()

// Forward declaration (defined further down, after the helper headers): a fast
// float->float floor for modular phasor wrap. It must be declared BEFORE the
// helper headers (osc_sub2.h etc.) because they call it from inline methods.
inline float fast_floorf(float x);

// Forward declaration (defined further down): 1 - e^x via a small-x Taylor
// series. Must be declared BEFORE the helper headers (osc_sub2.h etc.) because
// they call it from inline methods (K-rate coefficient setup).
inline float one_minus_expf(float x);

#include "osc_params.h"       // OscParams enum + constants + OscParamsData struct
#include "osc_waves.h"        // WaveTable: bank scanning + pitch-shifted morph
#include "osc_envelope.h"     // AhEnvelope: single-voice A-H-R envelope
#include "osc_saturator.h"    // Saturator: pre-drive wavefolder + asymmetric soft-clip
#include "osc_svf.h"          // Svf: 12 dB/oct state-variable filter (post-saturator)
#include "osc_random_walk.h"  // RandomWalk: multi-channel wandering
#include "osc_sub2.h"         // Sub2Voice: f/4 saturated, LPF'd, gated sub voice
#include "osc_detune.h"       // Detune: w0 detuning / factor (4th-order Taylor)
#include "osc_lfo_sim.h"      // LfoSim: web-sim-only shape-LFO synthesizer
#include "osc_constants.h"    // global-scope tuning constants (kSatuMax, kMix*, ...)

// Fast float->float floor used for modular phasor wrap (x - fast_floorf(x)).
// We must NOT call libm floorf(): it is emitted as a dynamic UND symbol that
// the NTS-1 mkII runtime cannot resolve ("ELF: unresolved symbol" at load).
// The trick (x - (float)(int32_t)x) floors by truncation, which is exact for
// finite x in the range where the int32_t conversion is defined (|x| < 2^31).
// Every phasor in this unit is kept in [0, 1) by the wrap itself, and the only
// inputs that ever reach this helper are phasors plus a small FM offset
// (bounded by kFmMaxCents / jitter), so the magnitude is far below 2^31 and
// the conversion is always exact. The |x| < 1e7 guard keeps the float->int32
// conversion defined (and bit-correct) in the common case; if it were ever
// exceeded (it is not, in practice) we fall back to a slow, safe path.
inline float fast_floorf(float x)
{
  // Fast path: |x| comfortably inside the exact float->int32 range.
  if (x > -1e7f && x < 1e7f)
  {
    const int32_t i = static_cast<int32_t>(x); // truncates toward zero
    const float xi = static_cast<float>(i);
    // truncation == floor for x >= 0; for negative x subtract 1 unless exact.
    return (x < 0.f && x != xi) ? xi - 1.f : xi;
  }
  // Slow, safe fallback for absurd magnitudes (never hit in practice): use the
  // FPU hardware floor if available, else a manual big-int shift. We avoid
  // libm here, so approximate with a large multiple: for |x| >= 1e7 the wrap
  // result is meaningless for audio anyway, so clamp to 0.
  return 0.f;
}

// one_minus_expf(x) = 1 - e^x, via a 3rd-order Taylor series. Used ONLY for the
// small (mildly negative) arguments this unit actually passes (|x| <= ~0.03:
// the A-H-R envelope alphas ~ -5e-5 .. -1e-4 and the sub-2 filter/gate
// coefficients ~ -0.004 .. -0.026). Accuracy over that whole range is
// |x|^4/24 <= ~3e-7 relative (worst case x = -0.026) -- far below audibility.
//
// Why not fasterexpf() / expf():
//  * expf() is libm -> an unresolved dynamic symbol -> the device rejects the
//    unit with "ELF: unresolved symbol".
//  * fasterexpf() is a bit-trick that is only accurate for "large-ish" |x|; for
//    |x| < ~1e-3 the catastrophic cancellation in (1 - fasterexpf(x)) destroys
//    it (it returned ~0.029 where the true value was ~5e-5, i.e. 300-550x too
//    large), which made the envelope attack/release collapse to a few samples
//    instead of 2 s. The series is exact to <3e-7 for our arguments and is
//    computed once at K-rate (init / update), never in the audio path.
//
// Valid for |x| small (|x| <= ~0.1 keeps the 3rd-order term adequate); all our
// call sites are well within that. For larger |x| the series would lose
// accuracy, but we never call it there.
inline float one_minus_expf(float x)
{
  // 1 - e^x = -x - x^2/2 - x^3/6 - ...
  const float x2 = x * x;
  return -x - x2 * 0.5f - x2 * x / 6.0f;
}

class Osc : public Processor
{
  // The wavetable scanner (osc_waves.h) is a flat helper struct; let it read
  // our private state (params_, the phasor w0) without exposing it.
  friend struct WaveTable;

public:
  uint32_t getBufferSize() const override final { return 0; } // NTS-1 osc do not support sdram allocation
  // ---- Audio parameters ----------------------------------------------------
  // PARAM_SEL / PARAM_VAL are the user-facing header slots (header.c slots 0/1
  // and 2/3). DETUNE..CHORD are the 12 internal DSP parameters that the
  // multiplexer edits.
  enum
  {
    PARAM_SEL = 0U, // knob A (header slot 0): parameter selector 0..15, shows name
    PARAM_VAL,      // knob B (header slot 1): raw value 0..1023 of PARAM_SEL, shows value
    RATE_AMT,       // SHARED random-walk depth ceiling for all three streams (0..100%, default 0% = off). FIRST editable param (select_ 0) -- it is the most-edited "chaos" lever, so it leads the multiplexer.
    DETUNE,         // static symmetric detune of A/B (−10..+10 cents, A=+c/2, B=−c/2, default 0). select_ 1.
    // Three independent wandering streams (Tone / Timbre / Density). Each has
    // its own period (SPDx) and time constant (SMTx), so they are not
    // phase-locked. See osc_params.h for the per-stream targets each drives.
    SPD1,           // Tone stream period   (0..100% -> 0.1 s..10 s, log, default 50%)
    SMT1,           // Tone stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SPD2,           // Timbre stream period  (0..100% -> 0.1 s..10 s, log, default 50%)
    SMT2,           // Timbre stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SPD3,           // Density stream period (0..100% -> 0.1 s..10 s, log, default 50%)
    SMT3,           // Density stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SUB1,           // sub-octave sine level (0..100%, default 50%)
    SUB2,           // second sub (f/4) very-soft-saturated level (0..100%, default 0% = off)
    SATU,           // saturator drive (0..3.0, default 2.0 = moderate)
    ASYM,           // asymmetric saturator (−100%..+100%, default 0% = symmetric)
    FOLD,           // pre-drive wavefolder depth (0..100%, default 0% = off)
    WAV1,           // oscillator A wavetable BANK (raw 0..1023 -> bank A..F, 0..5; the internal wave-LFO 1 morphs within the bank)
    WAV2,           // oscillator B wavetable BANK (raw 0..1023 -> bank A..F, 0..5; the internal wave-LFO 2 morphs within the bank). Was deprecated (A/B shared one wave); revived as the second wave stream for the 2D crossfade (see WAVE_MIX).
    // (WAV3 / CHORD / CHRD_INT were removed — the chord/interval concept is
    //  gone; the main A+B voice + SUB1/SUB2 remain.)
    MAIN,           // main (A+B mixed) voice level (0..100%, default 100% = full)
    RW_WAV,         // random-walk depth on wavetable morph (0..100%, default 0% = off)
    RW_DRV,         // random-walk depth on saturator drive (0..100%, default 0% = off)
    RW_FLD,         // random-walk depth on wavefolder depth (0..100%, default 0% = off)
    FCUT,           // internal SVF cutoff + dry/wet (log 80 Hz..12 kHz, 0% = raw, 100% = LPF)
    RES,            // internal SVF resonance Q (0..100% -> 0.5..2.5, default 0% = 0.5)
    RW_FC,          // random-walk depth on SVF cutoff (0..100%, default 0% = off)
    RW_LVL,         // breath: per-voice level swing depth on the MAIN voice (0..100%, default 0% = off, INDEPENDENT of RATE)
    RW_LSP,         // breath stream period (0..100% -> 0.1 s..10 s, log, default 50%) — how often each voice's breath target is redrawn
    RW_LSM,         // breath stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    // ---- Enrichment: jitter, ring-mod, phase-FM ----
    // (CHSAT / CHFLD / CINT2 were removed — the chord/interval concept is gone.)
    JIT,            // random phase jitter on A/B (0..100% -> 0..kJitMaxCycles peak, default 0 = off)
    RMOD,           // A*B ring-mod derived voice, ducked by MAIN (0..100%, default 0 = off)
    FM_A,           // STATIC phase-FM on osc A (B modulates A): raw 0..1 depth, quadratic curve -> 0..kFmMaxCents/2 cycles (no RATE needed)
    FM_B,           // STATIC phase-FM on osc B (A modulates B): raw 0..1 depth, quadratic curve -> 0..kFmMaxCents/2 cycles (no RATE needed)
    // RFM_A / RFM_B: random-walk DEPTH on the FM index (the "откуда гулять" knob,
    // like RDRV on SATU or RWAV on WAV1). The walk deviation (rw_[0]) scales
    // RFM and is gated by the shared RATE ceiling, so RFM adds a "breathing"
    // on top of the static FM index. RFM=0 -> pure static FM (no wandering).
    RFM_A,          // random-walk depth on FM A index (0..100%, RATE-gated)
    RFM_B,          // random-walk depth on FM B index (0..100%, RATE-gated)
    // ---- Enrichment-2 (formant / comb) --------------------------------------
    // (CHSAT2 / CHFLD2 were removed — the chord/interval concept is gone.)
    // All default to OFF (transparent), so the stock sound is bit-identical
    // until the user turns one on. NOTE: FOLD2 (cascaded wavefolder) and DCK
    // (sidechain-duck amount) were REMOVED — DCK is now a CONSTANT (see
    // kDuckConstant in osc_constants.h) and FOLD2 was judged unnecessary.
    FORM,           // formant vowel position (0..1, morph across A-E-I-O-U)
    FVOL,           // formant stage dry/wet (0..1, 0 = dry / transparent)
    COMB,           // comb resonator feedback (0..1 -> 0..kCombFbMax)
    SUBB,           // sub-bass "breath" depth (0..100% -> 0..kSubBreathMax): the SHARED sub bass (SUB1/SUB2) level follows the loudest (breath-focus) voice. 0% = static low end (regression-safe).
    // ---- Internal WAVE LFO (independent of the platform shape-LFO) ----------
    // The WAVETABLE intra-bank morph is driven by an INTERNAL LFO, not the
    // platform shape-LFO (the hardware's OSC-button RATE/SHAPE knobs are now
    // NO-OPs -- setShapeLfo() is a stub; see below). The LFO synthesizes a
    // phase-coherent sine at audio rate (ShapeLfoSim, osc_lfo_sim.h) and drives
    // the morph position within the selected WAVE bank. These two knobs give it
    // its own frequency / depth, decoupled from the platform so future LFO
    // sources can be bound here without touching the hardware knobs:
    //   WAVE_LFO_RATE  -- LFO frequency (LOG scale: a LINEAR knob mapped to an
    //                     EXPONENTIAL frequency; raw 0 = 0 Hz / LFO OFF, else
    //                     10^(3*raw/1023 - 3) = 0.001..1 Hz, 3 decades;
    //                     default 0 = OFF). The hardware's RATE
    //                     knob stepped in 0.1 Hz, too coarse for slow organic
    //                     morphs.
    //   WAVE_LFO_DEPTH -- LFO morph depth (LINEAR 0..100%, default 100%). At
    //                     100% the morph sweeps the full bank; 0 = static wave.
    WAVE_LFO_RATE,  // internal wave-LFO frequency (log 0..1 Hz, default 0 = off)
    WAVE_LFO_DEPTH, // internal wave-LFO morph depth (0..100%, default 100%)
    // ---- Second wave stream (2D morph) -------------------------------------
    // The audio path now runs TWO independent wave streams and crossfades them
    // (see the "2D wave crossfade" block in osc_mix.h): stream 1 is the original
    // WAV1 bank + WAVE_LFO_RATE/DEPH; stream 2 is the WAV2 bank + the two
    // parameters below. Each stream morphs within its own bank under its own
    // LFO (independent phase), so with two LFOs at incommensurate periods
    // (e.g. 13 s and 19 s) the (x, y) trajectory in the 2D texture space never
    // repeats while the sound stays perfectly smooth (interpolation happens
    // both inside the banks and between the two streams via the crossfade).
    //   WAVE2_LFO_RATE  -- stream-2 LFO frequency (same LOG scale as
    //                      WAVE_LFO_RATE; raw 0 = 0 Hz / LFO OFF, default OFF)
    //   WAVE2_LFO_DEPTH -- stream-2 LFO morph depth (LINEAR 0..100%, default
    //                      100%: full-bank sweep, same as stream 1)
    //   WAVE_MIX -- the stream-1 <-> stream-2 crossfade weight, SYMMETRIC
    //               -100%..+100% (raw 511 = 50/50, default). x = 0.5*(1+mix):
    //               raw 0   -> x = 0   (stream 1 only, stream 2 silent)
    //               raw 511 -> x = 0.5 (equal 50/50 blend)
    //               raw 1023-> x = 1   (stream 2 only, stream 1 silent)
    WAVE2_LFO_RATE,  // stream-2 wave-LFO frequency (log 0..1 Hz, default 0 = off)
    WAVE2_LFO_DEPTH, // stream-2 wave-LFO morph depth (0..100%, default 100%)
    WAVE_MIX,        // wave 1 <-> wave 2 crossfade (-100%..+100%, default 50/50)
    // ---- LFO shape (per stream) ---------------------------------------------
    // Each wave stream's LFO can be: 0 = SIN (classic sine, default),
    // 1 = S (S-curve warp), 2 = WALK (random-walk scene, regenerated every
    // cycle). WALK_GAP is the mean seconds between control points (LOG 1..300 s,
    // default 30 s); only used in WALK mode.
    WAVE_LFO_SHAPE,  // stream-1 LFO shape (0=SIN, 1=S, 2=WALK; default SIN)
    WAVE_LFO_GAP,    // stream-1 WALK: mean seconds between points (LOG 1..300 s)
    WAVE2_LFO_SHAPE, // stream-2 LFO shape (0=SIN, 1=S, 2=WALK; default SIN)
    WAVE2_LFO_GAP,   // stream-2 WALK: mean seconds between points (LOG 1..300 s)
    // ---- Wave-Mix LFO (third internal LFO, see osc_lfo_sim.h) ----------------
    // WAVE_MIX (index 39) is the CENTRAL crossfade between stream 1 (WAVE bank)
    // and stream 2 (WA2 bank): it is the "initial balance" the sound sits on.
    // This LFO MODULATES that balance: the audio path computes
    //   mix_eff = clamp(WMIX + lfo_mix_.process(), -1, +1)
    //   x = 0.5 * (1 + mix_eff)
    // so the static WMIX knob is the CENTER and the LFO (shape * depth, -1..+1)
    // is the SWING around it. At the factory default (RATE = 0, LFO OFF) the
    // mix is fully static (bit-identical to the pre-LFO sound). Same three
    // parameters and scales as the two per-stream wave-LFOs:
    //   WMIX_LFO_RATE  -- LFO frequency (LOG: raw 0 = 0 Hz / LFO OFF, else
    //                     10^(3*raw/1023 - 3) = 0.001..1 Hz; default 0 = OFF)
    //   WMIX_LFO_DEPTH -- swing depth (LINEAR 0..100%, default 100%: a full
    //                     -100%..+100% swing of the crossfade weight)
    //   WMIX_LFO_SHAPE -- trajectory (0=SIN default, 1=S, 2=WALK)
    //   WMIX_LFO_GAP   -- WALK mean gap (LOG 1..300 s, default ~30 s; only used
    //                     when shape = WALK)
    WMIX_LFO_RATE,  // wave-mix LFO frequency (log 0..1 Hz, default 0 = off)
    WMIX_LFO_DEPTH, // wave-mix LFO swing depth (0..100%, default 100%)
    WMIX_LFO_SHAPE, // wave-mix LFO shape (0=SIN, 1=S, 2=WALK; default SIN)
    WMIX_LFO_GAP,   // wave-mix WALK: mean seconds between points (LOG 1..300 s)
    // ---- FM modulator C: an INDEPENDENT oscillator that phase-FMs the
    // (A+B) SUM (the drone carrier). Classic FM: C is the modulator, A+B is
    // the carrier. C is NOT heard by itself (pure phase modulation, like the
    // DX7 modulator) -- it only bends the sum's phase for vibrato / motion
    // colour on top of the (untouched) cross-FM A+B timbre. At FM_CR = 0
    // (default) C is off and the stock sound is bit-identical.
    FM_CR,          // C frequency (LOG 0..1 Hz; raw 0 = OFF)
    FM_CW,          // C wave shape (0=SIN, 1=SAW, 2=SQR, 3=NOISE)
    FM_CD,          // C depth (LINEAR 0..100% -> 0..kFmCMaxCycles phase peak)
    // ---- FM modulator S (static teeth): a CARRIER-LOCKED FM oscillator ------
    // S's rate is locked to the (A+B) carrier: FMST sets N teeth per carrier
    // cycle (LOG kFmStaticMinTeeth..kFmStaticMaxTeeth; raw 0 = OFF). Teeth < 1
    // = sub-octave (fm/fc < 1, a deep organ-like comb); 1 = in-tune. The audio
    // path advances an S phasor by N x the A phasor advance per sample and
    // phase-FMs the (A+B) sum with N teeth of deviation per cycle (a static
    // harmonic comb, the static counterpart of C's free-running vibrato).
    FM_STATIC,      // S teeth per carrier cycle (LOG 0.05..16; raw 0 = OFF; <1 = sub-octave)
    // FMSA (amplitude): the SIZE (peak swing) of each tooth. LINEAR 0..100% ->
    // 0..kFmStaticMaxCycles peak phase deviation. At 0 (or FMST = 0) S is off
    // (bit-identical). 0.05 cycles (5% of the carrier) is the full-scale peak.
    FM_SAMP,        // S tooth amplitude (LINEAR 0..100% -> 0..kFmStaticMaxCycles peak)
    NUM_PARAMS
  };

  // ---- Multiplexer constants -----------------------------------------------
  // Number of editable DSP parameters (RATE_AMT..WMIX_LFO_GAP).
  static constexpr uint8_t kEditParamCount = 53; // RATE_AMT..FM_SAMP
  // The first editable enum value. The UI (header.c PARAM slot, 0..33) expects
  // this order (select_ index -> parameter), 34 parameters total:
  //   0 RATE (RATE_AMT, shared depth ceiling)  1 DET (DETUNE)
  //   2 SPD1             3 SM1   (Tone stream period / smooth)
  //   4 SPD2             5 SM2   (Timbre stream period / smooth)
  //   6 SPD3             7 SM3   (Density stream period / smooth)
  //   8 SUB1             9 SUB2  10 SAT    11 ASYM
  //   12 FLD             13 WA1  14 WA2
  //   15 MAIN (main A+B voice level)
  //   16 TWDR            17 DWDR 18 FWDR
  //   19 CUT  (SVF cutoff + dry/wet)  20 RES (SVF resonance Q)
  //   21 WFC  (RW_FC: random-walk depth on SVF cutoff)
  //   22 BRT  (RW_LVL: breath depth on voice levels)
  //   23 BRR  (RW_LSP: breath stream period)  24 SMT (RW_LSM: breath smooth)
  //   25 JIT (random phase jitter on A/B)
  //   26 RMOD (A*B ring-mod derived voice, MAIN-ducked)
  //   27 FM1 (FM_A: STATIC phase-FM on A by B)
  //   28 FM2 (FM_B: STATIC phase-FM on B by A)
  //   29 FMAB (RFM_A: random-walk depth on FM A, RATE-gated)
  //   30 FMBB (RFM_B: random-walk depth on FM B, RATE-gated)
  //   31 FMVT (formant vowel position)  32 FLVL (formant dry/wet)
  //   33 COMB (comb resonator)
  //   34 SUBB (shared sub-bass breath depth, follows the breath-focus voice)
  //   35 WLFO R (WAVE_LFO_RATE: internal wave-LFO 1 frequency, log 0..1 Hz)
  //   36 WLFO D (WAVE_LFO_DEPTH: internal wave-LFO 1 morph depth, 0..100%)
  //   37 W2LFO R (WAVE2_LFO_RATE: wave-LFO 2 frequency, log 0..1 Hz)
  //   38 W2LFO D (WAVE2_LFO_DEPTH: wave-LFO 2 morph depth, 0..100%)
  //   39 WMIX (WAVE_MIX: wave 1 <-> wave 2 crossfade, -100%..+100%, 50/50 def)
  //   40 WLFO SHP (WAVE_LFO_SHAPE: stream-1 LFO shape, 0=SIN/1=S/2=WALK)
  //   41 WLFO GAP (WAVE_LFO_GAP: stream-1 WALK mean gap, log 1..300 s)
  //   42 W2LFO SHP (WAVE2_LFO_SHAPE: stream-2 LFO shape, 0=SIN/1=S/2=WALK)
  //   43 W2LFO GAP (WAVE2_LFO_GAP: stream-2 WALK mean gap, log 1..300 s)
  //   44 WMXR (WMIX_LFO_RATE: wave-mix LFO frequency, log 0..1 Hz)
  //   45 WMXD (WMIX_LFO_DEPTH: wave-mix LFO swing depth, 0..100%)
  //   46 WMXS (WMIX_LFO_SHAPE: wave-mix LFO shape, 0=SIN/1=S/2=WALK)
  //   47 WMXG (WMIX_LFO_GAP: wave-mix WALK mean gap, log 1..300 s)
  // PARAM_SEL / PARAM_VAL are the two multiplexer knobs (not editable params).
  // kEditParamCount = 48 = (WMIX_LFO_GAP - RATE_AMT + 1);
  // select_ 0..47 -> RATE_AMT..WMIX_LFO_GAP.
  // kEditParamFirst = RATE_AMT so RATE leads the multiplexer (select_ 0).
  static constexpr uint8_t kEditParamFirst = RATE_AMT;
  // ---- Runtime DSP parameter struct (see osc_params.h) ---------------------
  using Params = ::OscParamsData;

  // ---- Multiplexer state (select_ / edit_value_) --------------------
  // edit_value_[i] is the raw 10-bit cache of editable parameter i, so
  // re-selecting a parameter restores its last value. Defaults match the
  // DSP defaults in OscParamsData::reset() (osc_params.h).
  // MAIN 1023 (full), RW_WAV 0, RW_DRV 0, RW_FLD 0, FCUT 921 (~90%), RES 0,
  // RWFC 0, RW_LVL 0 (breath off), RW_LSP 511, RW_LSM 511.
  // Enrichment-2 defaults (indices 31..33): FORM 511 (middle vowel, but the
  // formant stage is OFF until FVOL > 0), FVOL 0 (dry / transparent),
  // COMB 0 (all off, regression-safe). SUB2 (index 9) defaults to 410 (~40%).
  // SUBB (index 34) defaults to 307 (~30%): the shared sub bass breathes
  // gently (depth 0.18) by default — the low end follows the breath-focus
  // note subtly instead of staying fully static.
  // WAVE_LFO_RATE (index 35) defaults to 0: raw 0 = 0 Hz (LFO OFF -- the wave
  // stays at the bank center, static). Turning the knob up sweeps 0.001..1 Hz
  // on a LOG scale (10^(3*raw/1023 - 3), 3 decades, finer near 0). WAVE_LFO_DEPTH
  // (index 36) defaults to 1023 (100%): a full-bank sweep, so any non-zero
  // frequency drives the morph across the whole bank. WAVE2_LFO_RATE (index 37)
  // defaults to 0 (stream-2 LFO OFF) and WAVE2_LFO_DEPTH (index 38) to 1023
  // (100%), mirroring stream 1. WAV2 (index 14) defaults to 0 (bank A, same as
  // WAV1) and WAVE_MIX (index 39) to 511 (50/50): at the factory preset stream 2
  // is the SAME bank A with its LFO off, so the 50/50 crossfade of two
  // identical waves is bit-identical to the old single-stream sound.
  // Order = select_ order (kEditParamFirst = RATE_AMT): index 0 = RATE (0 =
  // off), index 1 = DET (511 = 0 c). RATE leads the multiplexer.
  // 48 values (kEditParamCount = 48, RATE_AMT..WMIX_LFO_GAP): indices 0..43
  // keep the old values (13 (WAV1) = 0 bank A, 14 (WAV2) = 128 bank A, 35 (WLFR)
  // = 0, 36 (WLFD) = 1023); 37 (W2LFR) = 0 (stream-2 LFO off); 38 (W2LFD) =
  // 1023 (100%); 39 (WMIX) = 511 (50/50); 40 (WLFS) = 0 (Sin, default);
  // 41 (WLFG) = 511 (~30 s, default walk gap); 42 (W2FS) = 0 (Sin); 43 (W2FG)
  // = 511 (~30 s); 44 (WMXR) = 0 (wave-mix LFO OFF — static mix, regression-
  // safe); 45 (WMXD) = 1023 (100% swing); 46 (WMXS) = 0 (Sin); 47 (WMXG) =
  // 511 (~30 s); 48 (FM_CR) = 0 (FM modulator C OFF — the (A+B) sum is
  // unmodulated, regression-safe); 49 (FM_CW) = 0 (SIN); 50 (FM_CD) = 0 (OFF).
  // At the factory preset the wave-mix LFO is OFF (rate = 0 Hz), so the
  // crossfade stays at the static WMIX value — bit-identical to the pre-LFO
  // sound; and FM modulator C is OFF (rate = 0), so the sum is bit-identical
  // to the pre-FM-C sound.
  int32_t edit_value_[kEditParamCount] = {0, 511, 511, 511, 511, 511, 511, 511, 511, 410, 341, 511, 0, 0, 128, 1023, 0, 0, 0, 921, 0, 0, 0, 511, 511, 0, 0, 0, 0, 0, 0, 511, 0, 0, 307, 0, 1023, 0, 1023, 511, 0, 511, 0, 511, 0, 1023, 0, 511, 0, 0, 0, 0, 1023};

  // Last STABLE state (RndM = ORBI reference): a raw-value snapshot of the
  // sound that every ORBI re-roll develops from. It is set by init() (factory
  // defaults -- the default initializer mirrors edit_value_ exactly, so it
  // already matches at startup) and is REWRITTEN by every All/Part re-roll
  // (they stabilize on their result). ORBI re-rolls read it but never write
  // it, so an ORBI session stays anchored to the last stable sound. Manual
  // PARAM/VALUE edits do NOT affect it (a manual tweak is not a "stable
  // state" until an All/Part re-roll or reset confirms it). Keep the default
  // list in sync with edit_value_ above.
  int32_t stable_value_[kEditParamCount] = {0, 511, 511, 511, 511, 511, 511, 511, 511, 410, 341, 511, 0, 0, 128, 1023, 0, 0, 0, 921, 0, 0, 0, 511, 511, 0, 0, 0, 0, 0, 0, 511, 0, 0, 307, 0, 1023, 0, 1023, 511, 0, 511, 0, 511, 0, 1023, 0, 511, 0, 0, 0, 0, 1023};

  // Currently selected parameter (0..kEditParamCount-1), i.e.
  // edit_value_[select_] is what PARAM_VAL edits. 0 = RATE (default, the
  // leading "chaos" lever).
  uint8_t select_ = 0;

  // Public accessors for the web simulator (wasm.cc) to read the selector
  // cache state without going through the audio worklet thread.
  uint8_t getSelect() const { return select_; }
  int32_t getEditValue(uint8_t idx) const
  {
    return (idx < kEditParamCount) ? edit_value_[idx] : 0;
  }

  // Cached value of the currently selected parameter (PARAM_VAL). Used by the
  // web simulator's VALUE slider init/display (wasm.cc unit_get_param_value).
  int32_t getValue() const { return edit_value_[select_]; }

  // Re-roll every edit parameter to a fresh audition-friendly random preset and
  // apply all of them to the live DSP. Used by the Rnd debounce (rndTick, in
  // osc.h) which is armed by:
  //   * the hardware "Rnd" optional slot (unit.cc -> rndArm()), and
  //   * the web simulator's *Random* button (rndRequest() -> rndArm()), so both
  //     share the exact same random windows / LCG and the same 0.5 s debounce.
  // Only ever called from the audio-worklet thread (rndTick), at block rate.
  void randomizeAll();

  // Partial re-roll: randomize a RANDOM SUBSET (2..6) of the edit parameters to
  // fresh values drawn from the SAME audition-friendly "safe windows" as
  // randomizeAll(), leaving the rest untouched (a "development" rather than a
  // full reset). Each changed parameter is guaranteed a SIGNIFICANT step: the
  // new raw must differ from the current raw by >= kRndMinStepRaw (102, ~10%
  // of the 10-bit range), re-drawing up to kRndRedrawMax times (see
  // osc_randomize.h). Used when rnd_mode_ == kRndModePart (the RndM selector).
  // Only ever called from the audio-worklet thread (rndTick), at block rate.
  void randomizePartial();

  // Orbit re-roll (RndM = ORBI): a TIGHT, SHORT "development" of the LAST STABLE
  // STATE (stable_value_), deliberately gentler than randomizePartial(): it
  // nudges 2..4 parameters, each picked from ONE weighted family
  // (core/timbre/motion/extra, with a small surprise chance) so a single fire
  // develops one "sphere" of the sound, and each nudge is a few % .. ~30% of
  // range (see kRndOrbi* in osc_randomize.h) rather than a fresh re-draw -- so
  // the result is recognizably the same sound, developed. The whole fire morphs
  // over a SHORT phrase (a quarter bar, not Part's full bar) so the shift lands
  // quickly and is clearly audible. The base for every nudge is stable_value_
  // (not the current edit_value_), and the result is applied to the live
  // edit_value_ WITHOUT touching stable_value_ -- so repeated ORBI re-rolls
  // always develop from the same stable sound. The stable state is set by
  // init() (factory defaults) and by every All/Part re-roll. Only ever called
  // from the audio-worklet thread (rndTick), at block rate.
  void randomizeOrbi();

  // Re-copy the live edit_value_ into the stable-state snapshot. Used by the
  // web simulator's UI-thread preset path (All Random / Partial Random /
  // Reset go through applyPresetRaw -> setEditParam, NOT through the audio
  // re-roll that would stabilize on its own), so the C++ stable state stays
  // in sync with what the sim is actually showing.
  void stableSync();

  // Draw one edit parameter's fresh raw value from its safe window (see
  // rnd_window_for() in osc_randomize.h). Shared by randomizeAll() (every
  // param) and randomizePartial() (a subset). Audio-thread only.
  int32_t rnd_draw(uint8_t idx);

  // Orbit helpers (see the Orbit tuning comment in osc_randomize.h): pick a
  // random edit parameter for one ORBIT fire (from ONE weighted family, with a
  // small "surprise" chance to touch any parameter), and draw a gentle NUDGE of
  // `base` (a step within kRndOrbiMinStepRaw..kRndOrbiMaxStepRaw, not a fresh
  // window draw). Audio-thread only.
  uint8_t  rnd_pick_orbi_param();
  int32_t  rnd_draw_orbi(uint8_t idx, int32_t base);

  // (ensure_cint_separation was removed — the chord/interval concept is gone.)

  // Randomizer primitive used by randomizeAll(): returns a uniform integer in
  // 0..count-1 from one osc_white() sample (integer math, no float/libm --
  // see osc_randomize.h for the derivation).
  int32_t rnd_int(uint32_t count) const;
  // ---- Rnd debounce state (both builds; bodies in osc_rnd.h) --------------
  volatile uint32_t rnd_now_ = 0;
  volatile uint32_t rnd_cooldown_until_ = 0;
  volatile uint8_t rnd_fire_ = 0;   // 0/1: immediate re-roll pending
  volatile uint8_t rnd_armed_ = 0;  // 0/1: deferred (final) re-roll pending

  // Rnd debounce window in samples: 0.5 s at the fixed 48 kHz rate
  // (24000 / 48000 = 0.5 s). Used by the hardware Rnd slot only.
  static constexpr uint32_t kRndDebounceSamples = 24000U;
  // Method bodies live in osc_rnd.h (included at the bottom of this file).
  void rndArm(bool debounced = true);
  void rndMorphStart(const int32_t *target, uint32_t phrase_beats = 0U);
  void rndMorphTick(uint32_t frames);
  void rndTick(uint32_t frames);

  // ---- Rnd randomization TYPE (header slot 4 "RndM") ----------------------
  static constexpr uint8_t kRndModeAll  = 0U;
  static constexpr uint8_t kRndModePart = 1U;
  static constexpr uint8_t kRndModeOrbi = 2U;
  volatile uint8_t rnd_mode_ = kRndModeAll;

  uint8_t getRndMode() const;
  void setRndMode(uint8_t mode);

  // The header slot 5 "Rnd" (0..15, enum) is the randomization TRIGGER (it
  // fires on any value 1..15, debounced). This is only for display/getter
  // parity; the trigger itself has no DSP state, so unit.cc reports the last
  // raw value it was written (cached_values[5]).
  uint8_t getRndRun() const;
  void setRndRun(uint8_t v);
  volatile uint8_t rnd_run_ = 0;
#ifdef __EMSCRIPTEN__
  // ---- Web-sim Rnd re-roll request channel (main-thread UI -> audio worklet) ----
  // The web simulator's All/Part/Orbi buttons run on the MAIN thread (UI) but
  // must fire the re-roll on the AUDIO-WORKLET thread (rndTick, in process()),
  // because the worklet's processor is a SEPARATE COPY (see wasm.cc) that
  // owns the live DSP state (edit_value_, stable_value_, morph_*). A plain
  // rndRequest() on the main-thread processor would set its own rnd_fire_
  // flag, which the worklet never reads -- so the re-roll would never fire.
  //
  // This channel packs (mode, valid) into a single 32-bit slot the worklet
  // consumes once per process() block:
  //   * bit 31   = valid flag
  //   * bits 0..2 = mode (kRndModeAll=0 / kRndModePart=1 / kRndModeOrbi=2)
  //   * bits 3..30 = reserved (0)
  // A 32-bit aligned store on x86 is atomic (same argument as sim_edit_request_).
  // Body: osc_rnd.h.
  volatile uint32_t sim_rnd_request_ = 0;
  void rndRequest();
  void simRndRequest(uint8_t mode);

  // ---- Direct edit-parameter channel (web simulator only) -------------------
  // On hardware the PARAM/VALUE multiplexer is driven by the platform's
  // unit_set_param_value() callback (K-rate, atomic per knob turn). The web
  // simulator's per-parameter sliders bypass WebAudioParam entirely and write a
  // packed (index, raw) request to this single 32-bit slot. The request is
  // consumed once per block by process() (audio worklet thread), so the
  // (select_, PARAM_VAL) pair is applied atomically -- the same write
  // semantics the hardware has. Layout: bit 31 = valid flag, bits 0..5 =
  // index, bits 8..17 = raw value (0..1023). A 32-bit aligned store on x86
  // is atomic. The hardware build does not use this channel: under
  // non-EMSCRIPTEN the volatile slot is absent (zero size) and process()
  // never touches it. Body: osc_rnd.h.
  volatile uint32_t sim_edit_request_;
  void simEditRequest(uint8_t index, int32_t raw);
#endif

  // ---- Rnd re-roll MORPH state (BPM-synced smooth transition) ----------------

  // ---- Rnd re-roll MORPH (BPM-synced smooth transition) ----------------------
  // The Rnd re-rolls (All / Part / Orbi) do not apply their new values
  // instantly: every affected edit parameter morphs from its OLD raw value to
  // the NEW one over a BPM-synced phrase (kRndMorphBeats beats, 4 bars of 4/4
  // = 8 s at the default 120 BPM), each with its own random START DELAY
  // (stagger, 0..kRndMorphStaggerDivs-1 subdivisions of the phrase) and a
  // smoothstep (3t^2 - 2t^3) curve, so the parameters "roll in" one after
  // another instead of all at once (see the constants in osc_constants.h and
  // rndMorphStart/rndMorphTick below). The morph runs in the AUDIO thread
  // (rndMorphTick, once per process() block, K-rate): it re-applies the
  // in-flight parameters through the SAME apply_edit_param() path a manual
  // PARAM_VAL edit uses, so the derived-voice caches (wavetables, chords, SVF
  // coefficients, formant combs) stay consistent with the moving raw values,
  // exactly as during a knob turn. Discrete parameters morph in their own
  // native space for free (a fractional wavetable index crossfades via
  // resolve_wave()). (fractional chord cents glide was removed — the chord/
  // interval concept is gone.)
  //
  //   morph_active_     0/1: a morph is in flight (rndMorphTick early-outs on
  //                       a single branch check when idle).
  //   morph_elapsed_    samples since the morph started (audio clock).
  //   morph_total_smp_  morph phrase length in samples (kRndMorphBeats at the
  //                       fire-time tempo).
  //   morph_from_[i]    raw value the param morphs FROM (the old value, or the
  //                       current interpolated value on a re-aim mid-morph).
  //   morph_to_[i]      raw value the param morphs TO (the re-roll target).
  //   morph_start_[i]   per-parameter start delay in samples (stagger).
  //   morph_cur_[i]     raw value currently applied (for re-aim + UI read).
  //   morph_t_[i]       the param's CURRENT progress in 0.0..1.0 (0 = not
  //                       started, 1 = at target). Updated each block by
  //                       rndMorphTick; read by rndMorphStart on a re-aim to
  //                       steer the param toward its new target WITHOUT a
  //                       velocity discontinuity (C1 continuity, see below).
  //   morph_idx_[i]     i if param i is active in this morph, else 0xFF.
  //   morph_cnt_        number of active params (<= kEditParamCount).
  int32_t  morph_from_[kEditParamCount];
  int32_t  morph_to_[kEditParamCount];
  uint32_t morph_start_[kEditParamCount];
  int32_t  morph_cur_[kEditParamCount];
  float    morph_t_[kEditParamCount];
  uint8_t  morph_idx_[kEditParamCount];
  uint8_t  morph_cnt_ = 0;
  uint32_t morph_total_smp_ = 0;
  uint32_t morph_elapsed_ = 0;
  uint8_t  morph_active_ = 0;
  int32_t getEditMorphValue(uint8_t idx) const; // body: osc_rnd.h
  bool isMorphActive() const; // body: osc_rnd.h

  // Raw value of edit parameter i as the web simulator's UI thread reads it
  // (the slider-sync loop): while a morph is in flight this is the CURRENT
  // interpolated raw value, so the sim's sliders visibly "travel" toward the
  // re-roll target instead of jumping. Returns the plain edit_value_ when no
  // morph is active, OR when this parameter is NOT part of the active morph
  // (unchanged by the re-roll): for such a parameter morph_cur_[i] is stale
  // garbage (only re-targeted params get it (re)seated in rndMorphStart), and
  // its true value is simply edit_value_[i] (the morph leaves it untouched).
  // Read-only for the UI (audio thread writes morph_cur_ / morph_idx_).

  // Rnd re-roll machinery (debounce state, morph engine, rndTick, the web-sim
  // request channels and the RndM mode state) lives in osc_rnd.h, included at
  // the bottom of this file, after the class is complete.

  // random-walk maximum deviation in cents at RATE AMT = 100% (full knob).
  // The detune never strays further from the DETUNE knob value than this.

  // Fixed sub-octave detune: the sub sine runs slightly flat (−4 cents) so
  // it "breathes" against the saws instead of locking to them. A few cents
  // at one octave below gives a slow, organic beat without sounding sloppy.
  static constexpr float kSubDetuneCents = -4.f;

  // LFO → FM depth on the sub phasor. The platform shape-LFO arrives in
  // lfo_ in (-1, 1); we scale it to ±5 cents of frequency modulation on the
  // sub-octave sine, giving it a subtle "wobble" the external LFO module
  // can't reach (it only sees the whole unit, not the internal sub voice).
  static constexpr float kSubLfoFmCents = 5.f;

  // (The LFO -> intra-bank wavetable morph depth lives in
  //  WaveTable::lfo_morph_depth / kLfoMorphDepthHalfSpan, see osc_waves.h.)

  // ---- Static symmetric detune (DETU knob, ±kDetuneMaxCents = ±10 c) ----
  // The DETU knob detunes A and B in OPPOSITE directions around the base
  // note: w0_A = w0 * 2^(+c/2 / 1200), w0_B = w0 * 2^(-c/2 / 1200). This
  // produces a per-voice phase drift (the inter-voice beat frequency is
  // c * f / 600, twice the sum-detune rate) — the audible chorus depth is
  // ~4x stronger than the post-mix sum detune at the same knob value,
  // matching the old two-knob DET1/DET2 feel at moderate settings. The
  // Taylor factors are cached in K-rate (update_w0_det) when the random

  // Map a raw 10-bit value (0..1023) for one of the 32 editable
  // parameters (DETUNE..CHFLD) onto the DSP state. This is the single
  // source of truth for the parameter scaling: it is used both by the
  // edit-menu path (setParameter(DETUNE..CHFLD)) and by the PARAM_VAL
  // multiplexer path. The body lives in osc_param_apply.h.
  void apply_edit_param(uint8_t idx, int32_t value);
  // ---- Polyphonic voice cluster (kVoiceCount = 6) ---------------------------
  // The drone is a single-voice instrument whose ONE voice plays up to
  // kVoiceCount notes AT THE SAME TIME (a "cluster"). Every note in the
  // cluster shares IDENTICAL settings: the same wavetables (WAV1/WAV2),
  // DETU, SATU/ASYM/FOLD, the random walks, the SVF, the formant, the comb
  // and the sidechain duck. What differs per note is ONLY the pitch (its own
  // w0), the phasors, the A-H-R envelope and the sub-voice states.
  // LATCH is ALWAYS ON: a note-on starts (or retargets) a cluster note; a
  // note-off is a no-op (the note is held); re-pressing the SAME note toggles
  // it off. (No glide/portamento: each new note starts at its exact pitch with
  // a random per-voice start phase to de-correlate the inter-voice beats.)
  //
  // The audio path (osc_mix.h) runs a per-voice loop over these (phasors ->
  // wavetables -> sub/sub-2 -> A-H-R) and feeds a SINGLE common post-stage
  // (wavefolder -> saturator -> SVF -> formant -> comb -> duck -> makeup +
  // soft-clip) so the kVoiceCount notes read as one blended drone, not kVoice
  // independent instruments.
  static constexpr uint8_t kVoiceCount = 6U;
  struct Voice
  {
    float   w0 = 0.f;         // current base phase increment (glide removed: always fixed per note)
    float   ph_a = 0.f;       // main osc A phasor
    float   ph_b = 0.f;       // main osc B phasor
    float   prev_wav_a = 0.f; // FM lookback cache (A)
    float   prev_wav_b = 0.f; // FM lookback cache (B)
    float   fm_smooth_a = 0.f; // phase-FM index LPF state (A)
    float   fm_smooth_b = 0.f; // phase-FM index LPF state (B)
    float   breath_dev = 0.f;    // per-voice breath deviation [-1, +1] (tracks breath_target)
    float   breath_target = 0.f; // per-voice breath target (redrawn on rw_[3] tick)
    AhEnvelope eg;            // per-voice A-H-R envelope
    uint8_t  note = 0;        // MIDI note this voice plays (for toggle-off)
    bool     active = false;  // a note is latched on this voice
    uint32_t gen = 0;         // monotonically increasing: used for "oldest" steal
  };

  // ---- Shared sub bass (ONE sub pair for the whole cluster) -----------------
  // Since the polyphonic build, SUB1 (f/2) and SUB2 (f/4) are no longer
  // per-voice: the cluster's N notes feed ONE shared sub pair whose pitch is
  // the ENV-WEIGHTED ANCHOR of the sounding voices (see sub_anchor_w0_). This
  // makes the low end one stable "fundamental under the drone" instead of N
  // incoherent sub voices, and note on/off morphs the sub pitch smoothly
  // (one-pole relax) instead of clicking. The sub bass LEVEL breathes with the
  // loudest (breath-focus) voice at a depth set by the SUBB knob
  // (sub_breath_gain_, see the kSubBreath* constants in osc_constants.h).
  float      sub_ph   = 0.f;   // shared sub-octave (f/2) phasor (0..1)
  float      sub_ph2  = 0.f;   // shared sub-2 (f/4) phasor (0..1)
  Sub2Voice  sub2;             // shared f/4 saturated / LPF'd / gated sub voice
  float      sub_anchor_w0 = 0.f; // one-pole relaxed anchor rate (0..w0) — the pitch the sub phasors advance at
  float      sub_breath_gain = 1.f;  // one-pole relaxed SUBB breath gain (1.0 = static)
  float      fm_c_ph = 0.f;   // FM modulator C phasor (0..1): independent LFO-style oscillator that phase-FMs the (A+B) sum
  float      fm_s_ph = 0.f;   // FM modulator S phasor (0..1): carrier-locked "static teeth" oscillator (N teeth per A cycle) that phase-FMs the (A+B) sum
  Voice voice_[kVoiceCount];
  // Shared K-rate static-detune FACTORS (the DETU knob, applied per-voice in
  // the audio path as w0_a = v.w0 * det_f_a_). These are pure factors
  // (Detune::factor(±c/2)) and do NOT depend on the pitch, so they are
  // recomputed only when the DETU knob changes (apply_edit_param), never per
  // voice / per sample. This keeps the per-voice audio cost to two multiplies
  // when the random walk is off (regression-cheap).
  float det_f_a_ = 1.f; // GRIMM-style asymmetric: A stays at f (no shift)
  float det_f_b_ = 1.f; // GRIMM-style asymmetric: B = f * 2^(detune_cents/1200)
  // (GLIDE / portamento removed: no per-sample step coefficient, no gl_want_
  //  glide distance.) last_note_ tracks the cluster's most recent note for the
  //  next one (kept for noteOn bookkeeping / toggle-off semantics).
  int32_t  last_note_ = -1; // the cluster's last note (for the next noteOn)
  uint32_t gen_counter_ = 0U; // monotonic voice-start counter (steal ordering)
  // Sum of the per-voice A-H-R envelope levels (0..kVoiceCount). Recomputed
  // once per sample in process() and used for POLYPHONIC LOUDNESS
  // COMPENSATION: the cluster's summed PER-VOICE voice signals (sum_main /
  // sum_ring — the A+B sum and the ring-mod sum; the subs are ONE shared pair,
  // not per-voice, so they are NOT env-compensated) are scaled by env_comp_
  // (one-pole smoothed, below) and the common output envelope is the per-voice
  // A-H-R (not the sum). Net effect: 1 note = the original mono level (comp=1,
  // env=1), N simultaneous notes stay at roughly the same loudness instead of
  // adding coherently and clipping. env_sum_ is the raw per-sample sum (for the
  // one-pole and for the shared sub bass's env-weighted ANCHOR mean); the
  // SMOOTHED value lives in env_comp_, which is what the gain uses so the
  // 2 s attack/release doesn't pump the makeup gain.
  float env_sum_ = 0.f;
  // Smoothed 1/env_sum (one-pole, kClusterCompAlpha) — the actual gain applied
  // to the summed PER-VOICE voice signals (sum_main / sum_ring; osc_mix.h).
  // Starts at 1 (silent / single note = no attenuation) and follows the
  // cluster loudness so the 2 s A-H-R attack/release and note count don't pump
  // the makeup gain. (The shared sub bass does NOT use env_comp_ — it is ONE
  // pair, not a per-voice sum; it uses env_sum_ as the denominator of its
  // env-weighted ANCHOR mean. See the "Shared sub bass" block in osc_mix.h.)
  float env_comp_ = 1.f;
  // Cached value of rw_[3].getRedrawCount() from the previous process() call.
  // Used to detect the "tick" sample (when the Breath stream's A/B targets are
  // redrawn): on a tick, every active voice redraws its own breath_target.
  // This lets the Breath stream drive PER-VOICE levels (each note breathes
  // independently) instead of a single shared level for all voices.
  uint32_t last_breath_redraw_ = 0;
  // Index of the "focus" voice (the one currently highlighted by the rotating
  // focus breath). Advances to the next active voice on each rw_[3] tick.
  // The focus voice's level increases by rw_lvl; the others decrease by
  // rw_lvl / (n_active - 1), preserving the sum of all levels.
  uint8_t breath_focus_ = 0;

  // Recompute the shared static-detune factors from the DETU knob (K-rate).
  // Called from apply_edit_param(DETUNE). The per-voice w0_a / w0_b are then
  // v.w0 * det_f_a_ / det_f_b_ in the audio path (two multiplies, no Taylor
  // when the walk is off).
  void update_det_factors()
  {
    // GRIMM-style ASYMMETRIC detune: OSC1 (A) stays at f, OSC2 (B) is shifted
    // UP by detune_cents (f * 2^(det/1200)). This is unlike the old symmetric
    // detune (A = f * 2^(+det/2), B = f * 2^(-det/2)), which centered both
    // oscillators around f. The asymmetric approach matches GRIMM's VA engine
    // (analog.c: inc2 = cents_inc(p16, det, ...), where OSC2 is det cents above
    // OSC1). The beat frequency is |f_B - f_A| = f * (2^(det/1200) - 1), which
    // for small det is ~f * det * ln(2) / 1200 (e.g. ~0.8 Hz at 440 Hz for 10c).
    det_f_a_ = 1.0f; // A stays at f (no shift)
    det_f_b_ = Detune::factor(params_.detune_cents); // B = f * 2^(det/1200)
  }
  // Start (or re-target) one cluster voice on a note, arming its per-voice
  // Starts (or re-targets) one cluster voice on a note. The body lives in
  // osc_lifecycle.h (included at the bottom, after the class is complete).
  void voiceStart(Voice &v, uint8_t note, float w0);
  // MIDI note -> base phase increment (w = f/sr), self-contained (no firmware
  // LUT). The body lives in osc_lifecycle.h.
  void noteW0(uint8_t note, float *out_w);
  // (The A-H-R envelope lives in AhEnvelope, see osc_envelope.h.)
  // (The runtime DSP parameter struct Params is defined in osc_params.h as
  //  OscParamsData; see the `using Params = ::OscParamsData;` alias above.)

  void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case PARAM_SEL:
      // Knob A: parameter selector. 0..15 -> RATE_AMT, DETUNE, RATE_SPD,
      // RATE_SMTH, SUB1, SUB2, SATU, ASYM, FOLD, WAV1, WAV2, MAIN,
      // RW_WAV, RW_DRV, RW_FLD. Mask the low nibble (& 0xF) so any in-range
      // index (0..15) maps to itself. On selection change, re-apply the
      // cached raw value of the newly selected parameter so the DSP
      // immediately reflects it (PARAM_VAL then edits that value).
      select_ = (uint8_t)(value & 0x3FU); // 0..63, covers all kEditParamCount params
      apply_edit_param(kEditParamFirst + select_, edit_value_[select_]);
      break;

    case PARAM_VAL:
      // Knob B: raw 10-bit value (0..1023) of the currently selected
      // parameter. Write into the cache (so re-selecting later restores
      // it) and apply to DSP.
      edit_value_[select_] = value;
      apply_edit_param(kEditParamFirst + select_, value);
      break;

    default:
      break;
    }
  }

  // String representations for the type_strings parameters. Header slots 0/1
  // are the fixed A/B knobs; slots 2/3 are their optional (edit-menu)
  // duplicates (see header.c). All four carry type_strings, so this method
  // accepts any of the four slot indices and routes them to the same
  // underlying state (select_ / edit_value_):
  //   PARAM (slot 0 or 2): the name of the selected parameter
  //                        ("DET", "SUB1", .., "TWDR", "DWDR", "FWDR").
  //   VALUE (slot 1 or 3): a short representation of the selected parameter's
  //                        raw 10-bit value ("50%", "+25%", wavetable index).
  // The RW_WAV / RW_DRV / RW_FLD depths format as 0..100% (the default branch).
  // The manual formatter below avoids pulling printf machinery into the ARM

  // Format the display string for a header slot (0..3): PARAM_SEL shows
  // the name of the selected parameter, PARAM_VAL shows its formatted
  // value. The body lives in osc_param_display.h.
  const char *getParameterStrValue(uint8_t id, int32_t value) const override final;
  // Unit lifecycle: construction/reset and note handling (latched cluster).
  // The bodies live in osc_lifecycle.h (included at the bottom of this file,
  // after the class is complete).
  void init(float *) override final;
  void reset() override final;
  void noteOn(uint8_t note, uint8_t velo) override final;
  void noteOff(uint8_t note) override final;
  void allNoteOff() override final;
  // setPitch: legacy single-note entry point. In the polyphonic build the
  // cluster's pitch is driven entirely by noteOn(), so setPitch is a no-op.
  // It is kept (and still wired in unit.cc / wasm.cc) for API compatibility
  // and the hardware's per-block pitch callback.
  void setPitch(float w0);

  // lfo in (-1.0f, 1.0f) -- PLATFORM SHAPE-LFO DELIVERY (HARDWARE ONLY).
  // The platform used to synthesize a shape-LFO from the OSC-button RATE/SHAPE
  // knobs and deliver it here (L-rate) to drive the wavetable intra-bank morph
  // and the sub-octave FM. Both targets are now OFF (the wavetable morph is
  // driven by the INTERNAL wave-LFO below; the sub FM is fully disabled), so
  // this callback is a NO-OP stub: the platform's shape_lfo is ignored and the
  // hardware's RATE/SHAPE knobs no longer do anything audible. The signature
  // (and the unit.cc call site) is kept so the platform's render hook
  // unchanged; the lfo_ member is not written by this path.
  void setShapeLfo(float /*lfo*/)
  {
  }

  // Tempo callback (K-rate). The platform (unit.cc) and the web simulator
  // (Module.fx_set_bpm -> setTempo) deliver the current BPM here. The value
  // is stored in tempo_bpm_ so it can be read from the audio path later
  // (e.g. for BPM-synced random-walk periods, see the planned SYNC mode).
  // A 32-bit aligned float write is effectively atomic on Cortex-M7; the
  // worst case is the audio thread seeing either the old or the new BPM for
  // one block, which is inaudible (it only affects a slow modulation period,
  // never the audio rate).
  void setTempo(float bpm) override
  {
    tempo_bpm_ = bpm;
  }

#ifdef EMSCRIPTEN
  // (The old simulator-only setLfoFreq()/setLfoDepth() are GONE. The internal
  //  wave-LFO is now driven by the two editable parameters WAVE_LFO_RATE /
  //  WAVE_LFO_DEPTH through the standard apply_edit_param() path -- both the
  //  hardware PARAM/VALUE multiplexer and the web-sim's per-parameter sliders
  //  write it, so there is no separate UI-only channel anymore.)
#endif
  // Per-block K-rate prepass (web-sim edit/Rnd request consumption, rndTick,
  // rndMorphTick). Body lives in osc_mix.h (before the per-sample loop,
  // which it calls first).
  void blockPrepass(uint32_t frames);
  // Per-sample audio path (phasor advance, wavetable resolve, A+B chain,
  // sub/sub-2, final mix, envelope). The body lives in osc_mix.h.
  // (chord was removed — the chord/interval concept is gone.)
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final;
private:
  // cached derived coefficients, recomputed only when inputs change
  // (setPitch / setParameter(DETUNE)) to keep the audio path lightweight.
  // (The static symmetric-detune FACTORS det_f_a_ / det_f_b_ are declared in
  //  the public voice-cluster block above. The per-voice w0_a / w0_b and the
  //  A-H-R envelope are per-voice members of Voice (voice_[]) — see the
  //  cluster block. The sub phasors, the sub-2 voice state and the anchor
  //  rate are SHARED by the whole cluster (sub_ph / sub_ph2 / sub2 /
  //  sub_anchor_w0 / sub_breath_gain — see the "Shared sub bass" block).)

  // Four independent random-walk streams (dual channel each). See
  // osc_random_walk.h. Index 0 = Tone (detune + cross-FM), 1 = Timbre
  // (wavetable morph + SVF cutoff), 2 = Density (drive + wavefolder), 3 =
  // Breath (the four voice LEVELS: A / B / chord / sub wander in prominence).
  // Each stream wanders with independent random targets on its own
  // (randomly-drawn) period, so the streams are not phase-locked to each
  // other. (The Breath stream uses all four of its channels — A/B/C/D — to
  // give the four voices independent wandering; the other three streams use
  // only the A/B pair, which keep their shared-counter behavior.)
  static constexpr uint8_t kRwStreamCount = 4;
  RandomWalk rw_[kRwStreamCount];

  // Internal 12 dB/oct state-variable filter (post-saturator, pre-sub-mix).
  // See osc_svf.h. The cutoff (FCUT knob) and resonance (RES knob) are set at
  // K-rate; the cutoff is modulated per-sample by the random walk (RWFC is
  // folded into the FCUT knob's dry/wet — see apply_edit_param). The dry/wet
  // crossfade (svf_wet) blends the raw saturator output with the LPF output.
  Svf::State svf_;

  // LATCH is ALWAYS ON (see the voice-cluster block). The per-voice "active"
  // flag and the A-H-R envelope live in Voice (voice_[]). The cluster's last
  // note (last_note_) is declared in the voice-cluster block.

  // First-order relaxation coefficients for attack/release, precomputed in
  // init() (never in the audio path). Shared by every voice's A-H-R envelope.
  float eg_alpha_a_ = 0.f;
  float eg_alpha_r_ = 0.f;
  // (Chord interval fade / crossfade state was removed — the chord/interval
  //  concept is gone.)

  // ---- Enrichment-2 DSP state (formant / comb / ducking) --------------------
  // All OFF by default (the corresponding params_ fields are 0), so the stock
  // sound is bit-identical until the user turns one on. No dynamic allocation:
  // every array is a fixed member (the comb ring buffer is kCombDelayMaxSmp
  // floats = 1 KB of RAM, acceptable for a Cortex-M7 unit).

  // Comb / resonator: a one-pole IIR comb = ring-buffer delay line with its
  // output summed back through the feedback gain. kCombDelaySmp is the fixed
  // delay; kCombDelayMaxSmp is the (pow-of-2) buffer size so the read index is
  // a fast `& (size-1)` mask. comb_w_ is the write cursor.
  float    comb_buf_[kCombDelayMaxSmp];  // ring buffer (zeroed in init())
  uint32_t comb_w_  = 0;                 // write cursor

  // Envelope follower / sidechain ducking: a one-pole abs-peak follower of
  // the combined SUB1+SUB2 output energy, and the per-sample duck gain. The
  // duck is now CONSTANT (kDuckConstant, see osc_constants.h) -- it is always
  // active, the follower advances every sample, and duck_gain in osc_mix.h
  // settles to 1 - kDuckConstant * env_follower (floored at kDuckFloor). The
  // duck gain applies to the MAIN voice (A+B + RMOD), NOT the subs: the low
  // end is the constant anchor, the main tone yields.
  float duck_alpha_ = 0.f;     // one-pole follower coefficient (1 - exp(-1/tau))
  float env_follower_ = 0.f;   // smoothed |sub1 + sub2| energy

  // Formant: TWO parallel one-pole IIR comb resonators (F1 + F2), each a
  // ring-buffer delay line with a single feedback tap (ALWAYS STABLE, |fb| < 1).
  // The FORM knob morphs BOTH delays (the vowel's F1 and F2 formant frequencies,
  // from kFormantStops, linearly interpolated across A-E-I-O-U); the FVOL knob
  // sets the shared feedback (0..0.9). The two combs' "color" (feedback x
  // delayed sample) is summed and ADDED on top of the intact main signal
  // (parallel branch, NOT dry/wet). F2 is what makes the vowels DISTINCT:
  // I/E have high F2 (~1.8-2.3 kHz, "bright"), U/O have low F2 (~0.9 kHz,
  // "warm"), so morphing FORM audibly shifts the timbre. The per-sample path
  // costs ~20 ops only when FVOL > 0; at FVOL = 0 the whole stage is bypassed
  // (regression-safe). K-rate: recompute_formant_comb() sets the delays/fb.
  static constexpr uint32_t kFormantDelayMaxSmp = 256U; // ring-buffer size (pow-of-2)
  uint32_t formant_comb_w1_ = 0U;        // write index (F1 comb)
  uint32_t formant_comb_w2_ = 0U;        // write index (F2 comb)
  float    formant_comb_buf1_[kFormantDelayMaxSmp]; // F1 ring buffer
  float    formant_comb_buf2_[kFormantDelayMaxSmp]; // F2 ring buffer
  uint32_t formant_delay1_smp_ = 100U;   // K-rate delay (F1, samples)
  uint32_t formant_delay2_smp_ = 50U;    // K-rate delay (F2, samples)
  float    formant_fb_ = 0.f;            // K-rate feedback (0..0.9, shared)
  float    formant_w_[5];                // vowel morph weights (sum == 1)

  // (update_w0_det / update_w0_sub / update_w0_sub2 were removed in the
  //  polyphonic build. The static detune is now the SHARED factor pair
  //  det_f_a_ / det_f_b_ (see update_det_factors, called from
  //  apply_edit_param(DETUNE)); the per-voice w0_a / w0_b are computed per
  //  sample in the audio path (osc_mix.h) from each voice's own (possibly
  //  gliding) w0. The sub (f/2) and sub-2 (f/4) phase increments and the
  //  sub-2 voice state are now SHARED by the whole cluster (sub_ph / sub_ph2
  //  / sub2 / sub_anchor_w0) and are computed per sample in osc_mix.h from
  //  the env-weighted ANCHOR rate (see the "Shared sub bass" block there).)

  // ---- Enrichment-2: formant biquad coefficient setup (K-rate) --------------
  // Recompute the 10 formant biquad coefficients (5 vowels x 2 formants, each a
  // 2nd-order RBJ band-pass at the vowel's F1 / F2 with a fixed Q) and the
  // vowel morph weights from params_.formant_pos. The FORM knob (0..1) selects
  // a continuous position across the 5 stops (A-E-I-O-U); we linearly
  // crossfade the coefficients between the two nearest stops so the vowel
  // "slides" smoothly. Called at K-rate only (apply_edit_param for FORM / FVOL,
  // and once from init()); the per-sample path just runs the 10 biquads. No
  // libm: the only transcendental is fastsinf (a Taylor series, already used
  // by Svf::ccForHz) for sin(w0); cos(w0) is derived as 1 - sin^2(w0)/2
  // (2nd-order, far below audibility error at these low frequencies).
  // Recompute the formant comb delay and feedback from the FORM / FVOL knobs.
  // K-rate only (apply_edit_param for FORM / FVOL, and once from init()).
  // The FORM knob (0..1) selects a continuous position across the 5 vowel stops
  // (A-E-I-O-U); we linearly interpolate the F1 frequency (300..700 Hz) and
  // set the delay = sr / F1. The FVOL knob (0..1) sets the feedback (0..0.9),
  // so the comb peak grows with FVOL. At FVOL = 0 the comb is bypassed
  // (regression-safe).
  void recompute_formant_comb()
  {
    // Interpolate BOTH formant frequencies (F1 and F2) from the 5 vowel stops
    // (kFormantStops: A/E/I/O/U). The FORM knob (0..1) maps linearly across the
    // stops (0 = A, 0.25 = E, 0.5 = I, 0.75 = O, 1.0 = U); between two stops
    // we linearly crossfade the (F1, F2) pair, so the vowel "slides".
    const float pos = params_.formant_pos; // 0..1
    const float x = pos * 4.f;             // 0..4 (continuous stop coordinate)
    const int i0 = (int)x;                 // 0..3 (i0 == 4 only at x == 4)
    const float frac = x - (float)i0;      // 0..1 (0 at x == 4)
    float f1 = 0.f, f2 = 0.f;
    if (i0 >= 0 && i0 < 4)
    {
      f1 = kFormantStops[i0][0] * (1.f - frac) + kFormantStops[i0 + 1][0] * frac;
      f2 = kFormantStops[i0][1] * (1.f - frac) + kFormantStops[i0 + 1][1] * frac;
    }
    else
    {
      f1 = kFormantStops[4][0]; // x == 4 exactly -> U
      f2 = kFormantStops[4][1];
    }
    // Delay = sr / (2 * f): the x2 factor puts the comb's fundamental peak at
    // f/2 (sub-octave of the formant) so the resonance sits in a musical band
    // (F1: 135..365 Hz, F2: 435..1145 Hz) instead of above the drone's body.
    // Clamped to [8, 250] samples: 8 smp = the highest possible peak (~3 kHz,
    // safe, no aliasing of the resonance itself), 250 smp keeps it in-buffer.
    const float sr = 48000.f;
    uint32_t d1 = (uint32_t)(sr / (2.f * f1));
    uint32_t d2 = (uint32_t)(sr / (2.f * f2));
    if (d1 < 8U) d1 = 8U; else if (d1 > 250U) d1 = 250U;
    if (d2 < 8U) d2 = 8U; else if (d2 > 250U) d2 = 250U;
    formant_delay1_smp_ = d1;
    formant_delay2_smp_ = d2;
    // Feedback = FVOL (0..1) scaled to 0..0.9 (stable, < 1).
    formant_fb_ = params_.formant_wet * 0.9f;
    // Zero the vowel weights (no longer used, but kept for API compatibility).
    for (int v = 0; v < 5; ++v)
      formant_w_[v] = 0.f;
  }

  Params params_;
  // (The old `lfo_` member -- the platform shape-LFO value -- is GONE. The
  //  platform's OSC-button RATE/SHAPE knobs are now NO-OPs (setShapeLfo() is a
  //  stub), so no external LFO feeds the audio path. The wavetable intra-bank
  //  morph is driven by the INTERNAL wave-LFO below (lfo_sim_).)
  // Current tempo in BPM, delivered via setTempo() (K-rate). Default 120
  // (the web-simulator slider default). Read from the audio path for
  // BPM-synced modulation (planned SYNC mode); today it is stored but not
  // yet consumed, so the stock sound is unaffected.
  float tempo_bpm_ = 120.f;

  // INTERNAL wave-LFO synthesizer (see osc_lfo_sim.h). Drives the wavetable
  // intra-bank morph (osc_mix.h: lfo_morph = bank_center + lfo_sim_.process()
  // * lfo_morph_depth). The frequency (WAVE_LFO_RATE) and depth
  // (WAVE_LFO_DEPTH) are set by the standard apply_edit_param() path, so both
  // the hardware PARAM/VALUE multiplexer and the web-sim's per-parameter
  // sliders drive it identically. On ARM the platform's shape_lfo no longer
  // feeds the audio path (setShapeLfo() is a stub), so the internal LFO is
  // the single source of the wavetable morph on both hardware and sim.
  ShapeLfoSim lfo_sim_;
  // Second internal wave-LFO for the 2D morph (see osc_lfo_sim.h): drives the
  // intra-bank morph of stream 2 (WAV2 bank) INDEPENDENTLY of lfo_sim_ (its
  // own phase, so the two streams are never phase-locked). Its frequency /
  // depth are set by WAVE2_LFO_RATE / WAVE2_LFO_DEPTH through the standard
  // apply_edit_param() path (osc_param_apply.h), exactly like lfo_sim_. The
  // osc_mix.h audio path calls process() on both LFOs once per sample and
  // crossfades the two resolved waves by WAVE_MIX (params_.wave_mix).
  ShapeLfoSim lfo_sim2_;
  // Third internal wave-LFO for the WAVE-MIX balance (see osc_lfo_sim.h):
  // MODULATES the stream 1 <-> stream 2 crossfade weight around the static
  // WAVE_MIX value (params_.wave_mix is the CENTER; lfo_mix_.process() is the
  // SWING). Its frequency / depth / shape / gap are set by WMIX_LFO_RATE /
  // WMIX_LFO_DEPTH / WMIX_LFO_SHAPE / WMIX_LFO_GAP through the standard
  // apply_edit_param() path (osc_param_apply.h), exactly like lfo_sim_.
  // Default (factory): freq_hz = 0 (LFO OFF) -> process() returns 0 -> the
  // mix is fully static (regression-safe). The osc_mix.h audio path computes
  //   mix_eff = clamp(params_.wave_mix + lfo_mix_.process(), -1, +1)
  //   x = 0.5 * (1 + mix_eff).
  ShapeLfoSim lfo_mix_;

  // local variables related to audio processing
  // (The per-voice phasors, FM lookback caches, FM index LPF states, sub-2
  //  voice states and A-H-R envelopes are now per-voice members of Voice
  //  (voice_[]) — see the polyphonic voice-cluster block above. The
  //  wavetable morph is now INTRA-BANK: the LFO drives the position within
  //  the selected bank (see WaveTable::resolve_wave in osc_waves.h), so
  //  there is no per-voice wave_shift_ anymore.)
  // ---- Wavetable scanning (see osc_waves.h) ----------------------------------
  // The six-bank Korg wavetable scanner + the intra-bank LFO morph live in
  // WaveTable (osc_waves.h). The WAVE knob selects a bank (A..F, 0..5); the
  // LFO drives the position within that bank, centered on the bank (clamped to
  // [0, bank_size-1]). resolve_wave is called as
  // WaveTable::resolve_wave(wave_bank, lfo_morph, ph), where lfo_morph =
  // bank_center + lfo_ * lfo_morph_depth. The LFO->morph depth constant is
  // WaveTable::kLfoMorphDepthHalfSpan.
  WaveTable waves_;
};

// The Osc method bodies are defined here, AFTER the class is complete, so
// they can reference every data member and the global-scope tuning constants
// from osc_constants.h.
#include "osc_param_apply.h"   // Osc::apply_edit_param()
#include "osc_param_display.h" // Osc::getParameterStrValue()
#include "osc_rnd.h"           // Osc::rndArm/rndTick/rndMorph* (Rnd re-roll + morph engine)
#include "osc_lifecycle.h"     // Osc::init/reset/noteOn/noteOff/allNoteOff/setPitch
#include "osc_randomize.h"     // Osc::randomizeAll()  (Rnd re-roll, device + sim)
#include "osc_mix.h"           // Osc::process()