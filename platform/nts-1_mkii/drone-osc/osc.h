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
// helper headers (osc_chord.h etc.) because they call it from inline methods.
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
#include "osc_chord.h"        // ChordVoice: third oscillator at selectable interval
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
    WAV1,           // oscillator A wavetable (0..90 across banks A-F)
    WAV2,           // oscillator B wavetable (0..90 across banks A-F)
    WAV3,           // oscillator C (chord voice) wavetable (0..90 across banks A-F)
    CHORD,          // third (chord) oscillator level (0..100%, default 0% = off)
    CHRD_INT,       // chord interval (0..1900 cents, continuous)
    MAIN,           // main (A+B mixed) voice level (0..100%, default 100% = full)
    RW_WAV,         // random-walk depth on wavetable morph (0..100%, default 0% = off)
    RW_DRV,         // random-walk depth on saturator drive (0..100%, default 0% = off)
    RW_FLD,         // random-walk depth on wavefolder depth (0..100%, default 0% = off)
    FCUT,           // internal SVF cutoff + dry/wet (log 80 Hz..12 kHz, 0% = raw, 100% = LPF)
    RES,            // internal SVF resonance Q (0..100% -> 0.5..2.5, default 0% = 0.5)
    RW_FC,          // random-walk depth on SVF cutoff (0..100%, default 0% = off)
    RW_LVL,         // breath: random-walk depth on the four voice LEVELS (0..100%, default 0% = off)
    RW_LSP,         // breath stream period (0..100% -> 0.1 s..10 s, log, default 50%)
    RW_LSM,         // breath stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    // ---- Chord voice's OWN drive/fold (independent of the A+B SATU/FOLD) ----
    // The chord has its own wavefolder + saturator pass (see the CHORD branch in
    // process()). CHSAT is its drive (0..3.0, like SATU) and CHFLD is its
    // fold depth (0..1, like FOLD). ASYM stays SHARED with the A+B voice.
    // Appended at the END (after RW_LSM) so the existing edit indices
    // (DETUNE..RW_LSM = 0..29) are not renumbered.
    CHSAT,          // chord saturator drive (0..3.0, default = SATU default)
    CHFLD,          // chord wavefolder depth (0..1, default = FOLD default = off)
    // ---- Enrichment: second chord interval, jitter, ring-mod, phase-FM ----
    // Appended at the END (after CHFLD) so the existing edit indices
    // (DETUNE..CHFLD = 0..31) are not renumbered. See the per-param comments
    // in osc_param_apply.h / osc_mix.h for the DSP details.
    CINT2,          // second chord-voice interval (0..1900 cents, continuous; shares CHRD level + CHSAT/CHFLD)
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
    // ---- CINT2's OWN drive/fold (CHSAT2 / CHFLD2) --------------------------
    // Appended at the END (after RFM_B) so the existing edit indices
    // (DETUNE..RFM_B = 0..38) are not renumbered. The CINT2 interval gets its
    // OWN folder+saturator pass, independent of the CINT voice's CHSAT/CHFLD
    // and the A+B SATU/FOLD (see the CINT2 branch in osc_mix.h). This lets the
    // higher interval be softened/filtered separately. Defaults MATCH
    // CHSAT/CHFLD (drive = SATU default, fold = FOLD default) so the stock
    // sound is unchanged. ASYM is shared.
    CHSAT2,         // CINT2's own saturator drive (0..3.0, default = CHSAT default)
    CHFLD2,         // CINT2's own wavefolder depth (0..1, default = CHFLD default = off)
    NUM_PARAMS
  };

  // ---- Multiplexer constants -----------------------------------------------
  // Number of editable DSP parameters (RATE_AMT..COMB).
  static constexpr uint8_t kEditParamCount = 41; // RATE_AMT..CHFLD2 (FORM/FLVL/COMB moved to DRONE-MOD)
  // The first editable enum value. The UI (header.c PARAM slot, 0..41) expects
  // this order (select_ index -> parameter), 44 parameters total:
  //   0 RATE (RATE_AMT, shared depth ceiling)  1 DET (DETUNE)
  //   2 SPD1             3 SM1   (Tone stream period / smooth)
  //   4 SPD2             5 SM2   (Timbre stream period / smooth)
  //   6 SPD3             7 SM3   (Density stream period / smooth)
  //   8 SUB1             9 SUB2  10 SAT    11 ASYM
  //   12 FLD             13 WA1  14 WA2    15 WA3
  //   16 CHRD (CHORD)    17 INT  (CHRD_INT) 18 MAIN (main A+B voice level)
  //   19 TWDR            20 DWDR 21 FWDR
  //   22 CUT  (SVF cutoff + dry/wet)  23 RES (SVF resonance Q)
  //   24 WFC  (RW_FC: random-walk depth on SVF cutoff)
  //   25 BRT  (RW_LVL: breath depth on voice levels)
  //   26 BRR  (RW_LSP: breath stream period)  27 SMT (RW_LSM: breath smooth)
  //   28 CH2D (chord's own saturator drive)  29 CFLD (chord's own fold depth)
  //   30 INT2 (second chord interval, 0..1900 c)
  //   31 JIT (random phase jitter on A/B)
  //   32 RMOD (A*B ring-mod derived voice, MAIN-ducked)
  //   33 FM1 (FM_A: STATIC phase-FM on A by B)
  //   34 FM2 (FM_B: STATIC phase-FM on B by A)
  //   35 FMAB (RFM_A: random-walk depth on FM A, RATE-gated)
  //   36 FMBB (RFM_B: random-walk depth on FM B, RATE-gated)
  //   37 CH2D (CINT2's own saturator drive)  38 CH2F (CINT2's own fold depth)
  // PARAM_SEL / PARAM_VAL are the two multiplexer knobs (not editable params).
  // kEditParamCount = 41 = (CHFLD2 - RATE_AMT + 1); select_ 0..40 -> RATE_AMT..CHFLD2.
  // (The former FORM/FLVL/COMB enrichment-2 tail (indices 39..41) was moved to
  //  the DRONE-MOD unit; removing it as the enum TAIL leaves 0..38 unchanged.)
  // kEditParamFirst = RATE_AMT so RATE leads the multiplexer (select_ 0).
  static constexpr uint8_t kEditParamFirst = RATE_AMT;
  // ---- Runtime DSP parameter struct (see osc_params.h) ---------------------
  using Params = ::OscParamsData;

  // ---- Multiplexer state (select_ / edit_value_) --------------------
  // edit_value_[i] is the raw 10-bit cache of editable parameter i, so
  // re-selecting a parameter restores its last value. Defaults match the
  // DSP defaults in OscParamsData::reset() (osc_params.h).
  // WAV3 0, CHORD 205, CINT 377, MAIN 1023 (full), RW_WAV 0, RW_DRV 0,
  // RW_FLD 0, FCUT 921 (~90%), RES 0, RWFC 0, RW_LVL 0
  // (breath off), RW_LSP 511, RW_LSM 511. SUB2 (index 9) defaults to 410 (~40%).
  // (The former enrichment-2 FORM/FVOL/COMB tail, indices 39..41, moved to
  //  DRONE-MOD and is no longer part of the OSC edit-parameter set.)
  // Order = select_ order (kEditParamFirst = RATE_AMT): index 0 = RATE (0 =
  // off), index 1 = DET (511 = 0 c). RATE leads the multiplexer.
  int32_t edit_value_[kEditParamCount] = {0, 511, 511, 511, 511, 511, 511, 511, 511, 410, 341, 511, 0, 0, 128, 0, 205, 377, 1023, 0, 0, 0, 921, 0, 0, 0, 511, 511, 341, 0, 511, 0, 0, 0, 0, 0, 0, 341, 0, 511};

  // Last STABLE state (RndM = ORBI reference): a raw-value snapshot of the
  // sound that every ORBI re-roll develops from. It is set by init() (factory
  // defaults -- the default initializer mirrors edit_value_ exactly, so it
  // already matches at startup) and is REWRITTEN by every All/Part re-roll
  // (they stabilize on their result). ORBI re-rolls read it but never write
  // it, so an ORBI session stays anchored to the last stable sound. Manual
  // PARAM/VALUE edits do NOT affect it (a manual tweak is not a "stable
  // state" until an All/Part re-roll or reset confirms it). Keep the default
  // list in sync with edit_value_ above.
  int32_t stable_value_[kEditParamCount] = {0, 511, 511, 511, 511, 511, 511, 511, 511, 410, 341, 511, 0, 0, 128, 0, 205, 377, 1023, 0, 0, 0, 921, 0, 0, 0, 511, 511, 341, 0, 511, 0, 0, 0, 0, 0, 0, 341, 0, 511};

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

  // Enforce minimum separation between CINT (idx 17) / CINT2 (idx 30) in
  // `target` after a re-roll: if both are non-zero and closer than
  // kRndCintMinSepRaw (94 raw ≈ 175 cents), re-draw CINT2 until it's far
  // enough. A 0 (unison) on either side is allowed. Works in raw domain.
  // Audio-thread only. ~40 B.
  void ensure_cint_separation(int32_t* target);

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
  // resolve_wave(); fractional chord cents glide continuously).
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

  // (The LFO -> wavetable morph depth lives in WaveTable::kLfoMorphDepth,
  // see osc_waves.h.)

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
  // walk is off; when the walk is active, process() recomputes them per
  // sample for the combined (static + walk) per-voice detune. The chord
  // voice and the sub voices stay at the base note (they are stable
  // "anchors", not beating voices).
  float w0_a_det_ = 0.f; // w0 * 2^(+detune_cents/2 / 1200)
  float w0_b_det_ = 0.f; // w0 * 2^(-detune_cents/2 / 1200)
  // (The A-H-R envelope lives in AhEnvelope, see osc_envelope.h.)
  // (The runtime DSP parameter struct Params is defined in osc_params.h as
  //  OscParamsData; see the `using Params = ::OscParamsData;` alias above.)

  void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case PARAM_SEL:
      // Knob A: parameter selector. 0..15 -> RATE_AMT, DETUNE, RATE_SPD,
      // RATE_SMTH, SUB1, SUB2, SATU, ASYM, FOLD, WAV1, WAV2, CHORD, CHRD_INT,
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
  // Unit lifecycle: construction/reset, note handling (legato drone) and the
  // 2 s pitch glide. The bodies live in osc_lifecycle.h (included at the
  // bottom of this file, after the class is complete).
  void init(float *) override final;
  void reset() override final;
  void noteOn(uint8_t note, uint8_t velo) override final;
  void noteOff(uint8_t note) override final;
  void allNoteOff() override final;
  void setPitch(float w0);

  // Recompute every voice whose phase increment derives from the base w0_.
  // Called at K-rate (parameter changes) and per-sample during a glide so the
  // detuned A/B pair, sub-1, sub-2, chord and chord-2 all follow the gliding
  // pitch exactly.
  void recalc_pitch_derived()
  {
    update_w0_det(); // A/B symmetric detune
    // The OLD phasors track chord_old_cents_ (not params_.chord*_cents): during
    // an active pitch crossfade the old note keeps its pre-change frequency
    // even if the base note changes mid-fade, so the crossfade is preserved.
    // The NEW phasors track params_.chord*_cents (the target). When no fade is
    // active chord_old_cents_ == params_.chord*_cents, so both are identical
    // (regression-safe / zero difference).
    chord_.update(w0_, chord_old_cents_);
    chord_new_.update(w0_, params_.chord_cents);
    chord2_.update(w0_, chord2_old_cents_);
    chord2_new_.update(w0_, params_.chord2_cents);
    update_w0_sub();
    update_w0_sub2();
    waves_.update(w0_);
  }

  // lfo in (-1.0f, 1.0f)
  void setShapeLfo(float lfo)
  {
    lfo_ = lfo;
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
  // Simulator-only LFO settings (set from the JS UI, K-rate). On hardware
  // these are the physical OSC-button knobs and are delivered via the
  // platform's shape_lfo; here they drive the internal sine generator
  // (ShapeLfoSim, see osc_lfo_sim.h).
  void setLfoFreq(float freq)
  {
    lfo_sim_.freq = (freq < 0.f) ? 0.f : (freq > 30.f) ? 30.f : freq;
  }

  void setLfoDepth(float depth)
  {
    lfo_sim_.depth = (depth < 0.f) ? 0.f : (depth > 1.f) ? 1.f : depth;
  }
#endif
  // Per-block K-rate prepass (web-sim edit/Rnd request consumption, rndTick,
  // rndMorphTick, pitch-glide advance). Body lives in osc_mix.h (before the
  // per-sample loop, which it calls first).
  void blockPrepass(uint32_t frames);
  // Per-sample audio path (phasor advance, wavetable resolve, A+B chain,
  // chord, sub/sub-2, final mix, envelope). The body lives in osc_mix.h.
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final;
private:
  // cached derived coefficients, recomputed only when inputs change
  // (setPitch / setParameter(DETUNE)) to keep the audio path lightweight.
  // (The static symmetric-detune factors w0_a_det_ / w0_b_det_ are declared
  //  earlier in the class, next to their documentation block. When the
  //  random walk is active, process() recomputes the per-voice factors per
  //  sample for the combined (static + walk) detune.)
  float w0_sub_ = 0.f;
  // second-sub (f/4) phase increment, precomputed in update_w0_sub2()
  float w0_sub2_ = 0.f;
  // Second sub voice (f/4, saturated, LPF'd, gated). See osc_sub2.h.
  Sub2Voice sub2_;
  // Chord voice (third oscillator at selectable interval). See osc_chord.h.
  ChordVoice chord_;
  // Second chord voice (fourth oscillator at a second selectable interval).
  // Shares the chord's level (CHRD), wavetable (WAV3), folder/saturator
  // (CHSAT/CHFLD), and the Breath stream's lvl_c (so both intervals breathe
  // in sync — the chord is one object, not two independent voices). See the
  // CINT2 branch in the audio path (osc_mix.h) and osc_param_apply.h.
  ChordVoice chord2_;

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

  // Note "active" flag: true while a note is held (noteOn not yet followed by
  // noteOff / allNoteOff / reset). Today this is set by the MIDI handlers;
  // later it may be driven algorithmically (self-starting notes) without
  // touching the audio path. The A-H-R envelope below is the single voice
  // that shapes the attack/release of the active note.
  bool note_on_ = false;

  // Legato is ALWAYS ON: key release does NOT stop the sound (it keeps sounding
  // indefinitely); pressing the SAME key again fades it out (toggle-off);
  // pressing a DIFFERENT key retargets the note (the pitch glide below slides
  // from the old note to the new one) and it keeps playing.
  // Last key pressed (for the toggle-off test in noteOn). 0 = none.
  uint8_t last_note_ = 0;

  // ---- Pitch glide (portamento) ----------------------------------------------
  // When the base note changes, w0_ is interpolated LINEARLY from glide_from_
  // to glide_to_ over kGlideSamples (2 s) instead of jumping. The audio path
  // (osc_mix.h) advances this every sample while glide_active_ is true and
  // recomputes the derived voices (see recalc_pitch_derived).
  bool glide_active_ = false;
  uint32_t glide_elapsed_ = 0; // samples elapsed in the current glide
  float glide_from_ = 0.f;     // w0 at the start of the glide
  float glide_to_ = 0.f;       // target w0

  // A-H-R envelope (single voice). See osc_envelope.h for the state machine.
  AhEnvelope eg_;

  // First-order relaxation coefficients for attack/release, precomputed in
  // init() (never in the audio path).
  float eg_alpha_a_ = 0.f;
  float eg_alpha_r_ = 0.f;

  // ---- Chord interval fade (INT / INT2 on-off, see kChordFadeSamples) --------
  // Two independent one-pole level fades, one per chord interval. The audio
  // path (osc_mix.h) relaxes each toward (cents > 0 ? 1 : 0) per sample and
  // multiplies the interval's level by it, so switching INT / INT2 on or off
  // fades the voice in / out over 2 s instead of clicking (exactly the
  // one-pole curve of the note attack/release envelope, same alpha).
  float chord1_fade_ = 0.f; // CINT  interval level fade (0..1)
  float chord2_fade_ = 0.f; // CINT2 interval level fade (0..1)
  float chord_fade_alpha_ = 0.f; // per-sample one-pole alpha (K-rate, init())

  // ---- Chord interval pitch crossfade (CINT / CINT2 offset change) -----------
  // When the interval cents value changes (Rnd re-roll, manual edit, morph),
  // the phasor's frequency ratio (w0) jumps and produces a click. This
  // crossfade runs TWO wavetable lookups per chord voice (old-offset phasor
  // + new-offset phasor) and blends their outputs with a one-pole coefficient
  // (chord1_cross_ / chord2_cross_, same 2 s alpha as the on/off level fade).
  // The "new" phasors (chord_new_ / chord2_new_) are reset to the FRESH
  // offset at change-detection time (blockPrepass); the "old" phasors
  // (chord_ / chord2_) keep running at their current w0. When the crossfade
  // completes (cross >= 0.999), the old phasor is synced to the new one and
  // only the new phasor is read in the audio path (zero extra lookups,
  // regression-safe). See the crossfade block in osc_mix.h.
  ChordVoice chord_new_;    // "new" CINT  phasor (target offset)
  ChordVoice chord2_new_;   // "new" CINT2 phasor (target offset)
  float chord1_cross_ = 1.f; // CINT  pitch crossfade position (0=old, 1=new)
  float chord2_cross_ = 1.f; // CINT2 pitch crossfade position (0=old, 1=new)
  // Interval cents the OLD phasors (chord_ / chord2_) are currently tracking.
  // These are the cents used to re-fret the old phasor in recalc_pitch_derived()
  // / noteOn() / glide, so a base-pitch change MID-FADE keeps the old note at
  // its old frequency (the crossfade is not destroyed by the note change).
  // When no crossfade is in flight (cross == 1) they equal params_.chord*_cents;
  // when a crossfade is active they are the PRE-CHANGE cents and are synced back
  // to params_.chord*_cents when the crossfade completes (osc_mix.h).
  float chord_old_cents_ = 0.f;   // CINT  cents the old phasor tracks
  float chord2_old_cents_ = 0.f;  // CINT2 cents the old phasor tracks
  float prev_chord_cents_ = 0.f;    // last-seen CINT  cents (change detection)
  float prev_chord2_cents_ = 0.f;   // last-seen CINT2 cents (change detection)

  // ---- Enrichment-2 DSP state (sidechain ducking) ----------------------------
  // (The former FORM/FLVL/COMB formant + comb-resonator DSP state was moved to
  //  the DRONE-MOD unit; only the constant sidechain duck remains here.)

  // Envelope follower / sidechain ducking: a one-pole abs-peak follower of
  // the combined SUB1+SUB2 output energy, and the per-sample duck gain. The
  // duck is now CONSTANT (kDuckConstant, see osc_constants.h) -- it is always
  // active, the follower advances every sample, and duck_gain in osc_mix.h
  // settles to 1 - kDuckConstant * env_follower (floored at kDuckFloor). The
  // duck gain applies to the MAIN voice (A+B + RMOD), NOT the subs: the low
  // end is the constant anchor, the main tone yields.
  float duck_alpha_ = 0.f;     // one-pole follower coefficient (1 - exp(-1/tau))
  float env_follower_ = 0.f;   // smoothed |sub1 + sub2| energy

  // recompute the static symmetric-detune factors (w0_a_det_ / w0_b_det_)
  // — only when the DETU knob or the base pitch changes. The DETU knob
  // detunes A and B in OPPOSITE directions: A gets +c/2 cents, B gets -c/2
  // cents (c = params_.detune_cents, ±10 cents). The Taylor expansion
  // (see Detune::factor, osc_detune.h) at |x| <= 5 * ln(2)/1200 ≈ 0.0042
  // (per voice, since each is detuned by c/2) keeps the relative error
  // far below audibility.
  //
  // Note: when the random walk is active (RATE AMT > 0), process() recomputes
  // the per-voice factors per sample for the combined (static + walk) detune,
  // so the cached w0_a_det_ / w0_b_det_ are only used while the walk is
  // disabled (0 Taylor cost in the audio path in that case — just a read).
  void update_w0_det()
  {
    const float half = 0.5f * params_.detune_cents;
    w0_a_det_ = w0_ * Detune::factor(+half);
    w0_b_det_ = w0_ * Detune::factor(-half);
  }

  // recompute w0_sub_ = w0_ * 2^((kSubDetuneCents)/1200) — the sub-octave
  // phasor's phase increment, cached at setPitch() (called every block from
  // unit_render). The sub runs half the base rate (f/2 = one octave down)
  // plus a fixed kSubDetuneCents detune. Because the detune is a compile-
  // time constant, the factor 2^(kSubDetuneCents/1200) is folded into a
  // single literal: 0.5 * 2^(-4/1200) ≈ 0.49885 (precomputed, so the audio
  // path is one multiply, no Taylor series needed per sample).
  void update_w0_sub()
  {
    w0_sub_ = w0_ * kSubW0Scale; // 0.5 * 2^(kSubDetuneCents/1200) == 0.5 * 2^(-4/1200)
  }

  // recompute the second-sub (f/4) phase increment and its fixed filter/gate
  // coefficients. Unlike sub-1 there is NO fixed detune: the voice is a
  // stable "anchor" in the low end, so w0_sub2_ = w0_ * 0.25 exactly (two
  // octaves below the base). Also precomputed here (called once per pitch
  // change, never in the audio path):
  //   sub2_.lpf_a  — one-pole LPF coefficient for Sub2Voice::kLpfCutoffHz (cuts the
  //                  saturator's harmonics so the voice stays in the low end)
  //   sub2_.gate_k — first-order smoothing coefficient for the pitch gate
  //                  (open/close transition, Sub2Voice::kGateTauSec time constant)
  //   sub2_.gate_hz_— base (saw) frequency in Hz at which f/4 == Sub2Voice::kGateHz;
  //                  below this the gate closes. Cached as 4 * Sub2Voice::kGateHz.
  void update_w0_sub2()
  {
    w0_sub2_ = w0_ * 0.25f; // f/4, no detune
    sub2_.update(w0_); // recompute LPF/gate coefficients (see osc_sub2.h)
  }

  Params params_;
  float w0_;
  float lfo_;
  // Current tempo in BPM, delivered via setTempo() (K-rate). Default 120
  // (the web-simulator slider default). Read from the audio path for
  // BPM-synced modulation (planned SYNC mode); today it is stored but not
  // yet consumed, so the stock sound is unaffected.
  float tempo_bpm_ = 120.f;

#ifdef EMSCRIPTEN
  // Host-side (web simulator) shape-LFO synthesizer. On hardware the platform
  // generates the LFO from the OSC-button A/B knobs and delivers it through
  // setShapeLfo(); in the simulator there is no platform, so we synthesize a
  // phase-coherent sine at audio rate here (see osc_lfo_sim.h). The UI sets
  // frequency (F knob, 0..30 Hz) and depth (S knob, 0..1) via
  // setLfoFreq()/setLfoDepth().
  ShapeLfoSim lfo_sim_;
#endif

  // local variables related to audio processing
  float phasor_a_;
  float phasor_b_;
  float phasor_sub_; // sub-octave sine phasor (advances at w0/2)
  // second sub voice (f/4, two octaves below the saws): a clean sine run
  // through a very soft saturator (adds "body"/thickening) then a one-pole
  // LPF (cuts the saturator's harmonics so it stays in the low end) and a
  // pitch gate (silences it when f/4 drops below Sub2Voice::kGateHz = infrasonic).
  // Unlike sub-1 it carries no fixed detune: it is a stable "anchor" in the
  // low end, not a beating voice.
  float phasor_sub2_; // second sub phasor (advances at w0/4, no detune)
  // Previous-sample wavetable outputs, cached for the next sample's FM
  // modulation (one-sample lookback, see the FM branch in the phasor update).
  // Zeroed on init / reset so the first sample's FM is a no-op.
  float prev_wav_a_ = 0.f;
  float prev_wav_b_ = 0.f;
  // One-pole low-pass state for the PHASE-FM modulation INDEX (see the FM
  // branch in the phasor update, osc_mix.h, and osc_constants.h kFmSmoothAlpha).
  // Smooths the fast sign-flips of the modulating wavetable so the upper
  // (harsh) FM sidebands are tamed while the slow "breathing" is kept. Zeroed
  // on init / reset so the first sample's FM is a no-op (the index is 0 when
  // FM=0 / walk off).
  float fm_smooth_a_ = 0.f;
  float fm_smooth_b_ = 0.f;
  // ---- Wavetable scanning (see osc_waves.h) ----------------------------------
  // The six-bank Korg wavetable scanner + the pitch-shifted morph state live in
  // WaveTable (osc_waves.h). The Osc owns a WaveTable member and reads its
  // cached morph position (waves_.wave_shift_) in the audio path; resolve_wave
  // is called as WaveTable::resolve_wave(...). The LFO->morph depth constant is
  // WaveTable::kLfoMorphDepth.
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