#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_randomize.h
 *
 *  Out-of-line definitions of Osc::randomizeAll(), Osc::randomizePartial() and
 *  Osc::randomizeOrbi() -- the re-roll routines. randomizeAll() re-rolls EVERY
 *  edit parameter to a fresh audition-friendly random preset;
 *  randomizePartial() re-rolls a random SUBSET (2..6) to fresh safe-window
 *  values (a "development", not a reset), each with a guaranteed significant
 *  step (>= kRndMinStepRaw from current); randomizeOrbi() is the same partial
 *  re-roll but anchored to the last STABLE state (stable_value_): the subset
 *  is drawn from stable_value_, the step is measured from the stable value,
 *  and the result is applied to the live edit_value_ without updating the
 *  stable state (see the function comments below).
 *
 *  All/Part re-rolls end by STABILIZING: their result becomes the new
 *  stable_value_ snapshot (the ORBI reference). ORBI never stabilizes.
 *
 *  Both are the ONE source of truth for randomization, shared by:
 *    * the hardware "Rnd" trigger slot (unit.cc: unit_set_param_value(id=5)
 *      -> Osc::rndArm(true), debounced 0.5 s), and
 *    * the web simulator's Fire button (wasm.cc rndRequest() ->
 *      Osc::rndArm(false), NOT debounced -- every click fires).
 *  The TYPE (All / Part) is set by the "RndM" slot (unit.cc id=4 ->
 *  Osc::setRndMode); rndTick() dispatches to randomizeAll or randomizePartial
 *  based on rnd_mode_. The hardware slot is debounced (first change immediate,
 *  further changes within 0.5 s only reset the timer, one final re-roll after
 *  0.5 s of silence); the web sim Fire button is not (every click re-rolls
 *  immediately). Both therefore produce bit-identical random windows (and the
 *  same osc_rand() Park-Miller-Carta source, see rnd_int below), so the
 *  simulator's and the device's re-rolls use the same distribution.
 *
 *  The safe windows are the exact port of the web simulator's randomRaw()
 *  (websim/osc.html): "safe" (walk / timbre) parameters get the full 0..1023
 *  range; "critical" parameters are constrained to audition-friendly windows
 *  so the result always plays cleanly (no clipping / mud / dissonance /
 *  resonance spikes). The CINT / CINT2 intervals are drawn from the same
 *  discrete consonant set the sim uses, so the chord is always "in key".
 *
 *  It is #included at the BOTTOM of osc.h, after the Osc class is complete, so
 *  it can reference edit_value_ / select_ / apply_edit_param() / osc_rand().
 */

#include "osc.h"  // Osc class (complete definition) + osc_consts::* + helpers

// ---- Size-conscious randomizer ----------------------------------------------
// Two deliberate choices keep the code small (the unit is -Os, and every byte
// matters in a Cortex-M7 flash image):
//   1. A SINGLE small integer helper (rnd_int) instead of six lambdas. Lambdas
//      capture and are emitted as separate .isra functions + a `bl` call site
//      per use (~350 B of helper code + ~150 B of calls); inlining one flat
//      helper avoids all of that.
//   2. A `constexpr` CINT lookup table instead of a file-scope `static const`
//      array. The latter is a static data object that pulls a static
//      initializer into _GLOBAL__sub_I (extra .init code + .rodata); a
//      constexpr is folded into the two use sites with zero storage/initializer.
//
// rnd_int(count) -> 0..count-1, uniform, from one osc_rand() sample.
//   osc_rand() is the platform's Park-Miller-Carta LCG exposed as a raw
//   uint32 in [0, 0x7FFFFFFF] (see osc_api.h "Random integer, Generated with
//   Park-Miller-Carta"). It is uniform over 2^31 values, so (s mod count) is
//   uniform over 0..count-1 for every count used here (<= 1024), with at most
//   a 1/2^17 bias -- far below audible. Pure integer math: no float round-trip,
//   no libm.
//
//   IMPORTANT: this must use osc_rand(), NOT osc_white(). osc_white() returns
//   a FLOAT in [-1, 1] (a Gaussian on the device runtime, a uniform LCG in the
//   wasm.cc sim stub) and (int32_t)truncation of it is 0 for ~99% of samples
//   (|x| < 1), which made rnd_int() return 0 every time and Rnd re-roll the
//   same fixed preset on hardware. osc_rand() carries the full 31-bit state,
//   so the distribution is correct on both the device and the simulator
//   (the sim stub mirrors the same PMC sequence, see wasm.cc).
inline int32_t Osc::rnd_int(uint32_t count) const
{
  const uint32_t s = osc_rand(); // [0, 0x7FFFFFFF]
  return (int32_t)(s % count);   // [0, count-1]
}

