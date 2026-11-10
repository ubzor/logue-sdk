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
 *  values (a loose "development"), each with a guaranteed significant step
 *  (>= kRndMinStepRaw from current); randomizeOrbi() ("Orbit") is a TIGHT,
 *  SHORT development anchored to the last STABLE state (stable_value_): it
 *  nudges 2..4 parameters, each picked from ONE weighted family (core/timbre/
 *  motion/extra) so a single fire develops one "sphere" of the sound, and the
 *  whole fire morphs over a SHORT phrase (a quarter bar, not Part's full bar)
 *  so the shift lands quickly. Each nudge is a few % .. ~30% of range,
 *  not a fresh re-draw) so the result is recognizably the same sound. The
 *  result is applied to the live edit_value_ WITHOUT updating the stable state
 *  (see the function comments below).
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
static constexpr uint32_t kRndCintCount = 9;

// Minimum separation in RAW (0..1023) between the two NON-zero chord intervals
// (CINT / CINT2) after a re-roll. 94 raw ≈ 175 cents (175 * 1023 / 1900). If
// both intervals land within this window they sound like one beating note; the
// randomizer re-draws the second. A 0 (unison) on either side is allowed.
static constexpr int32_t kRndCintMinSepRaw = 94;

// Enforce minimum separation between CINT (idx 17) / CINT2 (idx 30) in `target`
// after a re-roll. Works in raw domain (no float conversion). If both are
// non-zero and |CINT-CINT2| < kRndCintMinSepRaw, re-draw CINT2 from
// kRndCintSet (non-zero, far enough from CINT) until it fits (at most 9 tries,
// then accept the last draw -- a small step is better than no step). Audio-thread
// only (uses rnd_int / osc_rand). ~40 B in the binary.
inline void Osc::ensure_cint_separation(int32_t* target)
{
  const int32_t a = target[17]; // CINT raw
  int32_t b = target[30];       // CINT2 raw
  if (a == 0 || b == 0)
    return; // unison: no second voice to separate from
  int32_t d = a - b;
  if (d < 0)
    d = -d;
  if (d >= kRndCintMinSepRaw)
    return; // already separated
  // Too close: re-draw CINT2 until it's far enough from CINT.
  for (uint32_t t = 0; t < 9U; ++t)
  {
    const int32_t cand = kRndCintSet[rnd_int(kRndCintCount)];
    if (cand == 0)
      continue; // skip unison: we need a non-zero second voice
    int32_t dd = a - cand;
    if (dd < 0)
      dd = -dd;
    if (dd >= kRndCintMinSepRaw)
    {
      target[30] = cand;
      return;
    }
    b = cand; // keep the last draw as fallback
  }
  target[30] = b;
}

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
// members DETUNE..COMB / kEditParamFirst). It is a plain static array, built
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
    case 8:  w.kind=0; w.lo=204;  w.hi=818;  w.div=0; break; // SUB1  20..80% (bass always audible)
    case 9:  w.kind=0; w.lo=256;  w.hi=716;  w.div=0; break; // SUB2  25..70% (bass always audible)
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
    // (FOLD2 / DCK removed: FOLD2 no longer exists, DCK is now a constant.)
    // (FLVL/COMB formant+comb removed: moved to the DRONE-MOD unit.)
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

