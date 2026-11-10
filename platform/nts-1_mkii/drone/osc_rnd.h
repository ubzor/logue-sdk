#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_rnd.h
 *
 *  Out-of-line Rnd re-roll machinery: the hardware Rnd slot debounce (0.5 s
 *  window, rndArm/rndTick), the web-sim Random/All/Part/Orbi request
 *  channels (rndRequest/simRndRequest), the randomization TYPE state (RndM
 *  slot: kRndMode* / rnd_mode_), and the BPM-synced re-roll MORPH engine
 *  (rndMorphStart / rndMorphTick, smoothstep per-parameter transition with
 *  random stagger and one anchored parameter). rndTick() dispatches to the
 *  re-roll actions themselves (randomizeAll / randomizePartial /
 *  randomizeOrbi, defined in osc_randomize.h, included after this file).
 *
 *  This was the largest non-audio block of osc.h (~450 lines of state and
 *  methods). It now lives here so the class skeleton stays small. It is
 *  #included at the BOTTOM of osc.h, after the Osc class is complete, so it
 *  can reference every data member (rnd_*, morph_*, edit_value_,
 *  stable_value_, tempo_bpm_) and the rnd_int() primitive (declared in the
 *  class body, defined in osc_randomize.h).
 *
 */

#include "osc.h"  // Osc class (complete definition) + osc_consts::* + helpers

// Arm the Rnd re-roll from the K-rate thread (hardware slot 4) or the UI
// thread (web sim *Random* button).
//
//   debounced == true  (hardware slot 4):
//     Called on EVERY Rnd value change (the edit-menu knob sends one per
//     detent). The first change re-rolls IMMEDIATELY (rnd_fire_ -- consumed
//     on the next process() block) and starts the 0.5 s window; every
//     further change inside the window only marks rnd_armed_ (one final
//     re-roll when the window elapses) and EXTENDS the window to
//     (now + kRndDebounceSamples) so a continued knob turn keeps pushing the
//     deadline back -- the "reset to 0.5 s".
//
//   debounced == false (web sim *Random* button):
//     A single click is a single event (not a knob-turn stream), so there is
//     nothing to debounce. Just set rnd_fire_ -> immediate re-roll on the
//     next block, no window, no deferred final re-roll. Every click fires.
//
// rnd_now_ is read here (K-rate) and written by the audio thread; the read is
// at most one block (~2.9 ms) stale, negligible against the 0.5 s window.
inline void Osc::rndArm(bool debounced)
{
  if (!debounced)
  {
    // Sim button: immediate re-roll on every click, no debounce.
    rnd_fire_ = 1;
    return;
  }
  const uint32_t now = rnd_now_;
  const uint32_t until = now + kRndDebounceSamples;
  if (now >= rnd_cooldown_until_)
  {
    // Outside any window: this is a fresh change -> immediate re-roll.
    rnd_fire_ = 1;
  }
  else
  {
    // Inside the window: a change happened -> one final re-roll on expiry.
    rnd_armed_ = 1;
  }
  // Extend the window (monotonic; each change resets the 0.5 s countdown).
  rnd_cooldown_until_ = until;
}