// Discrete consonant chord intervals (cents) CINT / CINT2 are randomized over.
// Mirrors CINT_SET in websim/osc.html exactly (same values, same order).
// constexpr so it inlines into cint_raw() with no rodata / no static init.
static constexpr int32_t kRndCintSet[9] = {0, 400, 500, 700, 900, 1000, 1200, 1500, 1900};

// raw for a WAV table index (0..kWavesTotal-1). Mirrors the sim's
// Math.round(index * 1023 / 90). (kWavesTotal-1 == 90, a power-free divisor.)
static inline int32_t drone_wav_raw(int32_t index)
{
  return (int32_t)((float)index * (float)kRawMax / (float)(WaveTable::kWavesTotal - 1) + 0.5f);
}
// raw for a cent value (0..ChordVoice::kMaxCents == 1900). Mirrors the sim's
// Math.round(cents * 1023 / 1900).
static inline int32_t drone_cents_raw(int32_t cents)
{
  return (int32_t)((float)cents * (float)kRawMax / ChordVoice::kMaxCents + 0.5f);
}

// ---- Shared "safe windows" (audition-friendly random ranges) -----------------
// The single source of truth for the raw 0..1023 range each edit parameter is
// randomized over. randomizeAll() draws every parameter; randomizePartial()
// draws a random SUBSET of them. A parameter NOT listed in rnd_window_for()
// uses the full 0..1023 range (it is "safe" at any value -- the walk / timbre /
// period knobs, the per-stream depths, the wavetables, the breath stream, and
// the formant vowel all play cleanly across their full span).
//
// C++11 note: the window table is NOT a constexpr array (the ARM build is
// -std=c++11, and a constexpr initializer may not reference the Osc class
// members DETUNE..DCK / kEditParamFirst). It is a plain static array, built
// once at load time from rnd_window_for(), whose return type is deduced from
// its (C++11-legal) default-argument initializers. (The WASM build is C++17 and
// treats these as constant-initialized; either way they are initialized before
// any re-roll, which only ever happens after init().)
static constexpr int32_t kRndFullRange = 1023; // full 0..1023 window ceiling
static constexpr int32_t kRndWavMax     = (int32_t)(WaveTable::kWavesTotal - 1); // 90
static constexpr int32_t kRndWavDiv     = (int32_t)(WaveTable::kWavesTotal - 1); // 90
static constexpr int32_t kRndCentMax    = (int32_t)ChordVoice::kMaxCents; // 1900
static constexpr int32_t kRndCentDiv    = (int32_t)ChordVoice::kMaxCents; // 1900

// The safe window for one edit parameter. Named struct (C++11-safe: an
// anonymous struct in a function signature is ill-formed in C++11).
struct RndWindow
{
  uint8_t kind; // 0 = uniform raw [lo,hi]; 1 = wavetable 0..kWavesTotal-1; 2 = consonant interval
  int32_t lo;
  int32_t hi;
  int32_t div;  // divisor for kind 1 (kWavesTotal-1) / kind 2 (kMaxCents); 0 for kind 0
};

