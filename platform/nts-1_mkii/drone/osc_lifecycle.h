#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_lifecycle.h
 *
 *  Out-of-line unit lifecycle: init() (factory-state reset of every DSP
 *  stage), reset() (allNoteOff semantics on stop), the legato note handlers
 *  (noteOn/noteOff/allNoteOff: the drone is held until the same key is
 *  re-pressed or all-notes-off), and setPitch() (arms the 2 s linear pitch
 *  glide; the audio path in osc_mix.h advances it per sample).
 *
 *  This was a ~235-line block of osc.h. It now lives here so the class
 *  skeleton stays small. It is #included at the BOTTOM of osc.h, after the
 *  Osc class is complete, so it can reference every data member and call
 *  apply_edit_param() / recalc_pitch_derived() (osc_param_apply.h is
 *  included before this file).
 *
 */

#include "osc.h"  // Osc class (complete definition) + osc_consts::* + helpers

inline void Osc::init(float *) 
  {
    params_.reset();
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    chord_.reset(); // chord-voice phasor (the CINT interval default lives in params_.chord_cents)
    chord2_.reset(); // second chord-voice phasor (CINT2)
    phasor_sub_ = 0.f;
    phasor_sub2_ = 0.f;
    sub2_.lpf_state = 0.f;
    sub2_.gate = 0.f;
    w0_ = 0.f;
    lfo_ = 0.f;
    // Parameter selector: start with RATE (select_ = 0) selected (RATE leads
    // the multiplexer). Re-apply the cached default values so the DSP and the
    // PARAM SEL / PARAM VAL display agree on startup (the header's "init"
    // value of PARAM_VAL already matches edit_value_[0], but this keeps the
    // two in sync if either side is edited independently later).
    select_ = 0;
    for (uint8_t i = 0; i < kEditParamCount; i++)
      apply_edit_param(kEditParamFirst + i, edit_value_[i]);
    recalc_pitch_derived(); // resolve wavetables + all derived-voice phase increments from the default w0_
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // each stream starts centered, AMT=0 (off)
    svf_.reset(); // zero the SVF integrator states (coefficients set by apply_edit_param above)
    prev_wav_a_ = 0.f; // FM lookback cache: start at 0 (first sample is a no-op)
    prev_wav_b_ = 0.f;
    fm_smooth_a_ = 0.f; // phase-FM index LPF state (no FM on the first sample)
    fm_smooth_b_ = 0.f;
    // A-H-R envelope: start silent, no note active.
    note_on_ = false;
    last_note_ = 0;
    glide_active_ = false;
    glide_elapsed_ = 0;
    glide_from_ = 0.f;
    glide_to_ = 0.f;
    eg_.reset();
    // Reset the Rnd debounce state (both builds): no pending re-rolls, clock at
    // zero. (The debounce fields live outside the EMSCRIPTEN block so the
    // hardware build's Rnd slot shares them.)
    rnd_now_ = 0;
    rnd_cooldown_until_ = 0;
    rnd_fire_ = 0;
    rnd_armed_ = 0;
    // Stable state (ORBI reference): the factory default is the first stable
    // state (stable_value_'s default initializer already matches edit_value_,
    // but copy explicitly so the invariant holds even if the initializers
    // ever diverge).
    for (uint8_t i = 0; i < kEditParamCount; i++)
      stable_value_[i] = edit_value_[i];
#ifdef __EMSCRIPTEN__
    // Clear any pending direct edit-parameter request (the UI may have
    // already sent one before init ran on the worklet thread).
    sim_edit_request_ = 0;
    // Also clear any pending Rnd re-roll request (same race: a UI click that
    // landed before the worklet's init would otherwise fire a phantom re-roll
    // on the very first process() block after init).
    sim_rnd_request_ = 0;
#endif
    // First-order relaxation coefficients (per sample) for the exponential
    // attack/release. alpha = 1 - exp(-5 / N) where N is the phase length in
    // samples; five time-constants over the phase means the level covers
    // ~99.3% of the transition by the phase boundary. Precomputed here, never
    // in the audio path.
    // one_minus_expf (a small-x Taylor series, NOT libm expf and NOT the
    // inaccurate-for-tiny-x fasterexpf bit-trick) keeps the unit free of
    // unresolved libm symbols AND gives the correct ~5e-5 / ~1e-4 alphas so
    // the 2 s attack/release actually take 2 s. Computed once at init (K-rate).
    eg_alpha_a_ = one_minus_expf(-5.f / static_cast<float>(AhEnvelope::kAttackSamples));
    eg_alpha_r_ = one_minus_expf(-5.f / static_cast<float>(AhEnvelope::kReleaseSamples));
    // ---- Enrichment-2 state init (all OFF / transparent at default) --------
    // Comb / resonator: zero the ring buffer + reset the write cursor. The
    // comb is transparent (a pure wire) until COMB > 0, so a zeroed buffer is
    // exactly the default (regression-safe).
    for (uint32_t i = 0; i < kCombDelayMaxSmp; ++i)
      comb_buf_[i] = 0.f;
    comb_w_ = 0;
    // Formant: zero both comb ring buffers and write indices (the K-rate
    // delays/feedback are set by the apply_edit_param loop above, which
    // already ran for the default FORM/FVOL values -> formant_wet = 0 ->
    // stage is transparent, so a zeroed buffer is the exact default state).
    for (uint32_t i = 0; i < kFormantDelayMaxSmp; ++i)
    {
      formant_comb_buf1_[i] = 0.f;
      formant_comb_buf2_[i] = 0.f;
    }
    formant_comb_w1_ = 0U;
    formant_comb_w2_ = 0U;
    // Envelope follower / sidechain ducking: zero the follower and precompute
    // the one-pole follower coefficient (1 - exp(-1/tau) at 48 kHz). The duck
    // gain itself is local to process() (duck_gain, defaults to 1.0 when DCK
    // == 0, so the follower never advances and the main voice is untouched --
    // regression-safe).
    env_follower_ = 0.f;
    duck_alpha_ = one_minus_expf(-1.f / (kDuckFollowTauSec * 48000.f)); // ~0.013
    // Chord interval fades (INT / INT2 on-off): start SILENT -- the interval
    // level only ramps up from 0 over 2 s once its cents value is > 0. This
    // makes the first note-on fade the chord in (no instant "start
    // screaming") and keeps init() clean. Precompute the one-pole alpha for
    // the 2 s fade (same curve as the note attack/release: 1 - exp(-5 / N),
    // one_minus_expf, never libm).
    chord1_fade_ = 0.f;
    chord2_fade_ = 0.f;
    chord_fade_alpha_ = one_minus_expf(-5.f / static_cast<float>(kChordFadeSamples));
  }