// Start a morph toward a fresh set of targets (see the state block above).
// Called from the audio thread by randomizeAll/Partial/Orbi() with the new
// raw values (target[i]) for ALL kEditParamCount parameters; only the params
// that actually CHANGE become active (target != the param's current target).
// If a morph is already in flight, the in-flight params are RE-AIMED: their
// `from` becomes the CURRENT interpolated value (seamless re-target, no
// jump) and their stagger is kept, so the whole sound keeps flowing toward
// the new targets instead of restarting from scratch. The tempo at fire
// time sets the phrase length (a tempo change mid-morph does NOT re-aim an
// in-flight morph -- it affects the NEXT fire only).
// `phrase_beats` overrides the morph's length in beats for this fire (0 =
// use the default kRndMorphBeats). Orbit passes a shorter phrase (a quarter
// bar) so its development reads as a quick, audible shift instead of
// dissolving over a full bar; All/Part leave it 0 (unchanged default).
inline void Osc::rndMorphStart(const int32_t *target, uint32_t phrase_beats)
{
  float bpm = tempo_bpm_;
  if (bpm < kRndMorphBpmMin)
    bpm = kRndMorphBpmMin;
  else if (bpm > kRndMorphBpmMax)
    bpm = kRndMorphBpmMax;
  // Phrase length in samples: `phrase_beats` (or the default kRndMorphBeats)
  // beats at `bpm` (1 beat = 60/bpm s).
  const uint32_t beats = (phrase_beats > 0U) ? phrase_beats : kRndMorphBeats;
  const uint32_t total = (uint32_t)((float)beats * 60.0f / bpm * 48000.0f);

  const bool reaim = morph_active_;
  if (!reaim)
  {
    // Fresh morph: clean slate (nothing active until the loop below adds).
    morph_cnt_ = 0;
    morph_elapsed_ = 0;
  }
  if (reaim)
  {
    // RE-AIM: handle EVERY parameter (not just the currently-active ones).
    // The new fire may change parameters that were NOT in the previous
    // morph (e.g. Orbi re-rolls a different 2..6-subset each time). For
    // those, edit_value_ is about to be overwritten by the caller (see
    // osc_randomize.h) with the new target -- if we don't add them to the
    // morph here, their slider jumps instantly to the target (no glide)
    // and the DSP snaps (a hard discontinuity). So:
    //   * param already active in this morph AND re-targeted:
    //       - safely mid-flight (0<t<1): C1-continuous re-aim (re-solve from).
    //       - in stagger (t~0) or at/over target (t~1): plain re-start from
    //         the current value (start NOW, from = cur, t = 0). No division,
    //         no garbage read, no jump.
    //   * param NOT active in this morph but re-targeted by the new fire:
    //       add it to the morph NOW (from = edit_value_ [still the OLD value
    //       -- the caller overwrites edit_value_ AFTER rndMorphStart],
    //       to = new target, cur = from, t = 0, start = elapsed [no stagger
    //       on a re-aim -- the user already clicked, start immediately]).
    //   * param unchanged (new_to == current edit_value_): skip (nothing to
    //       morph; it keeps its value, active or not).
    //
    // "current edit_value_" for the from-value: for an ACTIVE param the
    // live value is morph_cur_[i] (the in-flight interpolation); for an
    // INACTIVE param it is edit_value_[i] (which is still the old value --
    // the caller has not overwritten it yet). We pick the source based on
    // whether the param is in morph_idx_[0..cnt-1].

    // First pass: mark which params are already active (for O(1) lookup).
    bool active_in_morph[kEditParamCount];
    for (uint8_t i = 0; i < kEditParamCount; ++i) active_in_morph[i] = false;
    for (uint8_t s = 0; s < morph_cnt_; ++s)
      active_in_morph[morph_idx_[s]] = true;

    for (uint8_t i = 0; i < kEditParamCount; ++i)
    {
      if (target == nullptr)
        continue; // no new targets (shouldn't happen; rndTrigger always draws)
      // CINT (idx 17) / CINT2 (idx 30) are NOT morphed: the chord interval
      // crossfades instead (old note fades out, new note fades in -- see the
      // pitch crossfade in osc_mix.h). The new target is applied IMMEDIATELY
      // here (not by the morph engine, which skips these two), through the
      // same apply_edit_param() path a manual edit uses -- which resets the
      // "new" phasor and starts the 2 s crossfade. edit_value_[i] is still the
      // OLD value (the caller overwrites it AFTER rndMorphStart), so the
      // != check detects a real change and avoids re-starting a crossfade for
      // an unchanged interval. CINT/CINT2 are never active in the morph
      // (skipped everywhere), so edit_value_ is their live value.
      if (i == 17 || i == 30)
      {
        if (target[i] != edit_value_[i])
          apply_edit_param(kEditParamFirst + i, target[i]);
        continue;
      }
      const int32_t new_to = target[i];
      // The param's CURRENT value (what the sound is at RIGHT NOW):
      const int32_t cur_val = active_in_morph[i] ? morph_cur_[i] : edit_value_[i];
      if (new_to == cur_val)
        continue; // not actually changing: nothing to morph
      if (new_to == morph_to_[i] && active_in_morph[i])
        continue; // already gliding to this exact target: leave it be

      if (active_in_morph[i])
      {
        // Already in this morph: re-aim it in place.
        const uint32_t end = morph_start_[i] + morph_total_smp_;
        const bool in_flight = (morph_elapsed_ >= morph_start_[i]) &&
                               (morph_elapsed_ <  end);
        if (in_flight && (morph_t_[i] > 0.001f) && (morph_t_[i] < 0.999f))
        {
          const float t  = morph_t_[i];
          const float st = t * t * (3.0f - 2.0f * t);
          morph_from_[i] = (int32_t)((morph_cur_[i] - new_to * st) / (1.0f - st) + 0.5f);
        }
        else
        {
          morph_from_[i] = morph_cur_[i];
          morph_start_[i] = morph_elapsed_;
          morph_t_[i] = 0.0f;
        }
        morph_to_[i] = new_to;
      }
      else
      {
        // New to this morph: seat it and append to the active list.
        morph_from_[i] = edit_value_[i]; // still the OLD value (pre-overwrite)
        morph_to_[i] = new_to;
        morph_cur_[i] = morph_from_[i];
        morph_t_[i] = 0.0f;
        morph_start_[i] = morph_elapsed_; // start NOW (no stagger on re-aim)
        morph_idx_[morph_cnt_] = i;
        ++morph_cnt_;
      }
    }
    morph_total_smp_ = total;
    morph_active_ = (morph_cnt_ > 0U) ? 1 : 0;
    return;
  }

  // ---- FRESH morph ---------------------------------------------------------
  for (uint8_t i = 0; i < kEditParamCount; ++i)
  {
    // CINT (idx 17) / CINT2 (idx 30) are NOT morphed: the chord interval
    // crossfades instead (see the re-aim loop above and osc_mix.h). Apply the
    // new target IMMEDIATELY here (the morph engine skips these two): this
    // resets the "new" phasor and starts the 2 s crossfade via apply_edit_param.
    // edit_value_[i] is still the OLD value (the caller writes it AFTER
    // rndMorphStart), so the != check applies only on a real change.
    if (i == 17 || i == 30)
    {
      if (target != nullptr && target[i] != edit_value_[i])
        apply_edit_param(kEditParamFirst + i, target[i]);
      continue;
    }
    const bool has_target = (target != nullptr) && (target[i] != edit_value_[i]);
    if (!has_target)
      continue; // unchanged: nothing to do
    // Fresh: morph from the current value to the new target. edit_value_ is
    // still the OLD value here (the caller writes it AFTER rndMorphStart,
    // see osc_randomize.h), so this is the value the sound is actually at.
    morph_from_[i] = edit_value_[i];
    morph_to_[i] = target[i];
    morph_cur_[i] = morph_from_[i];
    morph_t_[i] = 0.0f;
    // Random stagger: 0..(kRndMorphStaggerDivs-1) subdivisions of the phrase,
    // so the params roll in across the window.
    morph_start_[i] = (uint32_t)((uint32_t)rnd_int(kRndMorphStaggerDivs) * total / kRndMorphStaggerDivs);
    morph_idx_[morph_cnt_] = i;
    ++morph_cnt_;
  }
  // ANCHOR (fresh morphs only -- re-aims returned above): guarantee that at
  // least ONE parameter starts IMMEDIATELY (morph_start_ = 0), so the user
  // sees/hears a change on the very first audio block after the fire. The
  // anchor is the active parameter with the LARGEST raw delta (the most
  // audible change); if the morph has only one active parameter, that one is
  // the anchor. The other parameters keep their random stagger, so the
  // musical "roll-in" effect is preserved.
  if (morph_cnt_ > 0U)
  {
    uint8_t anchor = morph_idx_[0];
    int32_t max_delta = 0;
    for (uint8_t s = 1U; s < morph_cnt_; ++s)
    {
      const uint8_t i = morph_idx_[s];
      int32_t d = morph_to_[i] - morph_from_[i];
      if (d < 0)
        d = -d;
      if (d > max_delta)
      {
        max_delta = d;
        anchor = i;
      }
    }
    morph_start_[anchor] = 0;
  }
  morph_total_smp_ = total;
  morph_active_ = (morph_cnt_ > 0U) ? 1 : 0;
}