// ---- Orbit Random (RndM = ORBI) tuning: a "development", not a mutation ----
// Part (kRndModePart) is the LOOSE dev: it re-rolls a random SUBSET (2..6) of
// the 41 parameters, each to a FRESH safe-window value (a guaranteed >= 10%
// step from the current raw). That is a real mutation: a few of the 41 knobs
// can each jump up to ~50% of their range, so the result can be a different
// sound entirely.
//
// Orbit (kRndModeOrbi) is the TIGHT dev -- the same "pick a subset and nudge it"
// idea, but tuned so the result is recognizably the same sound, developed:
//   * A MODERATE number of parameters move per fire: 2..4
//     (kRndOrbiMinCount..kRndOrbiMaxCount), not Part's 2..6.
//   * A MINIMUM step (kRndOrbiMinStepRaw, ~6% of the 10-bit range) and a
//     MAXIMUM step (kRndOrbiMaxStepRaw, ~30%), so each parameter moves a few
//     percent to ~30%, not up to half its range -- but it is still always
//     audible.
//   * The whole fire morphs over a SHORT phrase (kRndOrbiBeats/kRndOrbiBeatDiv
//     = a quarter bar, ~0.5 s at 120 BPM) instead of Part's full bar, so the
//     development lands quickly and reads as a clear shift, not a slow fade.
//   * ONE "sphere" of the sound moves per fire (the weighted families below):
//     a single fire develops the harmony, or the timbre, or the motion -- not
//     all three at once. That is what keeps Orbit feeling like "the same sound,
//     explored", whereas Part can reach across the whole 41-param space.
//   * A small "surprise" chance (kRndOrbiSurpriseChance of 100) still lets a fire
//     escape the chosen family and touch any parameter (a bigger discovery).
// The families group the edit parameters by what a move there affects:
//   core (the harmonic core: detune/subs, A/B/C wavetables, chord, intervals),
//   timbre (the color: saturation/fold, filter, ring-mod, comb),
//   motion (the movement: RATE, the wander streams and walk depths, breath, FM),
//   extra  (the enrichment layers, default OFF -- a low-weight "surprise").
// Any parameter not in a family is still reachable via the surprise draw, so
// no parameter is ever permanently excluded from Orbit.
// Orbit fires SHORT and DENSE, not slow and spread-out: the whole re-roll
// morphs over kRndOrbiBeats of a bar (0.25 = ~0.5 s at 120 BPM) instead of
// Part's full bar, and each moved parameter takes a BIGGER step (a few % to
// ~30% of range) across 2..4 parameters. This is what makes an Orbit fire
// audibly "happen" instead of dissolving into the drone's constant breathing
// (the old full-bar / small-step version read as "nothing changed"). Part and
// All keep their original full-bar morph and 2..6 params -- Orbit is the
// quicker, punchier development.
static constexpr uint32_t kRndOrbiBeats          = 1U;  // numerator; beats = kRndOrbiBeats / kRndOrbiBeatDiv
static constexpr uint32_t kRndOrbiBeatDiv        = 4U;  // 1/4 of a bar = 0.5 s @ 120 BPM
static constexpr uint32_t kRndOrbiMinCount       = 2U;  // 2..4 params per Orbit fire
static constexpr uint32_t kRndOrbiMaxCount       = 4U;  // (inclusive ceiling)
static constexpr uint32_t kRndOrbiMinStepRaw     = 64U; // ~6% of 1024 (the nudge floor)
static constexpr uint32_t kRndOrbiMaxStepRaw     = 307U; // ~30% of 1024 (the nudge cap)
static constexpr uint32_t kRndOrbiSurpriseChance = 4U;  // ~4% of fires escape the family
static constexpr uint32_t kRndOrbiFamilyCount    = 4U;  // core / timbre / motion / extra
static constexpr uint8_t  kRndOrbiFamilyW[kRndOrbiFamilyCount] = {50U, 20U, 20U, 10U};

// The weighted families: a flat list of edit-param indices, one per family
// (order = kRndOrbiFamilyCount). C++11-safe plain static initializers.
static const uint8_t kRndOrbiFamCore[12]   = {1, 8, 9, 13, 14, 15, 16, 17, 18, 30, 31, 32};
static const uint8_t kRndOrbiFamTimbre[9]  = {10, 11, 12, 22, 23, 28, 29, 37, 38};
static const uint8_t kRndOrbiFamMotion[16] = {0, 2, 3, 4, 5, 6, 7, 19, 20, 21, 24, 25, 26, 27, 33, 34};
static const uint8_t kRndOrbiFamExtra[2]   = {9, 35};

// The family's parameter list + length for an Orbit fire (family 0..3).
static const uint8_t *rnd_orbi_family(uint8_t fam, uint32_t &n)
{
  switch (fam)
  {
    case 0:  n = sizeof(kRndOrbiFamCore) / sizeof(kRndOrbiFamCore[0]);   return kRndOrbiFamCore;
    case 1:  n = sizeof(kRndOrbiFamTimbre) / sizeof(kRndOrbiFamTimbre[0]); return kRndOrbiFamTimbre;
    case 2:  n = sizeof(kRndOrbiFamMotion) / sizeof(kRndOrbiFamMotion[0]); return kRndOrbiFamMotion;
    case 3:  n = sizeof(kRndOrbiFamExtra) / sizeof(kRndOrbiFamExtra[0]);  return kRndOrbiFamExtra;
    default: n = 0; return kRndOrbiFamCore;
  }
}

// Draw a fresh raw value for one edit parameter from its safe window (see
// rnd_window_for()). Used by randomizeAll() (every param) and
// randomizePartial() (a subset).
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