// Return the audition-friendly safe window for one edit parameter (idx = the
// select_ index = enum - kEditParamFirst). Parameters not listed here get the
// full 0..1023 range (safe at any value). The case labels are LITERAL select_
// indices (computed from the current enum order, where RATE is 0 and DETUNE is
// 1), so this free function does not reference any Osc member (keeping it
// free of class-scope issues). See the osc.h enum for the index -> name map.
static RndWindow rnd_window_for(uint8_t idx)
{
  RndWindow w;
  w.kind = 0; w.lo = 0; w.hi = kRndFullRange; w.div = 0; // default: full range
  switch (idx)
  {
    case 1:  w.kind=0; w.lo=256;  w.hi=767;  w.div=0; break; // DET   +/-5 c
    case 0:  w.kind=0; w.lo=204;  w.hi=511;  w.div=0; break; // RATE  20..50% (cap wander)
    case 8:  w.kind=0; w.lo=153;  w.hi=818;  w.div=0; break; // SUB1  15..80%
    case 9:  w.kind=0; w.lo=0;    w.hi=460;  w.div=0; break; // SUB2  0..45%
    case 10: w.kind=0; w.lo=136;  w.hi=819;  w.div=0; break; // SAT   40..240
    case 11: w.kind=0; w.lo=256;  w.hi=767;  w.div=0; break; // ASYM  +/-50%
    case 12: w.kind=0; w.lo=0;    w.hi=613;  w.div=0; break; // FLD   0..60%
    case 16: w.kind=0; w.lo=0;    w.hi=613;  w.div=0; break; // CHRD  0..60%
    case 17: w.kind=2; w.lo=0;    w.hi=0;    w.div=kRndCentDiv; break; // INT  consonant interval
    case 18: w.kind=0; w.lo=563;  w.hi=1023; w.div=0; break; // MAIN  55..100%
    case 22: w.kind=0; w.lo=740;  w.hi=1023; w.div=0; break; // CUT   1 kHz..11 kHz
    case 23: w.kind=0; w.lo=0;    w.hi=669;  w.div=0; break; // RES   0.5..1.8 Q
    case 25: w.kind=0; w.lo=204;  w.hi=818;  w.div=0; break; // BRT   20..80% breath
    case 28: w.kind=0; w.lo=0;    w.hi=613;  w.div=0; break; // CH2D  0..60% chord drive
    case 29: w.kind=0; w.lo=0;    w.hi=511;  w.div=0; break; // CFLD  0..50% chord fold
    case 30: w.kind=2; w.lo=0;    w.hi=0;    w.div=kRndCentDiv; break; // INT2 consonant interval
    case 31: w.kind=0; w.lo=0;    w.hi=153;  w.div=0; break; // JIT   0..15% smear
    case 32: w.kind=0; w.lo=0;    w.hi=256;  w.div=0; break; // RMOD  0..25% ring
    case 33: w.kind=0; w.lo=0;    w.hi=307;  w.div=0; break; // FM1   0..30% color
    case 34: w.kind=0; w.lo=0;    w.hi=307;  w.div=0; break; // FM2   0..30% color
    case 35: w.kind=0; w.lo=0;    w.hi=511;  w.div=0; break; // FMAB  0..50% FM breath
    case 36: w.kind=0; w.lo=0;    w.hi=511;  w.div=0; break; // FMBB  0..50% FM breath
    case 37: w.kind=0; w.lo=0;    w.hi=613;  w.div=0; break; // CH2D2 0..60% CINT2 drive
    case 38: w.kind=0; w.lo=0;    w.hi=511;  w.div=0; break; // CH2F  0..50% CINT2 fold
    case 40: w.kind=0; w.lo=0;    w.hi=512;  w.div=0; break; // FLVL  0..50% formant
    case 41: w.kind=0; w.lo=0;    w.hi=307;  w.div=0; break; // COMB  0..30% resonance
    case 42: w.kind=0; w.lo=0;    w.hi=307;  w.div=0; break; // FLD2  0..30% worn edge
    case 43: w.kind=0; w.lo=0;    w.hi=256;  w.div=0; break; // DCK   0..25% duck
    default: break; // full range (SPDx/SMTx, walk depths, WAV1/2/3, etc.)
  }
  return w;
}

// Minimum SIGNIFICANT step (raw units) for a Partial re-roll: ~10% of the
// 10-bit range. A Partial change that moves a parameter by less than this is
// inaudible ("FOLD 53 -> 56"), so the draw is re-rolled until the new value is
// at least kRndMinStepRaw away from the current one (up to kRndRedrawMax tries,
// after which the last draw is accepted -- better a small step than none).
static constexpr uint32_t kRndMinStepRaw = 102U; // ~10% of 1024
static constexpr uint32_t kRndRedrawMax  = 5U;

// Draw a fresh raw value for one edit parameter from its safe window (see
// rnd_window_for()). Used by both randomizeAll() and randomizePartial().
inline int32_t Osc::rnd_draw(uint8_t idx)
{
  const RndWindow w = rnd_window_for(idx);
  if (w.kind == 0)
    return w.lo + rnd_int((uint32_t)(w.hi - w.lo + 1));
  if (w.kind == 1)
    return drone_wav_raw(rnd_int((uint32_t)(kRndWavMax + 1)));
  if (w.kind == 2)
    return drone_cents_raw(kRndCintSet[rnd_int((uint32_t)(sizeof(kRndCintSet) / sizeof(kRndCintSet[0])))]);
  return 0;
}

// ---- The re-roll itself ------------------------------------------------------
// All-mode re-roll: draw a fresh safe value for EVERY edit parameter, then apply
// all of them to the live DSP (see the tail loop). This is the original Rnd
// behaviour (a full reset to a fresh audition-friendly preset).
inline void Osc::randomizeAll()
{
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    edit_value_[i] = rnd_draw(i);

  // ---- Apply every edit parameter to the live DSP ---------------------------
  // Re-run the same apply path init() uses, so params_ (and the derived phasor
  // caches / SVF coefficients) are fully consistent with the new raw values.
  // apply_edit_param() is idempotent per-parameter.
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    apply_edit_param(kEditParamFirst + i, edit_value_[i]);

  // STABILIZE: an All re-roll is a fresh stable state -- the ORBI reference
  // (stable_value_) now points at this result.
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    stable_value_[i] = edit_value_[i];
}