// Audio-thread tick, called once per process() block with the block's frame
// count, AFTER the re-rolls (rndTick) so a fresh fire this block is picked
// up in the very same block. Advances the morph clock and, for each active
// parameter whose start delay has elapsed, applies the smoothstep-
// interpolated raw value through apply_edit_param() (the same path as a
// manual PARAM_VAL edit, so all derived-voice caches stay consistent). When
// a param reaches its end it snaps exactly to the target (kills float-lerp
// residue) and retires from the active list; when the last param retires the
// morph disarms (the idle path is a single branch check from then on).
inline void Osc::rndMorphTick(uint32_t frames)
{
  if (!morph_active_)
    return;
  morph_elapsed_ += frames;
  bool any_left = false;
  for (uint8_t s = 0; s < morph_cnt_; ++s)
  {
    const uint8_t i = morph_idx_[s];
    const uint32_t end = morph_start_[i] + morph_total_smp_;
    if (morph_elapsed_ >= end)
    {
      // Done: snap exactly to the target and retire (swap with the last).
      if (morph_cur_[i] != morph_to_[i])
      {
        edit_value_[i] = morph_to_[i];
        apply_edit_param(kEditParamFirst + i, morph_to_[i]);
      }
      morph_cur_[i] = morph_to_[i];
      morph_t_[i] = 1.0f; // at target (matters if a later re-aim reads it)
      morph_idx_[s] = morph_idx_[morph_cnt_ - 1U];
      --morph_cnt_;
      --s; // re-process the swapped-in slot
      continue;
    }
    if (morph_elapsed_ < morph_start_[i])
    {
      morph_t_[i] = 0.0f; // still in its stagger delay
      any_left = true;    // start delay not elapsed yet
      continue;
    }
    // In flight: smoothstep over [start, start+total).
    const float t = (float)(morph_elapsed_ - morph_start_[i]) / (float)morph_total_smp_;
    const float tt = t * t * (3.0f - 2.0f * t); // smoothstep 3t^2 - 2t^3
    morph_t_[i] = t; // remember progress for a possible C1 re-aim
    const int32_t raw = (int32_t)(morph_from_[i] + (morph_to_[i] - morph_from_[i]) * tt + 0.5f);
    if (raw != morph_cur_[i])
    {
      morph_cur_[i] = raw;
      edit_value_[i] = raw; // keep the display (PARAM VAL) tracking the morph
      apply_edit_param(kEditParamFirst + i, raw);
    }
    any_left = true;
  }
  if (!any_left)
    morph_active_ = 0;
}
inline uint8_t Osc::getRndMode() const { return rnd_mode_; }
inline void Osc::setRndMode(uint8_t mode) { rnd_mode_ = (mode <= kRndModeOrbi) ? mode : kRndModeAll; }
inline uint8_t Osc::getRndRun() const { return rnd_run_; }
inline void Osc::setRndRun(uint8_t v) { rnd_run_ = v; }