// Pick a random edit parameter for one ORBIT fire. On the small "surprise"
// chance (kRndOrbiSurpriseChance of 100) take ANY of the kEditParamCount
// parameters (a bigger discovery); otherwise draw the FAMILY (0..3) with the
// kRndOrbiFamilyW weights (sum 100: 50/20/20/10) and a parameter inside it.
// Returns the edit index (0..kEditParamCount-1). Audio-thread only.
inline uint8_t Osc::rnd_pick_orbi_param()
{
  if (rnd_int(100U) < kRndOrbiSurpriseChance)
    return (uint8_t)rnd_int(kEditParamCount); // surprise: any parameter
  const uint32_t r = rnd_int(100U);
  uint8_t fam = 0;
  if (r < 50U)      fam = 0;
  else if (r < 70U) fam = 1;
  else if (r < 90U) fam = 2;
  else              fam = 3;
  uint32_t n = 0;
  const uint8_t *list = rnd_orbi_family(fam, n);
  return list[rnd_int(n ? n : 1U)];
}

// Draw an ORBIT raw value for one edit parameter: a gentle NUDGE of `base`
// rather than a fresh window draw. Take the window draw (rnd_draw), re-draw up
// to kRndRedrawMax times aiming for a step within [kRndOrbiMinStepRaw,
// kRndOrbiMaxStepRaw] of `base`, then clamp the accepted draw to that band so a
// single Orbit fire never moves a parameter more than a few percent of its range
// (but always by at least the minimum nudge). Audio-thread only.
inline int32_t Osc::rnd_draw_orbi(uint8_t idx, int32_t base)
{
  int32_t v = rnd_draw(idx);
  for (uint32_t t = 0; t < kRndRedrawMax; ++t)
  {
    int32_t d = v - base;
    if (d < 0)
      d = -d;
    if (d >= (int32_t)kRndOrbiMinStepRaw && d <= (int32_t)kRndOrbiMaxStepRaw)
      break;
    v = rnd_draw(idx);
  }
  int32_t d = v - base;
  if (d > (int32_t)kRndOrbiMaxStepRaw)
    v = base + (int32_t)kRndOrbiMaxStepRaw;
  else if (d < -(int32_t)kRndOrbiMaxStepRaw)
    v = base - (int32_t)kRndOrbiMaxStepRaw;
  return v;
}

// ---- The re-roll itself ------------------------------------------------------
// All-mode re-roll: draw a fresh safe value for EVERY edit parameter. The new
// values become the MORPH TARGETS (rndMorphStart): the live sound then glides
// from the current raw values to the new ones over a BPM-synced phrase with
// per-parameter stagger (see Osc::rndMorphStart / rndMorphTick in osc.h). The
// OLD apply-all loop is gone -- the morph engine re-applies the in-flight
// parameters through the same apply_edit_param() path each audio block, so the
// derived-voice caches (wavetables, chords, SVF, formant) stay consistent
// while the values move. edit_value_ is written to the TARGETS immediately
// (the display / stable state show the destination; the morph drives the DSP
// toward it) -- but rndMorphStart must run BEFORE that write so it can read
// the OLD values as the morph's `from` (it falls back to morph_cur_ for
// in-flight params, which already hold the current interpolated value).
inline void Osc::randomizeAll()
{
  int32_t target[kEditParamCount];
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    target[i] = rnd_draw(i);

  // CINT / CINT2: enforce minimum separation so the two chord voices don't
  // land within kRndCintMinSepRaw of each other (they'd sound like one beating
  // note). Works in raw domain (no float conversion). ~20 B.
  ensure_cint_separation(target);

  // Start the morph toward the fresh targets (reads the current values as
  // `from`). Must run BEFORE edit_value_ is overwritten below.
  rndMorphStart(target);

  for (uint8_t i = 0; i < kEditParamCount; ++i)
    edit_value_[i] = target[i];

  // STABILIZE: an All re-roll is a fresh stable state -- the ORBI reference
  // (stable_value_) now points at this result (the morph target, not the
  // in-flight value: an ORBI fire mid-morph develops from the destination).
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
  // Draw the fresh targets into a temp array (seeded from the CURRENT live
  // state, so the untouched params keep their exact values -- the morph engine
  // then only activates the params that actually changed). The LCG draw order
  // is unchanged from the pre-morph version: one count draw, then per-slot
  // idx + value draws.
  int32_t target[kEditParamCount];
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    target[i] = edit_value_[i];

  const uint32_t count = 2U + (uint32_t)rnd_int(5U); // 2..6 parameters
  uint8_t touched_cint = 0; // was CINT (17) or CINT2 (30) re-rolled this pass?
  for (uint32_t c = 0; c < count; ++c)
  {
    const uint8_t idx = (uint8_t)rnd_int(kEditParamCount);
    if (idx == 17U || idx == 30U)
      touched_cint = 1;
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
    target[idx] = v;
  }

  // CINT / CINT2 separation: if this pass re-rolled at least one interval,
  // enforce the minimum separation on the resulting pair. ~15 B.
  if (touched_cint)
    ensure_cint_separation(target);

  // Morph toward the targets (reads the current values as `from`; unchanged
  // params have target == old target, so the engine skips them). Then commit
  // the targets to edit_value_ (display / stable state show the destination).
  rndMorphStart(target);
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    edit_value_[i] = target[i];

  // STABILIZE: a Part re-roll is a fresh stable state too -- the ORBI
  // reference (stable_value_) now points at this result.
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    stable_value_[i] = edit_value_[i];
}

