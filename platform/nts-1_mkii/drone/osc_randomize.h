#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_randomize.h
 *
 *  Out-of-line definition of Osc::randomizeAll() -- the single "re-roll every
 *  edit parameter to a fresh audition-friendly random preset" routine.
 *
 *  It is the ONE source of truth for randomization, shared by:
 *    * the hardware "Rnd" optional slot (unit.cc: unit_set_param_value(id=4)
 *      -> Osc::randomizeAll(), K-rate, fires once per value change), and
 *    * the web simulator's "Rnd" control (wasm.cc rndRequest() -> process() ->
 *      Osc::randomizeAll() on the audio-worklet thread).
 *  Both therefore produce bit-identical random windows (and the same
 *  osc_rand() Park-Miller-Carta source, see rnd_int below), so the simulator's
 *  "Rnd" and the device's "Rnd" re-roll the same distribution.
 *
 *  The raw windows below are the exact port of the web simulator's randomRaw()
 *  (websim/osc.html): "safe" (walk / timbre) parameters get the full 0..1023
 *  range; "critical" parameters are constrained to audition-friendly windows
 *  so the result always plays cleanly (no clipping / mud / dissonance /
 *  resonance spikes). The CINT / CINT2 intervals are drawn from the same
 *  discrete consonant set the sim uses, so the chord is always "in key".
 *
 *  It is #included at the BOTTOM of osc.h, after the Osc class is complete, so
 *  it can reference edit_value_ / select_ / apply_edit_param() / osc_white().
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

