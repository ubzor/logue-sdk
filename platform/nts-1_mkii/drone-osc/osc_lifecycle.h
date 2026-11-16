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
 *  re-pressed or all-notes-off), and setPitch() (legacy no-op in the
 *  polyphonic build; the cluster's pitch is driven by noteOn()).
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
    lfo_ = 0.f;
    // ---- Polyphonic voice cluster: zero every voice --------------------------
    // Each cluster voice is independent state (phasors, sub phasors, sub-2
    // state, A-H-R, w0). All start silent (w0 = 0, idle envelope) so
    // an all-off cluster produces exactly zero (regression: 0 notes = silent).
    for (uint8_t vi = 0; vi < kVoiceCount; vi++)
    {
      Voice &v = voice_[vi];
      v.w0 = 0.f;
      v.ph_a = 0.f;
      v.ph_b = 0.f;
      v.ph_sub = 0.f;
      v.ph_sub2 = 0.f;
      v.prev_wav_a = 0.f; // FM lookback cache: start at 0 (first sample is a no-op)
      v.prev_wav_b = 0.f;
      v.fm_smooth_a = 0.f; // phase-FM index LPF state (no FM on the first sample)
      v.fm_smooth_b = 0.f;
      v.breath_dev = 1.f;
      v.breath_target = 1.f;
      v.sub2.lpf_state = 0.f;
      v.sub2.gate = 0.f;
      v.note = 0;
      v.active = false;
      v.gen = 0;
      v.eg.reset();
    }
    // (glide removed: no gl_k_ / gl_want_); no last note yet.
    last_note_ = -1;
    // Parameter selector: start with RATE (select_ = 0) selected (RATE leads
    // the multiplexer). Re-apply the cached default values so the DSP and the
    // PARAM SEL / PARAM VAL display agree on startup (the header's "init"
    // value of PARAM_VAL already matches edit_value_[0], but this keeps the
    // two in sync if either side is edited independently later). This also
    // recomputes det_f_a_/det_f_b_ (DETUNE).
    select_ = 0;
    for (uint8_t i = 0; i < kEditParamCount; i++)
      apply_edit_param(kEditParamFirst + i, edit_value_[i]);
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // each stream starts centered, AMT=0 (off)
    svf_.reset(); // zero the SVF integrator states (coefficients set by apply_edit_param above)
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
    // is now CONSTANT (kDuckConstant) -- the follower advances every sample in
    // process() and duck_gain settles to 1 - kDuckConstant * env_follower.
    // Starting the follower at 0 makes the very first notes un-ducked; the
    // follower then rises to the steady-state low-end level (~0.5) over a few
    // tau (~75 ms) as the subs sound.
    env_follower_ = 0.f;
    duck_alpha_ = one_minus_expf(-1.f / (kDuckFollowTauSec * 48000.f)); // ~0.013
    // Polyphonic-cluster loudness compensation (see env_sum_ in osc.h and its
    // use in process(): 1/sqrt(env_sum) on the summed voice signals). The
    // per-voice A-H-R envelope is applied per-voice in the accumulation loop
    // (each voice gated by its own v.eg.env); env_sum_ only feeds the
    // loudness-compensation one-pole (env_comp_). Nothing sounding yet, so the
    // smoothed 1/sqrt(N) starts at 1 (a single note / silence -> no attenuation).
    env_sum_ = 0.f;
    env_comp_ = 1.f;
    // (chord_fade_alpha_ / chord_new_ / chord2_new_ / prev_chord_cents_ /
    //  prev_chord2_cents_ were removed — the chord/interval concept is gone.)
  }