inline void Osc::reset() 
  {
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    chord_.reset();
    chord2_.reset();
    phasor_sub_ = 0.f;
    phasor_sub2_ = 0.f;
    sub2_.lpf_state = 0.f;
    sub2_.gate = 0.f;
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // re-center every stream on stop
    svf_.reset(); // zero the SVF integrator states (keeps the knob-derived coefficients)
    prev_wav_a_ = 0.f; // FM lookback cache
    prev_wav_b_ = 0.f;
    fm_smooth_a_ = 0.f; // phase-FM index LPF state
    fm_smooth_b_ = 0.f;
    // Stop the note and kill the A-H-R envelope (allNoteOff semantics).
    note_on_ = false;
    last_note_ = 0;
    glide_active_ = false;
    eg_.reset();
    // Silence the chord interval fades on stop (allNoteOff semantics: the
    // chord goes quiet instantly, no 2 s tail). The next note-on ramps them
    // back in from 0 over 2 s.
    chord1_fade_ = 0.f;
    chord2_fade_ = 0.f;
  }
  // ---- note handling ---------------------------------------------------------

  // Key press. Legato is ALWAYS ON: the drone is held until explicitly stopped.
  //  * If a note is ALREADY sounding: this only re-targets the pitch. The
  //    envelope is deliberately NOT re-triggered, so the amplitude stays
  //    continuous (no drop to zero / re-attack) while setPitch() glides the
  //    whole sound from the old note to this one over 2 s.
  //  * If nothing is sounding: start the A-H-R attack (2 s ramp-up).
  //  * The SAME key is pressed while sounding -> TOGGLE-OFF: the sound fades
  //    out to zero (re-triggering the same key stops it).