// Raw value of edit parameter i as the web simulator's UI thread reads it
// (the slider-sync loop): while a morph is in flight this is the CURRENT
// interpolated raw value, so the sim's sliders visibly "travel" toward the
// re-roll target instead of jumping. Returns the plain edit_value_ when no
// morph is active, OR when this parameter is NOT part of the active morph
// (unchanged by the re-roll): for such a parameter morph_cur_[i] is stale
// garbage (only re-targeted params get it (re)seated in rndMorphStart), and
// its true value is simply edit_value_[i] (the morph leaves it untouched).
// Read-only for the UI (audio thread writes morph_cur_ / morph_idx_).
inline int32_t Osc::getEditMorphValue(uint8_t idx) const
{
  if (idx >= kEditParamCount)
    return 0;
  if (!morph_active_)
    return edit_value_[idx];
  // Active in the current morph? (index present in morph_idx_[0..cnt-1])
  for (uint8_t s = 0; s < morph_cnt_; ++s)
    if (morph_idx_[s] == idx)
      return morph_cur_[idx];
  return edit_value_[idx]; // unchanged param: keep its real value, no garbage
}

// True while a Rnd re-roll morph is in flight (any parameter still moving
// or still in its stagger delay). The web simulator's UI thread uses this
// to know when to stop the post-fire slider-sync poll: the poll must NOT
// stop on a "no value changed this frame" tick during the stagger window
// (before any parameter has started moving), because the morph is still in
// progress -- it must wait until rndMorphTick disarms morph_active_ (all
// parameters have reached their targets). Inlined branch, zero cost on ARM;
// only called from the UI thread in the web build.
inline bool Osc::isMorphActive() const { return morph_active_ != 0U; }