// ---- Partial re-roll (RndM = Part) ------------------------------------------
// A "development" rather than a reset: pick a RANDOM SUBSET of 2..6 edit
// parameters and re-roll ONLY those to fresh safe-window values, leaving the
// rest exactly as they are (the current sound is the reference). Each changed
// parameter is guaranteed a SIGNIFICANT step (>= kRndMinStepRaw from its
// current raw, up to kRndRedrawMax re-draws) so the change is actually audible
// -- no "FOLD 53 -> 56" no-ops. The same LCG (rnd_int) and safe windows as
// randomizeAll() are used, so All/Part share the same distribution.
inline void Osc::randomizePartial()
{
  const uint32_t count = 2U + (uint32_t)rnd_int(5U); // 2..6 parameters
  for (uint32_t c = 0; c < count; ++c)
  {
    const uint8_t idx = (uint8_t)rnd_int(kEditParamCount);
    const int32_t current = edit_value_[idx];
    // First draw, then re-draw until the step is significant (or give up after
    // kRndRedrawMax tries and accept the last draw -- a small step is better
    // than no step at all).
    int32_t v = rnd_draw(idx);
    for (uint32_t t = 0; t < kRndRedrawMax; ++t)
    {
      int32_t d = v - current;
      if (d < 0)
        d = -d;
      if (d >= (int32_t)kRndMinStepRaw)
        break;
      v = rnd_draw(idx);
    }
    edit_value_[idx] = v;
    // Apply just this parameter immediately (idempotent). Applying per-parameter
    // (rather than the full tail loop) keeps the other 40+ untouched params
    // out of the apply path and matches the "only these changed" semantics.
    apply_edit_param(kEditParamFirst + idx, v);
  }

  // STABILIZE: a Part re-roll is a fresh stable state too -- the ORBI
  // reference (stable_value_) now points at this result.
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    stable_value_[i] = edit_value_[i];
}

// ---- Orbi re-roll (RndM = Orbi) --------------------------------------------
// A partial re-roll (same 2..6-subset count, same safe windows, same
// significant-step rule and the same LCG draw order as randomizePartial())
// anchored to the LAST STABLE STATE instead of the current sound:
//   * the base for every step is stable_value_[idx] (the last stable state --
//     factory defaults at startup, or the result of the last All/Part re-roll);
//   * the result is written to the LIVE edit_value_ and applied to the DSP,
//     but stable_value_ is NOT touched -- an ORBI re-roll does not create a
//     new stable state, so every ORBI fire develops from the same stable
//     sound no matter how many times it is fired (or how much the live sound
//     has drifted in the meantime).
// Manual PARAM/VALUE edits of the live parameters also leave stable_value_
// untouched by design.
inline void Osc::randomizeOrbi()
{
  // Work on a temp copy seeded from the stable state; edit_value_ is committed
  // only once, at the end. (Keeps the LCG draw order identical to
  // randomizePartial(): one count draw, then per-slot idx + value draws.)
  int32_t v[kEditParamCount];
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    v[i] = stable_value_[i];

  const uint32_t count = 2U + (uint32_t)rnd_int(5U); // 2..6 parameters
  for (uint32_t c = 0; c < count; ++c)
  {
    const uint8_t idx = (uint8_t)rnd_int(kEditParamCount);
    const int32_t base = v[idx];
    int32_t x = rnd_draw(idx);
    for (uint32_t t = 0; t < kRndRedrawMax; ++t)
    {
      int32_t d = x - base;
      if (d < 0)
        d = -d;
      if (d >= (int32_t)kRndMinStepRaw)
        break;
      x = rnd_draw(idx);
    }
    v[idx] = x;
  }

  // Commit to the live state + DSP. stable_value_ is intentionally left
  // untouched: ORBI never stabilizes.
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    edit_value_[i] = v[i];
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    apply_edit_param(kEditParamFirst + i, v[i]);
}

// Re-copy the live edit_value_ into the stable-state snapshot (see
// Osc::stableSync()). Audio-thread safe (rndTick path) and main-thread
// (web-sim preset path).
inline void Osc::stableSync()
{
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    stable_value_[i] = edit_value_[i];
}