// ---- Orbit re-roll (RndM = ORBI) --------------------------------------------
// A TIGHT "development" of the LAST STABLE STATE, deliberately far gentler than
// a Part re-roll (see the Orbit tuning comment near kRndOrbiMaxCount above):
//   * the base for every nudge is stable_value_[idx] (the last stable state --
//     factory defaults at startup, or the result of the last All/Part re-roll);
//   * A MODERATE number of parameters move: 2..4 (kRndOrbiMinCount..kRndOrbiMaxCount),
//     each picked from ONE weighted family (core / timbre / motion / extra) so a
//     single fire develops one "sphere" of the sound (with a small surprise
//     chance to touch any param);
//   * each moved parameter is NUDGED by a step: at least kRndOrbiMinStepRaw
//     (~6%, still audible) but at most kRndOrbiMaxStepRaw (~30%), so it moves a
//     few percent to ~30% of its range, not up to half;
//   * the whole fire morphs over a SHORT phrase (kRndOrbiBeats/kRndOrbiBeatDiv
//     of a bar, ~0.5 s at 120 BPM), not Part's full bar -- so the shift lands
//     quickly and is clearly audible;
//   * the result is written to the LIVE edit_value_ and applied to the DSP, but
//     stable_value_ is NOT touched -- an ORBI fire does not create a new stable
//     state, so every ORBI fire develops from the same stable sound no matter
//     how many times it is fired (or how much the live sound has drifted).
// Manual PARAM/VALUE edits of the live parameters also leave stable_value_
// untouched by design.
inline void Osc::randomizeOrbi()
{
  // Work on a temp copy seeded from the stable state; edit_value_ is committed
  // only once, at the end.
  int32_t target[kEditParamCount];
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    target[i] = stable_value_[i];

  const uint32_t count = kRndOrbiMinCount + (uint32_t)rnd_int(kRndOrbiMaxCount - kRndOrbiMinCount + 1U); // 2..4
  uint8_t touched_cint = 0; // was CINT (17) or CINT2 (30) nudged this fire?
  for (uint32_t c = 0; c < count; ++c)
  {
    const uint8_t idx = rnd_pick_orbi_param();
    if (idx == 17U || idx == 30U)
      touched_cint = 1;
    target[idx] = rnd_draw_orbi(idx, target[idx]); // nudge from the stable value
  }

  // CINT / CINT2 separation: if this fire nudged at least one interval, keep the
  // resulting pair at least kRndCintMinSepRaw apart (same rule as Part/All).
  if (touched_cint)
    ensure_cint_separation(target);

  // Morph toward the targets (the live sound glides from its CURRENT values to
  // the Orbit result, not from the stable state). Orbit uses a SHORTER phrase
  // (kRndOrbiBeats/kRndOrbiBeatDiv of a bar) so the shift lands quickly and is
  // clearly audible, unlike Part/All's full-bar morph. stable_value_ is
  // intentionally left untouched: Orbit never stabilizes.
  rndMorphStart(target, kRndOrbiBeats / kRndOrbiBeatDiv);
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    edit_value_[i] = target[i];
}

// Re-copy the live edit_value_ into the stable-state snapshot (see
// Osc::stableSync()). Audio-thread safe (rndTick path) and main-thread
// (web-sim preset path).
inline void Osc::stableSync()
{
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    stable_value_[i] = edit_value_[i];
}