// Audio-thread tick, called once per process() block with the block's frame
// count. Advances the audio clock, then consumes pending re-rolls:
//   * rnd_fire_ (immediate, from a fresh change) always fires.
//   * rnd_armed_ (a change inside the window) fires once the window elapses.
// The re-roll itself honours the current rnd_mode_ (All -> randomizeAll,
// Part -> randomizePartial, Orbi -> randomizeOrbi).
inline void Osc::rndTick(uint32_t frames)
{
  rnd_now_ += frames;
  const bool fire = rnd_fire_;
  if (fire)
    rnd_fire_ = 0;
  bool do_final = false;
  if (rnd_armed_ && rnd_now_ >= rnd_cooldown_until_)
  {
    rnd_armed_ = 0;
    do_final = true;
  }
  if (fire || do_final)
  {
    if (rnd_mode_ == kRndModePart)
      randomizePartial();
    else if (rnd_mode_ == kRndModeOrbi)
      randomizeOrbi();
    else
      randomizeAll();
  }
}

#ifdef __EMSCRIPTEN__
// ---- Direct edit-parameter channel (web simulator only) -------------------
// On hardware the PARAM/VALUE multiplexer is driven by the platform's
// unit_set_param_value() callback (K-rate, atomic per knob turn). The web
// simulator's per-parameter sliders used to write to the WebAudioParam
// slots 0/1 (PARAM_SEL / PARAM_VAL), but K-rate WebAudioParam smoothing
// causes select_ to sweep through intermediate values (0 -> 1 -> 2 -> ...
// -> target), each of which re-applies a different edit parameter to the
// DSP -- producing audible glitches that look like a phantom LFO.
//
// To fix this, the UI bypasses WebAudioParam entirely and writes a
// packed (index, raw) request to this single 32-bit slot. The request is
// consumed once per block by process() (audio worklet thread), so the
// (select_, PARAM_VAL) pair is applied atomically -- the same write
// semantics the hardware has.
//
// Layout: bit 31 = valid flag, bits 0..4 = index (0..15), bits 8..17 =
// raw value (0..1023). A 32-bit aligned store on x86 is atomic; even if
// the worklet observed a torn value it would just skip the request
// (valid bit would not be consistent with the payload).
//
// The hardware build does not use this channel: under non-EMSCRIPTEN
// the volatile slot is absent (zero size) and process() never touches it.

// Write an edit-parameter request from the main thread (UI). Index is
// 0..(kEditParamCount - 1), raw is 0..1023. Safe to call from any
// thread; the audio worklet thread picks it up on the next block.
// The index occupies 6 bits (bits 18..23) so kEditParamCount may grow
// up to 64 without re-packing. raw occupies 10 bits (bits 8..17).
// Bit 31 is the "request pending" flag; bits 24..30 are reserved (0).
inline void Osc::simEditRequest(uint8_t index, int32_t raw)
{
  if (index >= kEditParamCount)
    return;
  const uint32_t packed = (1U << 31) | (static_cast<uint32_t>(index) & 0x3FU) |
                          ((static_cast<uint32_t>(raw) & 0x3FFU) << 8);
  sim_edit_request_ = packed;
}

// Fire the *Random* button from the main thread (UI). A single click is a
// single event (no knob-turn stream), so it is NOT debounced: every click
// re-rolls immediately on the next process() block.
inline void Osc::rndRequest()
{
  rndArm(false);
}

// ---- Rnd re-roll request channel (main-thread UI -> audio worklet) --------
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

// Write an Rnd re-roll request from the main thread (UI). Mode is 0..2
// (kRndModeAll/kRndModePart/kRndModeOrbi); out-of-range values fall back to
// kRndModeAll. Safe to call from any thread; the worklet picks it up on the
// next block (see the consumption block in osc_mix.h::process).
inline void Osc::simRndRequest(uint8_t mode)
{
  const uint8_t m = (mode <= kRndModeOrbi) ? mode : kRndModeAll;
  sim_rnd_request_ = (1U << 31) | static_cast<uint32_t>(m);
}
#endif