inline void Osc::reset() 
  {
    // Stop the whole cluster and kill every voice's A-H-R envelope
    // (allNoteOff semantics: instant silence, no release tail).
    for (uint8_t vi = 0; vi < kVoiceCount; vi++)
    {
      Voice &v = voice_[vi];
      v.ph_a = 0.f;
      v.ph_b = 0.f;
      v.ph_sub = 0.f;
      v.ph_sub2 = 0.f;
      v.sub2.lpf_state = 0.f;
      v.sub2.gate = 0.f;
      v.breath_dev = 1.f;
      v.breath_target = 1.f;
      v.active = false;
      v.eg.reset();
    }
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // re-center every stream on stop
    svf_.reset(); // zero the SVF integrator states (keeps the knob-derived coefficients)
    last_note_ = -1;
    // (glide removed: no gl_want_)
    env_sum_ = 0.f; // cluster is silent -> no pending loudness compensation
    last_breath_redraw_ = 0; // reset the breath tick cache
    breath_focus_ = 0;       // reset the rotating focus to voice 0
  }
  // ---- note handling ---------------------------------------------------------

  // MIDI note -> base phase increment (w = f/sr). Used by noteOn (the
  // polyphonic cluster's pitch driver). It reuses the SDK's authoritative
  // `midi_to_hz_lut_f` (Hz, 152 entries, extern in common/osc_api.h -- the
  // SAME table the platform's osc_w0f_for_note / the web sim use), scaled by
  // 1/48000 to a phase increment. This is numerically identical to
  // osc_w0f_for_note(note, 0) WITHOUT pulling in that function's mod/clip
  // logic, and works in both the ARM build (linked firmware LUT) and the web
  // sim (where drone-chain links websim/dsp/midi_to_hz_lut.c). Note: this
  // header must be included AFTER unit_osc.h (which brings in osc_api.h and
  // declares midi_to_hz_lut_f) -- osc.h satisfies that (it includes
  // unit_osc.h at the top).
  inline void Osc::noteW0(uint8_t note, float *out_w)
  {
    const uint32_t n = (note > 151U) ? 151U : (uint32_t)note;
    *out_w = midi_to_hz_lut_f[n] * (1.f / 48000.f);
  }


  // Start (or re-target) one cluster voice on a note. A new note always gets
  // its own voice + a fresh 2 s A-H-R attack (the cluster is polyphonic: notes
  // sound together). (No glide: the note starts at its exact pitch with a
  // random per-voice start phase.)
  inline void Osc::voiceStart(Voice &v, uint8_t note, float w0)
  {
    v.note = note;
    v.active = true;
    v.gen = ++gen_counter_; // monotonically increasing (steal ordering)
    v.w0 = w0;
    // (glide removed: no glide_to / glide_from / glide_dist / glide_active)
    // ---- Polyphonic-cluster DISCORD-avoidance (random phases + detune jitter) ----
    // (1) RANDOM PHASES (GRIMM analog: va_begin sets ph[1]=ph[0]+0x40000000, and
    //     SUPERSAW's saws start at random phases via xorshift32). Starting every
    //     voice at ph=0 makes all voices' A/B phasors PHASE-LOCKED, so their
    //     DETUNE beats (A−B, ~0.8-1 Hz per voice) add COHERENTLY when 2+ notes
    //     are held -> a harsh low-frequency "howl" (~16-19 Hz = A2−B1 / B2−A1).
    //     Random per-voice phases de-correlate the beats (they still exist per
    //     voice, but no longer sum coherently across voices).
    // (2) PER-VOICE DETUNE JITTER (GRIMM analog: SUPERSAW spreads 7 saws by
    //     DETUNE; the spread smears the beat spectrum). A fixed random ±2-cent
    //     offset (drawn once, stored in v.det_jitter_cents / _factor) shifts BOTH
    //     A and B of this voice in OPPOSITE directions (GRIMM-style asymmetry),
    //     so its A−B SPREAD = DETU + walk + 2*jitter (asymmetric, per-voice).
    //     This de-correlates the inter-voice beat spectra MUCH more strongly
    //     than a common offset: with 2-3 notes the A2−B1 / B2−A1 differences
    //     (the ~16-19 Hz "howl") are now spread by up to 8 cents per voice-pair
    //     (~2.9 Hz at 440 Hz), so the howl is SMOOTHED across a band. The mono
    //     timbre (1 note) is unchanged (jitter=0..4c on A/B is inaudible; the
    //     A−B spread stays within DETU+walk+8c, which reads as a slightly
    //     thicker drone).
    v.ph_a = osc_white(); // random [0,1)
    v.ph_b = v.ph_a + 0.25f; // +1/4 cycle (GRIMM's ph[1]=ph[0]+0x40000000)
    if (v.ph_b >= 1.f) v.ph_b -= 1.f; // wrap
    v.ph_sub = osc_white();
    v.ph_sub2 = osc_white();
    v.prev_wav_a = 0.f;
    v.prev_wav_b = 0.f;
    v.fm_smooth_a = 0.f;
    v.fm_smooth_b = 0.f;
    v.breath_dev = 1.f;     // per-voice breath: start at neutral level (1.0)
    v.breath_target = 1.f;  // will be set by the rotating-focus on the first tick
    v.sub2.lpf_state = 0.f;
    v.sub2.gate = 0.f;
    v.eg.noteOn(); // 2 s A-H-R attack
  }

  // Key press (LATCH is ALWAYS ON). Each new note gets its OWN voice in the
  // kVoiceCount cluster, so up to kVoiceCount notes sound at the same time.
  //  * Re-pressing the SAME note -> TOGGLE-OFF that voice (fade out).
  //  * A new note while all kVoiceCount voices are busy -> STEAL the OLDEST
  //    active voice (lowest gen) and start this note on it.
  // (No glide: the new note starts at its exact pitch with a random
  //  per-voice start phase; last_note_ is still tracked for bookkeeping.)
