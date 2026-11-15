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
    lfo_ = 0.f;
    // ---- Polyphonic voice cluster: zero every voice --------------------------
    // Each cluster voice is independent state (phasors, sub phasors, sub-2
    // state, A-H-R, glide, w0). All start silent (w0 = 0, idle envelope) so
    // an all-off cluster produces exactly zero (regression: 0 notes = silent).
    for (uint8_t vi = 0; vi < kVoiceCount; vi++)
    {
      Voice &v = voice_[vi];
      v.w0 = 0.f;
      v.glide_from = 0.f;
      v.glide_to = 0.f;
      v.glide_dist = 0;
      v.glide_active = false;
      v.ph_a = 0.f;
      v.ph_b = 0.f;
      v.ph_sub = 0.f;
      v.ph_sub2 = 0.f;
      v.prev_wav_a = 0.f; // FM lookback cache: start at 0 (first sample is a no-op)
      v.prev_wav_b = 0.f;
      v.fm_smooth_a = 0.f; // phase-FM index LPF state (no FM on the first sample)
      v.fm_smooth_b = 0.f;
      v.sub2.lpf_state = 0.f;
      v.sub2.gate = 0.f;
      v.note = 0;
      v.active = false;
      v.gen = 0;
      v.eg.reset();
    }
    // Cluster glide bookkeeping: no pending distance, no last note yet.
    gl_k_ = 0U;
    gl_want_ = 0;
    last_note_ = -1;
    // Parameter selector: start with RATE (select_ = 0) selected (RATE leads
    // the multiplexer). Re-apply the cached default values so the DSP and the
    // PARAM SEL / PARAM VAL display agree on startup (the header's "init"
    // value of PARAM_VAL already matches edit_value_[0], but this keeps the
    // two in sync if either side is edited independently later). This also
    // recomputes det_f_a_/det_f_b_ (DETUNE) and gl_k_ (GLIDE).
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
      v.glide_active = false;
      v.glide_dist = 0;
      v.active = false;
      v.eg.reset();
    }
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // re-center every stream on stop
    svf_.reset(); // zero the SVF integrator states (keeps the knob-derived coefficients)
    last_note_ = -1;
    gl_want_ = 0;
    env_sum_ = 0.f; // cluster is silent -> no pending loudness compensation
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


  // Start (or re-target) one cluster voice on a note, arming its GRIMM-style
  // exponential GLIDE from the cluster's LAST note (gl_want_ is the pending
  // distance, written by noteOn). A new note always gets its own voice + a
  // fresh 2 s A-H-R attack (the cluster is polyphonic: notes sound together).
  inline void Osc::voiceStart(Voice &v, uint8_t note, float w0)
  {
    v.note = note;
    v.active = true;
    v.gen = ++gen_counter_; // monotonically increasing (steal ordering)
    v.w0 = w0;
    v.glide_to = w0;
    v.glide_from = w0;
    // Glide from the last note (gl_want_): 0 = jump straight to w0.
    v.glide_dist = gl_want_;
    v.glide_active = (gl_want_ != 0) && (gl_k_ > 0U);
    v.ph_a = 0.f;
    v.ph_b = 0.f;
    v.ph_sub = 0.f;
    v.ph_sub2 = 0.f;
    v.prev_wav_a = 0.f;
    v.prev_wav_b = 0.f;
    v.fm_smooth_a = 0.f;
    v.fm_smooth_b = 0.f;
    v.sub2.lpf_state = 0.f;
    v.sub2.gate = 0.f;
    v.eg.noteOn(); // 2 s A-H-R attack
  }

  // Key press (LATCH is ALWAYS ON). Each new note gets its OWN voice in the
  // kVoiceCount cluster, so up to kVoiceCount notes sound at the same time.
  //  * Re-pressing the SAME note -> TOGGLE-OFF that voice (fade out).
  //  * A new note while all kVoiceCount voices are busy -> STEAL the OLDEST
  //    active voice (lowest gen) and start this note on it.
  // The new note GLIDES from the cluster's LAST note (GRIMM exponential):
  // gl_want_ = (last_note - new_note) * 256 * 4 (1/16 semitone * 256 units).
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
    // Compute the glide distance from the cluster's LAST note (GRIMM vc_gl
    // semantics: positive = new note is HIGHER than the last, the voice glides
    // UP; negative = glides down). last_note_ < 0 (first note) -> no glide.
    // Scale: 1 semitone = 4096/12 units (1/12 octave per semitone, 4096
    // units per octave). The audio path (osc_mix.h) computes:
    //   current_w0 = glide_to * 2^(-glide_dist / 4096)
    // so glide_dist = 4096 * (octaves from target) and the sign is:
    //   glide_dist > 0 -> current < target (still gliding UP from below)
    //   glide_dist < 0 -> current > target (still gliding DOWN from above)
    // At glide_dist = 0 -> current = glide_to (arrived).
    int32_t gl_want = 0;
    if (last_note_ >= 0)
      gl_want = ((int32_t)note - last_note_) * 4096 / 12; // 1/12 octave per semitone
    gl_want_ = gl_want;
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
      voice_[vi].glide_active = false;
      voice_[vi].glide_dist = 0;
      voice_[vi].eg.allOff();
    }
    last_note_ = -1;
    gl_want_ = 0;
  }

  // audio processing callbacks

  // setPitch: LEGACY single-note entry point. In the polyphonic build the
  // cluster's pitch is driven entirely by noteOn() (which arms each voice's
  // per-voice GLIDE), so setPitch does nothing. It is kept (and still wired in
  // unit.cc / wasm.cc) for API compatibility and the hardware's per-block
  // pitch callback; the hardware's note MIDI events (noteOn) are what drive the
  // cluster. (The old 2 s linear single-voice glide is gone.)
inline void Osc::setPitch(float w0)
  {
    (void)w0; // no-op: polyphonic cluster, pitch driven by noteOn()
  }