inline void Osc::noteOn(uint8_t note, uint8_t velo) 
  {
    (void)velo;
    // Legato: the drone is held until explicitly stopped.
    //  * same key as the one currently sounding -> TOGGLE-OFF (fade out).
    //  * different key (or nothing sounding)    -> the new note becomes the
    //    sounding one: start the attack if we were silent, otherwise only
    //    retarget the pitch (setPitch glides, amplitude stays continuous).
    if (note_on_ && note == last_note_)
    {
      note_on_ = false;
      last_note_ = 0;
      eg_.noteOff();
      return;
    }
    const bool was_sounding = note_on_;
    last_note_ = note;
    note_on_ = true;
    if (!was_sounding)
      eg_.noteOn(); // genuine new drone: 2 s attack
    // else: keep the envelope (continuous amplitude); only the pitch glides.
  }

  // Key release. Legato is ALWAYS ON, so releasing a key is a NO-OP: it does
  // not stop the drone. It also must NOT clear the "sounding note" tracking,
  // otherwise a click (which is a noteOn immediately followed by a noteOff)
  // would clobber last_note_ and the re-press toggle-off in noteOn could never
  // match. Only an explicit re-press of the sounding note (noteOn) or
  // All-Notes-Off stops a drone.
inline void Osc::noteOff(uint8_t note) 
  {
    (void)note;
  }

  // All-notes-off: note inactive, kill any in-flight envelope immediately
  // (instant silence — no release tail). This is the global stop: it silences
  // the held (legato) drone too, which per-key release does not.
inline void Osc::allNoteOff() 
  {
    note_on_ = false;
    last_note_ = 0;
    eg_.allOff();
  }

  // audio processing callbacks

  // set frequency in digital w (w = f/samplerate, 0.5 is Nyquist).
  // When the target differs from the current base pitch and a glide is not
  // already running, launch a 2 s LINEAR glide from the current w0_ to the new
  // one (the audio path advances it sample by sample and recomputes every
  // derived voice). A glide already in progress is re-targeted: it slides from
  // where it is now to the newest note, so a fast run of keys always ends on
  // the last one. With no glide active and the same pitch this is the cheap
  // direct path (no state change).
inline void Osc::setPitch(float w0)
  {
    // Nothing is sounding (the previous note faded out / was toggled off /
    // all-notes-off): there is no "old note" to glide from. Kill any in-flight
    // glide and jump straight to the new pitch so the NEXT note-on starts clean
    // at its own pitch, not by sliding up from the (now silent) old one.
    if (!note_on_)
    {
      glide_active_ = false;
      glide_elapsed_ = 0;
      w0_ = w0;
      recalc_pitch_derived();
      return;
    }
    if (!glide_active_ && w0 != w0_)
    {
      glide_active_ = true;
      glide_elapsed_ = 0;
      glide_from_ = w0_;
      glide_to_ = w0;
      // w0_ itself is advanced by the audio path (osc_mix.h) from here to the
      // target; nothing to do in the K-rate path besides arming the glide.
      return;
    }
    if (glide_active_)
    {
      // Re-target an in-flight glide: slide from the CURRENT w0_ to the newest
      // note over a fresh full kGlideSamples, and restart the clock. Holding a
      // new key while gliding is the intended legato action, so the move to the
      // latest key always gets the full glide.
      glide_from_ = w0_;
      glide_to_ = w0;
      glide_elapsed_ = 0;
      return;
    }
    // Same pitch (or already at target with no glide): nothing to do. The
    // derived voices already match w0_.
  }