// ---- The re-roll itself ------------------------------------------------------
inline void Osc::randomizeAll()
{

  // ---- Full-range (walk / timbre) parameters --------------------------------
  // SPD1..3 / SMT1..3: stream period / time constant. Any value is safe.
  edit_value_[SPD1 - kEditParamFirst] = rnd_int(1024);
  edit_value_[SMT1 - kEditParamFirst] = rnd_int(1024);
  edit_value_[SPD2 - kEditParamFirst] = rnd_int(1024);
  edit_value_[SMT2 - kEditParamFirst] = rnd_int(1024);
  edit_value_[SPD3 - kEditParamFirst] = rnd_int(1024);
  edit_value_[SMT3 - kEditParamFirst] = rnd_int(1024);
  // RW_WAV / RW_DRV / RW_FLD / RW_FC: per-stream random-walk depths (0..100%).
  edit_value_[RW_WAV - kEditParamFirst] = rnd_int(1024);
  edit_value_[RW_DRV - kEditParamFirst] = rnd_int(1024);
  edit_value_[RW_FLD - kEditParamFirst] = rnd_int(1024);
  edit_value_[RW_FC - kEditParamFirst]  = rnd_int(1024);
  // WAV1 / WAV2 / WAV3: wavetable index (0..kWavesTotal-1 = 0..90), full span.
  edit_value_[WAV1 - kEditParamFirst] = drone_wav_raw(rnd_int((uint32_t)WaveTable::kWavesTotal));
  edit_value_[WAV2 - kEditParamFirst] = drone_wav_raw(rnd_int((uint32_t)WaveTable::kWavesTotal));
  edit_value_[WAV3 - kEditParamFirst] = drone_wav_raw(rnd_int((uint32_t)WaveTable::kWavesTotal));

  // ---- Constrained (critical) parameters ------------------------------------
  // (raw window = lo + rnd_int(hi-lo+1), i.e. uniform over lo..hi inclusive)
  // DETU: +/-5 c (raw 256..767) -- clear beat without going out of tune.
  edit_value_[DETUNE - kEditParamFirst] = 256 + rnd_int(767 - 256 + 1);
  // RATE: shared walk ceiling, 20..50% (raw 204..511) -- capped so the combined
  // wander doesn't clip/howl.
  edit_value_[RATE_AMT - kEditParamFirst] = 204 + rnd_int(511 - 204 + 1);
  // SUB1: 15..80% (raw 153..818) -- body without mud.
  edit_value_[SUB1 - kEditParamFirst] = 153 + rnd_int(818 - 153 + 1);
  // SUB2: 0..45% (raw 0..460) -- light low end, never the f/4 "mud" zone.
  edit_value_[SUB2 - kEditParamFirst] = rnd_int(460 + 1);
  // SATU: 40..240 (drive 0.4..2.4, raw 136..819) -- audible drive, no clip.
  edit_value_[SATU - kEditParamFirst] = 136 + rnd_int(819 - 136 + 1);
  // ASYM: +/-50% (raw 256..767) -- moderate asymmetry.
  edit_value_[ASYM - kEditParamFirst] = 256 + rnd_int(767 - 256 + 1);
  // FOLD: 0..60% (raw 0..613) -- metallic character, no "metal mush".
  edit_value_[FOLD - kEditParamFirst] = rnd_int(613 + 1);
  // CHORD: 0..60% (raw 0..613) -- presence without masking.
  edit_value_[CHORD - kEditParamFirst] = rnd_int(613 + 1);
  // CINT / CINT2: discrete consonant intervals (see kRndCintSet).
  edit_value_[CHRD_INT - kEditParamFirst] = drone_cents_raw(kRndCintSet[rnd_int(9)]);
  edit_value_[CINT2 - kEditParamFirst]    = drone_cents_raw(kRndCintSet[rnd_int(9)]);
  // MAIN: 55..100% (raw 563..1023) -- stays clearly present (never hollow).
  edit_value_[MAIN - kEditParamFirst] = 563 + rnd_int(1023 - 563 + 1);
  // FCUT: 1 kHz..11 kHz (raw 740..1023) -- working band, never "in a box".
  edit_value_[FCUT - kEditParamFirst] = 740 + rnd_int(1023 - 740 + 1);
  // RES: 0.5..1.8 (raw 0..669) -- gentle peak, no ringy spike.
  edit_value_[RES - kEditParamFirst] = rnd_int(669 + 1);
  // RVLL: breath depth 20..80% (raw 204..818) -- audible, no voice vanishing.
  edit_value_[RW_LVL - kEditParamFirst] = 204 + rnd_int(818 - 204 + 1);
  // RW_LSP / RW_LSM: breath period / time constant, full range.
  edit_value_[RW_LSP - kEditParamFirst] = rnd_int(1024);
  edit_value_[RW_LSM - kEditParamFirst] = rnd_int(1024);
  // CHSAT: chord drive 0..60% (raw 0..613) -- gentle-to-moderate, no clip.
  edit_value_[CHSAT - kEditParamFirst] = rnd_int(613 + 1);
  // CHFLD: chord fold 0..50% (raw 0..511) -- light edge, no harsh aliasing.
  edit_value_[CHFLD - kEditParamFirst] = rnd_int(511 + 1);
  // CHSAT2: CINT2's own drive 0..60% (raw 0..613) -- same window as CHSAT, so the
  // CINT2 interval is driven independently but to the same safe ceiling.
  edit_value_[CHSAT2 - kEditParamFirst] = rnd_int(613 + 1);
  // CHFLD2: CINT2's own fold 0..50% (raw 0..511) -- same window as CHFLD.
  edit_value_[CHFLD2 - kEditParamFirst] = rnd_int(511 + 1);
  // JIT: 0..15% (raw 0..153) -- light "smear", never harsh static.
  edit_value_[JIT - kEditParamFirst] = rnd_int(153 + 1);
  // RMOD: 0..25% (raw 0..256) -- gentle metallic "ring".
  edit_value_[RMOD - kEditParamFirst] = rnd_int(256 + 1);
  // FM_A / FM_B: static phase-FM 0..30% (raw 0..307) -- audible color, no screech.
  edit_value_[FM_A - kEditParamFirst] = rnd_int(307 + 1);
  edit_value_[FM_B - kEditParamFirst] = rnd_int(307 + 1);
  // RFM_A / RFM_B: FM-walk depth 0..50% (raw 0..511) -- "breathing" on the FM.
  edit_value_[RFM_A - kEditParamFirst] = rnd_int(511 + 1);
  edit_value_[RFM_B - kEditParamFirst] = rnd_int(511 + 1);
  // ---- Enrichment-2 (formant / tape / comb / fold2 / ducking) ----
  // FORM: any vowel (full range 0..1023) -- the formant stage is OFF until
  // FVOL > 0, so this is musically free (no clipping).
  edit_value_[FORM - kEditParamFirst] = rnd_int(1024);
  // FVOL: 0..50% (raw 0..512) -- a clear formant color, never the full-level
  // resonant biquad sum that would mask the drone.
  edit_value_[FVOL - kEditParamFirst] = rnd_int(512 + 1);
  // COMB: 0..30% (raw 0..307) -- a gentle resonance, never the ringy fb>0.7
  // zone that "rings" harshly.
  edit_value_[COMB - kEditParamFirst] = rnd_int(307 + 1);
  // FOLD2: 0..30% (raw 0..307) -- a light "worn" edge, never the full double-
  // fold that aliases harshly at the upper partials.
  edit_value_[FOLD2 - kEditParamFirst] = rnd_int(307 + 1);
  // DCK: 0..25% (raw 0..256) -- a gentle low-end "breathing", never the full
  // duck that would silence the subs into a "hollow" drone.
  edit_value_[DCK - kEditParamFirst] = rnd_int(256 + 1);

  // ---- Apply every edit parameter to the live DSP ---------------------------
  // Re-run the same apply path init() uses, so params_ (and the derived phasor
  // caches / SVF coefficients) are fully consistent with the new raw values.
  // apply_edit_param() is idempotent per-parameter.
  for (uint8_t i = 0; i < kEditParamCount; ++i)
    apply_edit_param(kEditParamFirst + i, edit_value_[i]);
}