inline void Osc::noteOn(uint8_t note, uint8_t velo) 
  {
    (void)velo;
    // Toggle-off: re-pressing a note that is already sounding on a voice stops
    // that voice (the only per-note "off" — LATCH keeps all other notes).
    for (uint8_t vi = 0; vi < kVoiceCount; vi++)
    {
      Voice &v = voice_[vi];
      if (v.active && v.note == note)
      {
        v.active = false;
        v.eg.noteOff(); // 2 s release (the note fades out, others stay latched)
        return;
      }
    }
    // (glide removed: no gl_want_ / glide-distance computation)
    last_note_ = (int32_t)note; // this note becomes the cluster's last
    // Find a free voice, or steal the oldest active one.
    uint8_t target = kVoiceCount;
    for (uint8_t vi = 0; vi < kVoiceCount; vi++)
      if (!voice_[vi].active) { target = vi; break; }
    if (target == kVoiceCount)
    {
      // All busy: steal the oldest (lowest gen) active voice.
      uint8_t oldest = 0;
      for (uint8_t vi = 1; vi < kVoiceCount; vi++)
        if (voice_[vi].gen < voice_[oldest].gen) oldest = vi;
      target = oldest;
      voice_[target].active = false;
      voice_[target].eg.noteOff(); // release the stolen note (fades under the new one)
    }
    float w0;
    noteW0(note, &w0);
    voiceStart(voice_[target], note, w0);
  }

  // Key release. LATCH is ALWAYS ON, so releasing a key is a NO-OP: the note
  // is held (latched). Only re-pressing the SAME note (noteOn) or
  // All-Notes-Off stops a note.
inline void Osc::noteOff(uint8_t note) 
  {
    (void)note;
  }

  // All-notes-off: stop the whole cluster, kill every voice's envelope
  // immediately (instant silence — no release tail).
inline void Osc::allNoteOff() 
  {
    for (uint8_t vi = 0; vi < kVoiceCount; vi++)
    {
      voice_[vi].active = false;
      voice_[vi].eg.allOff();
    }
    last_note_ = -1;
    // (glide removed: no gl_want_ cleanup)
  }

  // audio processing callbacks

  // setPitch: LEGACY single-note entry point. In the polyphonic build the
  // cluster's pitch is driven entirely by noteOn(), so setPitch does nothing.
  // It is kept (and still wired in unit.cc / wasm.cc) for API compatibility
  // and the hardware's per-block pitch callback; the hardware's note MIDI
  // events (noteOn) are what drive the cluster.
inline void Osc::setPitch(float w0)
  {
    (void)w0; // no-op: polyphonic cluster, pitch driven by noteOn()
  }

