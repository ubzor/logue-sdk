#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_mix.h
 *
 *  Out-of-line definition of Osc::process() -- the per-sample audio path:
 *  phasor advance (with cross-FM), wavetable resolve, the A+B folder /
 *  saturator / SVF chain, the chord's own folder / saturator, the sub and
 *  sub-2 voices, the final 4-branch mix + hard clip, and the A-H-R envelope.
 *
 *  This was the largest block of osc.h (a ~475-line audio loop). It now lives
 *  here so the class body stays small. It is #included at the BOTTOM of osc.h,
 *  after the Osc class is complete, so it can reference every data member and
 *  the osc_consts::* tuning constants.
 *
 *  IMPORTANT: this file must contain ZERO per-sample branching that depends on
 *  audio data (the only branches are the fixed hard-clip guards and the
 *  once-per-block caches), keeping the DSP deterministic and low-latency.
 *
 */

#include "osc.h"  // Osc class (complete definition) + osc_consts::* + helpers

inline void Osc::process(const float *__restrict in, float *__restrict out, uint32_t frames)
{
  (void)in; // drone generator does not use the audio input

  // ---- K-rate prepass (see blockPrepass) -------------------------------------
  // Consume the (sim-only) Rnd re-roll request, advance the Rnd debounce
  // clock (rndTick) and the Rnd re-roll morph (rndMorphTick). Everything here
  // runs at K-rate (once per block, before the per-sample loop) and is
  // extracted from process() so that this file stays a pure per-sample audio
  // path (see the file header comment).
  blockPrepass(frames);
  // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
  const Params p = params_;

  // ---- Per-voice base w0 (the raw note-rate phasor advance) --------------------
  // In the polyphonic cluster each voice has its OWN w0 (voice_[vi].w0), set
  // once per note (no glide). There is no single shared w0. The static A/B
  // detune is the SHARED factor pair det_f_a_ / det_f_b_ (K-rate,
  // apply_edit_param(DETUNE)) multiplied by each
  // voice's w0 in the loop; the random-walk detune (rw_dev_a/b_cents) is also
  // shared and added per-voice.
  //
  // (The A-H-R relaxation coefficients are cached once per block for cheap
  //  access by the per-voice envelope advance below.)
  const float alpha_a = eg_alpha_a_;
  const float alpha_r = eg_alpha_r_;
  // One-pole relax coefficients (per sample) for the SHARED sub bass.
  // K-rate precompute: the per-sample one_minus_expf would be a libm call in
  // the audio path, so both are cached here (see one_minus_expf in osc.h).
  // kSubAnchorAlpha (osc_constants.h) = 1 - exp(-1/tau), tau = 250 ms at
  // 48 kHz: the anchor follows the env-weighted mean w0 of the sounding
  // voices, so a note on/off MORPHS the sub pitch smoothly (no click).
  // kSubBreathAlpha = 1 - exp(-1/tau), tau = kSubBreathTauSec (~0.75 s):
  // the SUBB breath-gain follower (see the "Shared sub bass" block below the
  // per-voice loop).
  const float sub_anchor_alpha = kSubAnchorAlpha;
  const float sub_breath_alpha = one_minus_expf(-1.f / (kSubBreathTauSec * 48000.f));

  const bool rw_active = p.rw_amount > 0.f;

  // Wavetable: the WAVE knob selects a BANK (0..5, A..F). The LFO drives the
  // intra-bank morph position centered on the bank (see WaveTable::resolve_wave
  // / bank_center): at LFO=0 the position is the bank center, and at LFO=+/-100%
  // it sweeps from the first to the last table of the bank, covering every wave.
  // Both A/B oscillators read the SAME wave (the intra-bank LFO morph); the
  // A/B DETUNE (det_f_a_/det_f_b_) provides the width, the LFO provides the
  // timbre movement. The bank boundary is hard: the morph never crosses into
  // the next bank.
  uint32_t wave_bank = (uint32_t)p.wave_pos;
  if (wave_bank > 5U) wave_bank = 5U;
  if (rw_active)
  {
    // Recompute each stream's interval / tracking from its own SPDx/SMTx
    // (setup() is cheap and caches; the shared rw_amount ceiling is passed
    // but only used for max_dev, which does not affect the period/tracking).
    // The first three streams are addressed by p.rw_rate[i] / p.rw_smooth[i]
    // (SPD1..3 / SMT1..3, 3-element arrays).
    for (uint8_t i = 0; i < 3; i++)
      rw_[i].setup(p.rw_rate[i], p.rw_amount, p.rw_smooth[i]);
    // Initialize each stream's targets to its current deviations on first
    // activation (avoids a visible jump when the RATE knob crosses 0).
    for (uint8_t i = 0; i < 3; i++)
    {
      if (rw_[i].target_a == 0.f && rw_[i].dev_a != 0.f)
        rw_[i].target_a = rw_[i].dev_a;
      if (rw_[i].target_b == 0.f && rw_[i].dev_b != 0.f)
        rw_[i].target_b = rw_[i].dev_b;
    }
  }
  // Breath stream (rw_[3]): setup() is called UNCONDITIONALLY (not gated by
  // rw_active / RATE) so that breathing works independently of the RATE knob.
  // The breath stream has its OWN knobs (RW_LSP / RW_LSM -> p.rw_lsp / p.rw_lsm)
  // and its own depth (RW_LVL -> p.rw_lvl); it does NOT use the shared RATE
  // ceiling. The rw_amount parameter is passed for API compatibility (used
  // only for max_dev, which does not affect period/tracking).
  rw_[3].setup(p.rw_lsp, p.rw_amount, p.rw_lsm);
  // Initialize the breath stream's targets on first activation.
  if (rw_[3].target_a == 0.f && rw_[3].dev_a != 0.f)
    rw_[3].target_a = rw_[3].dev_a;
  if (rw_[3].target_b == 0.f && rw_[3].dev_b != 0.f)
    rw_[3].target_b = rw_[3].dev_b;

  // Random-walk scaled values (computed per-sample inside the loop below).
  // Each stream's dev_a/b (rw_[i].dev_a/b) are normalized to [-1, +1] and
  // advance per-sample via rw_[i].advance(). Each stream scales its own
  // deviations to its targets' units at the point of use:
  //   Tone (rw_[0])    -> detune (cents) + cross-FM
  //   Timbre (rw_[1])  -> wavetable morph (tables) + SVF cutoff
  //   Density (rw_[2]) -> drive + wavefolder depth
  // The scaled values must be recomputed every sample (not cached per-block)
  // because rw_[i].advance() moves dev_a/b on every sample.
  // (RW_WAV is now handled by the LFO intra-bank sweep — see lfo_morph below.)
  const bool rw_drv_active = rw_active && p.rw_drv > 0.f;
  const bool rw_fld_active = rw_active && p.rw_fld > 0.f;
  // ---- Phase-FM (FM1/FM2 = STATIC index + RFM walk): per-block cache ----
  // FM_A / FM_B are the STATIC FM index (peak phase deviation, cycles, after
  // the 0.5 normalization): the knob value (already 0..kFmMaxCents) times 0.5.
  // The static index is NOT gated by RATE — turning FM on gives constant
  // sidebands immediately. RFM_A / RFM_B add a random-walk
  // "breathing" on top: the walk depth (dev * rw_amt * rfm) is gated by RATE,
  // like every other walk. Precomputed once per block (knobs are L-rate); the
  // per-sample raw index (prev_wav * 0.5 * eff_index) is in the loop below.
  // At fm=0 & rfm=0 the FM is fully off (regression-safe).
  //
  // The knob stores the RAW normalized depth (p.fm_a/p.fm_b are 0..1, see
  // apply_edit_param) and the audio path maps it through the kFmCurveExp
  // (quadratic) depth curve -> 0..kFmMaxCents, then the 0.5 normalization to
  // the peak index in cycles. The low end of the knob is a near-silent
  // "breathing" (curve ~ x^2 is tiny) and the harsh sidebands only open up
  // near the top, so the FM is a smooth, usable timbre knob instead of
  // "slight turn = screech".
  // kFmCurveExp is 2.0f (see osc_constants.h), so the quadratic depth curve is
  // just x^2. We do NOT call libm powf(): it is emitted as a dynamic UND
  // symbol that the NTS-1 mkII runtime cannot resolve at load.
  const float fm_a_curve  = (p.fm_a * p.fm_a) * kFmMaxCents; // 0..kFmMaxCents
  const float fm_b_curve  = (p.fm_b * p.fm_b) * kFmMaxCents;
  const float fm_a_static = fm_a_curve * 0.5f;  // static peak index (cycles)
  const float fm_b_static = fm_b_curve * 0.5f;  // static peak index (cycles)
  const bool fm_a_wander = rw_active && p.rfm_a > 0.f;  // walk on FM A (RATE-gated)
  const bool fm_b_wander = rw_active && p.rfm_b > 0.f;  // walk on FM B (RATE-gated)
  const float fm_a_wander_depth = p.rfm_a * kFmMaxCents * 0.5f;  // walk peak (cycles)
  const float fm_b_wander_depth = p.rfm_b * kFmMaxCents * 0.5f;  // walk peak (cycles)
  // ---- JIT: per-block cache (the per-sample white() draws are in the loop) ----
  // The knob is L-rate; cache the > 0 gate so the loop's branch is a simple
  // const test. At 0 (default) the jitter is off and the phasor update is
  // identical to the stock sound.
  const bool jit_active = p.jit_cycles > 0.f;
  // ---- RMOD: per-block cache (the per-sample wav_a*wav_b is in the loop) ----
  // The knob is L-rate; cache the > 0 gate so the loop's ring-mod branch is a
  // simple const test. At 0 (default) the ring-mod is off.
  const bool rmod_active = p.ring_mix > 0.f;
  // SVF: the internal 12 dB/oct low-pass is engaged when the dry/wet (svf_wet)
  // is non-zero. When svf_wet == 0 (the default) the filter is transparent
  // and its state update is skipped (regression-safe). The walk-modified
  // cutoff coefficient (svf_.cc_w) is set per-sample below when active.
  const float svf_wet = p.svf_wet;
  const bool svf_active = svf_wet > 0.f;

  for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
  {
    // ================================================================
    // POLYPHONIC CLUSTER: kVoiceCount notes, identical settings, shared
    // post-stage. Per sample we (1) advance each voice's phasors (with the
    // SHARED random-walk detune/morph/FM/jitter), (2) sum its A/B wavetable
    // outputs and ring-mod into shared accumulators, (3) compute the ONE
    // shared sub bass (the cluster's env-weighted ANCHOR note — see the
    // "Shared sub bass" block below the per-voice loop), then (4) run the
    // SINGLE common post-stage (folder -> saturator -> SVF -> formant -> comb
    // -> duck -> makeup + soft-clip) on the sums. The A-H-R envelope is
    // per-voice (each note fades in/out on its own); the duck follower and the
    // sub bass are shared (one drone's low end).

    // ---- Shared random walk (advance ONCE per sample, shared by all voices)
    // The RATE AMT knob is the DEPTH CEILING of the whole walk: it scales
    // every random-walk modulation (detune below; wavetable morph / drive /
    // fold further down). The normalized deviations (rw_.dev_a/b, in [-1,+1])
    // are multiplied by p.rw_amount (0..1, quadratic-mapped from the knob)
    // at each point of use, so RATE = 0 means no wandering at all.
    const float rw_amt = p.rw_amount;
    float rw_dev_a_cents = 0.f;
    float rw_dev_b_cents = 0.f;


    if (rw_active)
    {
      // Advance all three streams (each draws its own random targets on its
      // own period, so they are not phase-locked). The Tone stream (rw_[0])
      // drives detune + cross-FM; Timbre (rw_[1]) drives wavetable morph +
      // SVF cutoff; Density (rw_[2]) drives drive + wavefolder.
      rw_[0].advance(p.rw_amount, osc_white);
      rw_[1].advance(p.rw_amount, osc_white);
      rw_[2].advance(p.rw_amount, osc_white);
      // Per-sample scaled Tone-stream walk values (dev_a/b change every sample).
      rw_dev_a_cents = rw_[0].dev_a * rw_amt * kRwDetuneScaleCents;
      rw_dev_b_cents = rw_[0].dev_b * rw_amt * kRwDetuneScaleCents;
      // (w0_a / w0_b are now computed PER-VOICE in the loop below from each
      //  voice's own w0 + the shared det_f_a_/det_f_b_ factors + these walks.)
    }
    // Timbre stream (rw_[1]): the wavetable-morph target (RW_WAV) is now
    // handled by the LFO (intra-bank sweep). The rw_[1] stream still drives
    // the SVF cutoff (RWFC) — see the FCUT modulation below. The old
    // rw_dev_a_tables / rw_dev_b_tables are removed.
    // Density stream (rw_[2]): saturator drive + wavefolder depth.
    const float rw_dev_drive    = rw_drv_active ? rw_[2].dev_a * rw_amt * kRwDriveScale : 0.f;
    // Multiplicative: eff_fold = p.fold_depth * (1 + dev_b * p.rw_fld * rw_amt).
    // p.rw_fld is 0..1 (RFLD knob). At RFLD=0 -> eff_fold = p.fold_depth (no mod).
    // At RFLD=100% & RATE=100% -> eff_fold wanders from 0 to 2*p.fold_depth
    // (clamped to 1). The rw_amt factor keeps the folder walk gated by the
    // global RATE AMT depth, consistent with the detune/morph/drive walks.
    // When p.fold_depth = 0, eff_fold = 0 regardless (no fold).
    const float rw_fold_mod = rw_fld_active ? rw_[2].dev_b * p.rw_fld * rw_amt : 0.f;

    // ---- Breath stream (rw_[3]): ROTATING FOCUS ----------------------------
    // The fourth stream drives the MAIN voice level of EACH cluster note,
    // so the notes "breathe" in and out of prominence relative to each
    // other — the heart of a drone (a cluster of notes trading the floor).
    //
    // INDEPENDENT of RATE: breathing is gated ONLY by RW_LVL (Breath Depth).
    // Turning up Breath Depth with RATE=0 still breathes (the other three
    // wandering streams are still RATE-gated, as before).
    //
    // MECHANISM — "Rotating Focus" (mechanical, predictable, sum-preserving):
    //   On each tick (rw_[3] target-redraw event, period = RW_LSP), the
    //   "focus" (the highlighted note) advances to the next active voice
    //   in round-robin order. The focus voice's level increases by
    //   rw_lvl (Breath Depth); the other voices' levels decrease by
    //   rw_lvl * focus / (n_active - 1), so the SUM of all levels stays
    //   constant (the overall loudness does not pump). The transition is
    //   SMOOTH: each voice's breath_dev tracks its target with the RW_LSM
    //   smoothing coefficient (exponential, non-linear).
    //
    //   Per-voice level factors:
    //     focus voice:   1 + rw_lvl
    //     other voices:  1 - rw_lvl * focus / (n_active - 1)
    //   where focus = 1.0 for the highlighted note, 0.0 for the rest.
    //   At RW_LVL=0 all factors are 1.0 (regression-safe).
    //   At RW_LVL=100% & n=4: focus = 1 + 1.0 = 2.0 (clamped to 1.0),
    //   others = 1 - 1.0 * 1.0 / 3 = 0.67 (clearly attenuated).
    //   At RW_LVL=50% & n=4: focus = 1.5, others = 0.83 (subtle).
    //
    // Only the MAIN (A+B) voice is breathed. SUB1 (f/2) and SUB2 (f/4) are
    // intentionally NOT breathed — the low end stays static so the drone's
    // anchor never "flutters".
    const bool breath_active = p.rw_lvl > 0.f;
    if (breath_active)
    {
      // Advance the Breath stream's clock (amount=1.0 bypasses the RATE gate;
      // the period / smooth are set by RW_LSP / RW_LSM via setup()).
      rw_[3].advance(1.f, osc_white);
      const float k = rw_[3].getK();
      const uint32_t rc = rw_[3].getRedrawCount();
      const bool tick = (rc != last_breath_redraw_);
      last_breath_redraw_ = rc;

      // Count active voices and advance the focus on a tick.
      uint8_t n_active = 0;
      for (uint8_t vi = 0; vi < kVoiceCount; vi++)
        if (voice_[vi].active)
          ++n_active;

      if (n_active >= 2 && tick)
      {
        // Advance the focus to the next active voice (round-robin).
        // breath_focus_ is an index into the active-voice list; we find the
        // current focus voice and move to the next active one.
        uint8_t cur = 0;
        bool found = false;
        for (uint8_t vi = 0; vi < kVoiceCount; vi++)
        {
          if (voice_[vi].active)
          {
            if (found) { cur = vi; break; }
            if (vi == breath_focus_) found = true;
          }
        }
        if (!found)
        {
          // Focus voice is no longer active: find the first active voice.
          for (uint8_t vi = 0; vi < kVoiceCount; vi++)
            if (voice_[vi].active) { cur = vi; break; }
        }
        // Advance to the next active voice after cur.
        for (uint8_t d = 1; d <= kVoiceCount; d++)
        {
          const uint8_t next = (cur + d) % kVoiceCount;
          if (voice_[next].active) { breath_focus_ = next; break; }
        }
      }

      // Compute per-voice targets and track them smoothly.
      // The focus voice gets BOOSTED (up to 2x at RW_LVL=100%), the other
      // voices get ATTENUATED (down to 0 at RW_LVL=100% & n=2). The total
      // loudness DOES change (the drone "swells" toward the focus note and
      // "recedes" from the others) — this is what makes the breath audible.
      if (n_active < 2)
      {
        // With 0 or 1 notes, breathing is off: level stays at 1.0.
        for (uint8_t vi = 0; vi < kVoiceCount; vi++)
          if (voice_[vi].active)
            voice_[vi].breath_dev = 1.f;
      }
      else
      {
        // Attenuation for the non-focus voices: at n=2 -> 1.0 (full mute),
        // n=3 -> 0.5, n=4 -> 0.67, n=5 -> 0.8, n=6 -> 0.8. The (n-2)/(n-1)
        // factor keeps the others clearly audible at higher note counts
        // (a drone with 6 notes shouldn't have 5 of them nearly silent).
        const float other_atten = ((float)n_active - 2.f) / (float)(n_active - 1);
        for (uint8_t vi = 0; vi < kVoiceCount; vi++)
        {
          Voice &v = voice_[vi];
          if (v.active)
          {
            const bool is_focus = (vi == breath_focus_);
            // Focus: 1 + rw_lvl (up to 2.0); Others: 1 - rw_lvl * other_atten
            // (down to 0 at n=2, 0.33 at n=4, 0.2 at n=6).
            v.breath_target = is_focus
              ? 1.f + p.rw_lvl
              : 1.f - p.rw_lvl * other_atten;
            // Smoothly track the target (RW_LSM time constant).
            v.breath_dev += (v.breath_target - v.breath_dev) * k;
          }
        }
      }
    }
    // When breath_active becomes false, reset all breath_dev to 1.0 (neutral
    // level) so the next activation starts from a static state.
    else
    {
      for (uint8_t vi = 0; vi < kVoiceCount; vi++)
        voice_[vi].breath_dev = 1.f;
    }

    // ---- Simulator-only shape-LFO (shared, phase-coherent) ------------------
    // On hardware lfo_ is delivered by the platform via setShapeLfo(); in the
    // web sim we synthesize it here. All voices share this one LFO.
#ifdef EMSCRIPTEN
    lfo_ = lfo_sim_.process();
#endif
    // Intra-bank LFO morph: the LFO drives the position within the selected
    // bank, centered on the bank (see WaveTable::resolve_wave / bank_center).
    // The base position is the bank center; the LFO adds +/-half the (size-1)
    // span, so at 100% LFO the morph sweeps from the first to the last table
    // of the bank (clamped to [0, bank_size-1] inside resolve_wave).
    const float lfo_morph =
        WaveTable::bank_center(wave_bank) + lfo_ * WaveTable::lfo_morph_depth(wave_bank);

    // ---- Per-voice loop: phasors, wavetables, ring-mod -----------------------
    // Each of the kVoiceCount cluster voices runs the SAME DSP chain with its
    // own w0 (possibly gliding). The A/B detune uses the SHARED static factors
    // (det_f_a_/det_f_b_) + the SHARED random-walk detune (rw_dev_a/b_cents);
    // the FM index, JIT, morph and walk are all SHARED (one drone); only the
    // phasors, FM-lookback and A-H-R are per-voice. (The sub phasors and the
    // sub-2 voice are now SHARED by the whole cluster — see the "Shared sub
    // bass" block below this loop, computed from the env-weighted anchor.)
    // Accumulators feed the single common post-stage below.
    float sum_main = 0.f;   // sum over voices of (wav_a*lvl + wav_b*lvl) — per-voice breath lvl
    float sum_ring = 0.f;   // sum over voices of (wav_a * wav_b)
    // ---- Shared sub bass anchor state (updated per-sample below) ------------
    // env-weighted mean of the sounding voices' base rates: the ONE pitch the
    // shared sub pair plays. Computed in the same pass as env_sum so there is
    // a single loop over the voices.
    float anchor_w_num = 0.f; // running sum(env * w0) over active voices
    float sub_env_focus = 0.f; // loudest (breath-focus) voice envelope, for SUBB
    {
      // Shared per-sample FM index (advance once, use for all voices).
      const float fm_a_wander = fm_a_wander ? (rw_[0].dev_b * rw_amt * fm_a_wander_depth) : 0.f;
      const float fm_b_wander = fm_b_wander ? (rw_[0].dev_a * rw_amt * fm_b_wander_depth) : 0.f;
      const float fm_a_eff = fm_a_static + fm_a_wander;  // effective index (cycles)
      const float fm_b_eff = fm_b_static + fm_b_wander;  // effective index (cycles)
      // Per-voice JIT: each voice draws its OWN white() so the notes are not
      // phase-locked (a shared draw would beat them together). (Plain arrays,
      // not const — they are written inside the loop below.)
      float jit_a[kVoiceCount];
      float jit_b[kVoiceCount];
      if (jit_active)
      {
        for (uint8_t vi = 0; vi < kVoiceCount; vi++)
        {
          jit_a[vi] = (osc_white() - 0.5f) * p.jit_cycles;
          jit_b[vi] = (osc_white() - 0.5f) * p.jit_cycles;
        }
      }
      else
      {
        for (uint8_t vi = 0; vi < kVoiceCount; vi++)
        {
          jit_a[vi] = 0.f;
          jit_b[vi] = 0.f;
        }
      }

      for (uint8_t vi = 0; vi < kVoiceCount; vi++)
      {
        Voice &v = voice_[vi];
        // Skip entirely silent voices (no note latched, envelope at 0): their
        // phasors need not advance and they contribute nothing. This keeps an
        // all-off cluster at near-zero cost (regression: 0 notes = silent).
        if (!v.active && v.eg.phase == AhEnvelope::kIdle)
          continue;

        // Current base phase increment (glide removed: always v.w0, the fixed
        // per-note rate).
        float vw0 = v.w0;

        // Per-voice detuned A/B increments. Walk active: combine static + walk
        // cents in one Taylor (shared by all voices); walk off: cached static
        // factors det_f_a_/det_f_b_ (0 Taylor cost).
        float w0_a, w0_b;
        if (rw_active)
        {
          const float half_det = 0.5f * p.detune_cents;
          w0_a = Detune::w0_detuned(vw0, +half_det + rw_dev_a_cents);
          w0_b = Detune::w0_detuned(vw0, -half_det + rw_dev_b_cents);
        }
        else
        {
          w0_a = vw0 * det_f_a_;
          w0_b = vw0 * det_f_b_;
        }

        // ---- Phase-FM (shared index, per-voice lookback state) --------------
        const float fm_a_raw = v.prev_wav_b * fm_a_eff;
        const float fm_b_raw = v.prev_wav_a * fm_b_eff;
        v.fm_smooth_a += kFmSmoothAlpha * (fm_a_raw - v.fm_smooth_a);
        v.fm_smooth_b += kFmSmoothAlpha * (fm_b_raw - v.fm_smooth_b);

        // ---- Advance A/B phasors (+FM smoothed +JIT), wrap to [0,1) --------
        // (The modular fast_floorf wrap handles the signed FM/JIT offsets so
        //  the phasor stays in [0,1) for any finite input — see the original
        //  wrap comment; one fast_floorf per phasor per sample.)
        v.ph_a += w0_a + v.fm_smooth_a + jit_a[vi];
        v.ph_b += w0_b + v.fm_smooth_b + jit_b[vi];
        v.ph_a -= fast_floorf(v.ph_a);
        v.ph_b -= fast_floorf(v.ph_b);

        // ---- SUPERSAW (GRIMM-style: single 7-saw stack instead of wavetables) ---
        // TEMPORARY: replaced wavetable lookup with a GRIMM-style supersaw:
        // 7 saw oscillators detuned symmetrically around the note (+/-60c total
        // spread, ~17c between neighbors). All 7 saws share the same base phasor
        // v.ph_a but each is offset by a fixed random-like phase spread to
        // de-correlate. Saw = 2*ph - 1 (range [-1,1]). The 7 saws are summed
        // and normalized by 1/7 so the output amplitude matches a single saw.
        // This tests whether the polyphonic howl is a wavetable artifact or
        // a fundamental issue with coherent summing of 2-3 voices.
        //
        // GRIMM supersaw: 7 saws at -60, -40, -20, 0, +20, +40, +60 cents
        // (symmetric spread). Each saw's phasor is offset from v.ph_a by a
        // fixed amount proportional to its detune: ph_i = ph_a + offset_i,
        // where offset_i = detune_cents_i / 1200.0 (in cycles, since 1200
        // cents = 1 octave = 1 cycle ratio... actually 1200 cents = 2x freq,
        // so a cent offset shifts the phasor increment, not the phase itself.
        // For a STATIC test we use fixed phase offsets that approximate the
        // steady-state phase drift of detuned oscillators over ~100 ms).
        //
        // Simpler approach: use v.ph_a as the base and add fixed phase offsets
        // that spread the 7 saws across one cycle (like GRIMM's random phase
        // initialization). This gives a static spread that de-correlates the
        // harmonics without per-sample detune computation.
        // ---- Wavetable samples (intra-bank LFO morph) ------------------------
        // Both A/B read the SAME wave: the LFO drives the position within the
        // selected bank, centered on the bank (see WaveTable::resolve_wave).
        // The A/B detune (w0_a/w0_b, computed above) provides the width; the
        // LFO provides the timbre movement. The bank boundary is hard: the
        // morph never crosses into the next bank.
        const float wav_a = WaveTable::resolve_wave(wave_bank, lfo_morph, v.ph_a);
        const float wav_b = WaveTable::resolve_wave(wave_bank, lfo_morph, v.ph_b);
        v.prev_wav_a = wav_a; // FM lookback cache (next sample's modulator)
        v.prev_wav_b = wav_b;

        // ---- Accumulate the common-post-stage inputs (per-voice Breath lvl) --
        // PER-VOICE ENVELOPE: each voice is gated by its OWN A-H-R envelope
        // (v.eg.env). This is what makes the polyphonic cluster work: a new
        // note (env 0→1 over 2 s) ADDS to the cluster instead of dragging the
        // whole thing through a common MAX that dips on the 1→2 note boundary.
        // (The old COMMON output envelope = max over all voices used to read
        // the cluster's level as its loudest note, so the instant a 2nd note
        // started from env=0 the shared gain could momentarily collapse ->
        // "silence" on the 2nd note, especially where the new note's envelope
        // reset coincides with the others. Per-voice gating removes that
        // shared-dip.) The local env_max below is a DIFFERENT, per-block
        // accumulator (the loudest voice's envelope, for the SUBB breath
        // follower) — it is NOT the old common output envelope.
        const float eg = v.eg.env;
        // Per-voice Breath level: each note's MAIN (A+B) voice gets its OWN
        // level factor (breath_dev, which tracks the rotating-focus target).
        // At RW_LVL=0 (default) breath_active=false and breath_dev=0 ->
        // lvl=1.0 (static, regression-safe). The sum of all per-voice levels
        // is preserved (sum-preserving), so the overall loudness does not
        // pump as the focus rotates from note to note.
        // Per-voice Breath level: breath_dev tracks the absolute level target
        // (focus: 1+rw_lvl, others: 1-rw_lvl*atten). At RW_LVL=0 breath_dev=0
        // and lvl=1.0 (static, regression-safe).
        const float lvl = breath_active ? v.breath_dev : 1.f;
        const float lvl_c = (lvl < 0.f) ? 0.f : (lvl > 2.f) ? 2.f : lvl;
        sum_main += (wav_a * lvl_c + wav_b * lvl_c) * eg;
        sum_ring += (wav_a * wav_b) * eg;
        // Shared sub-bass anchor bookkeeping (see the block below the loop):
        // the env-weighted mean w0 (anchor target) and the loudest voice's
        // envelope (SUBB breath focus) are gathered here, in the same pass.
        anchor_w_num += eg * vw0;
        if (eg > sub_env_focus) sub_env_focus = eg;
      }
    }

    // A+B mix (the shared post-stage input); per-voice Breath levels are
    // already folded into sum_main.
    //
    // POLYPHONIC LOUDNESS COMPENSATION (Variant A): every summed PER-VOICE
    // voice signal (sum_main / sum_ring) grows linearly with the number of
    // sounding notes, but the downstream kMix* budget (kMixOsc = 0.5, ...) was
    // designed for ONE note. Without compensation N notes add coherently
    // (worst case ~x4) and, fed through fold/saturate at the same FOLD/SATU,
    // clip and "shout" (fastertanh2f pegs at +1). So we scale each summed
    // signal by env_comp_ ~= 1/env_sum: N=1 -> 1 (mono EXACTLY unchanged),
    // N=4 -> 0.25 (one note's level back). This is applied BEFORE the shared
    // fold/saturator/SVF so the shared post-stage sees a single-note-magnitude
    // signal and the whole cluster reads as one DRONE at the intended
    // loudness. (env_comp_ is the smoothed 1/N from the envelope-advance block
    // above, so the 2 s attack/release don't pump it per sample.)
    // NOTE: the sub voices are NOT env_comp_ scaled — they are ONE SHARED pair
    // (not N per-voice voices), so there is no N-fold growth to compensate. A
    // single note -> env=1 -> exactly the old single-note sub level; N notes
    // -> one stable fundamental under the cluster (no N-fold low-end growth).
    const float mix  = kMixOsc * (sum_main * env_comp_);
    // RMOD: A*B ring-mod DERIVED voice (summed per-voice), MAIN-ducked.
    const float ring_out = rmod_active ? (sum_ring * env_comp_) * p.ring_mix * p.main_level : 0.f;

    // ---- Shared sub bass (ONE sub pair for the whole cluster) ---------------
    // The env-weighted mean of the sounding voices' base rates is the ONE
    // pitch the shared sub pair plays (the "anchor"). A one-pole relax
    // (sub_anchor_alpha, ~250 ms) glides the anchor when notes join/leave, so
    // the low end MORPHS instead of stepping/clicking. At 0 notes the target
    // is 0 and the anchor relaxes to 0 (the sub fades out with the cluster).
    //
    // The sub LEVEL breathes with the loudest (breath-focus) voice: the SUBB
    // knob (p.subb_depth, 0..1) sets the swing depth d = subb_depth *
    // kSubBreathMax; a one-pole follower (kSubBreathTauSec) drives
    // sub_breath_gain toward (1 + (sub_env_focus - 0.5) * 2 * d) and is floored
    // at kSubBreathFloor (the low end never fully vanishes while a note
    // sounds). At SUBB = 0 the gain is constant 1.0 (the old static low end —
    // regression-safe). At a single note sub_env_focus = that note's A-H-R ->
    // the gain follows it (swelling with the attack, sinking on release),
    // which is exactly the "the low end follows the breathing note" behaviour.
    // The (sub_env_focus - 0.5) * 2 mapping centers the gain at 1.0
    // (sub_env_focus = 0.5 -> no swing) so a held note at full level sits at
    // exactly the old static level; the swing is asymmetric in practice (a
    // note's env spends most of its time above 0.5, so the low end sits
    // slightly ABOVE static — the drone "breathes in" with the note).
    //
    // The phasors advance with the ANCHOR rate (not any single voice's w0), so
    // the sub bass is one continuous fundamental under the whole cluster.
    {
      // Anchor target: the raw env-weighted mean (sum(env*w0)/sum(env)).
      // env_sum_ (the raw per-sample sum of the per-voice A-H-R levels,
      // updated in the envelope-advance block below) is the denominator;
      // anchor_w_num (the running sum(env*w0) gathered in the per-voice loop
      // above) is the numerator. Using the RAW sum (not env_comp_, the
      // one-pole smoothed loudness-comp gain) keeps the anchor tracking the
      // true instantaneous mean.
      const float env_now = env_sum_;
      const float anchor_target = (env_now > 1e-6f) ? (anchor_w_num / env_now) : 0.f;
      // One-pole relax (note on/off -> glide, no click).
      sub_anchor_w0 += (anchor_target - sub_anchor_w0) * sub_anchor_alpha;
      // SUBB breath gain: one-pole follower toward (1 + (focus - 0.5)*2*d).
      // (sub_breath_alpha is the K-rate-precomputed 1 - exp(-1/tau), tau =
      // kSubBreathTauSec — see the prepass block above the per-sample loop.)
      const float subb_d = p.subb_depth * kSubBreathMax;
      const float subb_target = 1.f + (sub_env_focus - 0.5f) * 2.f * subb_d;
      sub_breath_gain += (subb_target - sub_breath_gain) * sub_breath_alpha;
      if (sub_breath_gain < kSubBreathFloor) sub_breath_gain = kSubBreathFloor;
      if (sub_breath_gain > 1.f + kSubBreathMax) sub_breath_gain = 1.f + kSubBreathMax;
      // SUB1 (f/2) phasor: anchor rate, shared LFO-FM detune (the low end's
      // slow organic "swell"). kSubW0Scale includes the -4 c fixed detune.
      {
        const float lfo_fm_cents = lfo_ * kSubLfoFmCents;
        sub_ph += sub_anchor_w0 * kSubW0Scale * Detune::factor(lfo_fm_cents);
        sub_ph -= fast_floorf(sub_ph);
      }
      // SUB2 (f/4) phasor: stable low-end anchor, no LFO-FM.
      sub_ph2 += sub_anchor_w0 * 0.25f;
      sub_ph2 -= fast_floorf(sub_ph2);
    }
    // Sub-octave / sub-2 (ONE shared pair; the SUBB breath gain modulates the
    // level, the anchor modulates the pitch; scaled by their levels in the mix).
    // NOTE: the local float `sub2_out` must NOT shadow the member `sub2`
    // (Sub2Voice) — the old name `sub2` would hide the member and break the
    // .process() call.
    const float sub    = osc_sinf(sub_ph) * sub_breath_gain;
    const float sub2_out = sub2.process(sub_ph2, sub_anchor_w0, sub_breath_gain);

    // The COMMON post-stage input is `mix` (the per-voice A+B sum, computed in
    // the loop above as kMixOsc * sum_main). It is then run through the SINGLE
    // shared pre-drive wavefolder (FOLD knob) and the asymmetric soft-clip
    // saturator (SATU/ASYM knobs) — one instance for the whole cluster.
    // (The static DETU knob is applied per-voice above via det_f_a_/det_f_b_,
    //  not post-mix: A and B are detuned in opposite directions around each
    //  voice's note, so every note keeps its own beating A/B pair.)
    // Wavefolder depth: FOLD knob modulated by random-walk fold (RW_FLD knob).
    // Multiplicative: eff_fold = p.fold_depth * (1 + rw_fold_mod), where
    // rw_fold_mod = dev_b * p.rw_fld (normalized -1..1 * 0..1 = -1..1).
    // At RFLD=0: eff_fold = p.fold_depth (no modulation).
    // At RFLD=100%: eff_fold wanders from 0 to 2*p.fold_depth (clamped to 1).
    // When p.fold_depth = 0: eff_fold = 0 (no fold, RFLD has no effect).
    float eff_fold = p.fold_depth * (1.f + rw_fold_mod);
    if (eff_fold < 0.f) eff_fold = 0.f;
    else if (eff_fold > 1.f) eff_fold = 1.f;
    // Saturator drive: SATU knob + random-walk drive (RW_DRV knob, via
    // rw_dev_drive, per-sample). Clamp to 0..kSatuMax so the walk never
    // pushes the drive beyond the saturator's stable range.
    float eff_drive_clamped = p.drive + rw_dev_drive;
    if (eff_drive_clamped < 0.f) eff_drive_clamped = 0.f;
    else if (eff_drive_clamped > kSatuMax) eff_drive_clamped = kSatuMax;
    // (FOLD2 — the second cascaded wavefolder — was removed: the A+B chain
    //  now runs a single wavefolder (FOLD) before the saturator.)
    const float folded = Saturator::fold(mix, eff_fold);
    // Shared post-stage saturator (original): one saturator for the whole cluster.
    const float y = kMixOsc * Saturator::saturate(folded, eff_drive_clamped, p.asym);

    // ---- internal SVF (12 dB/oct low-pass, post-saturator) ----
    // When the FCUT dry/wet (svf_wet) is non-zero, the saturator output is
    // low-passed by the SVF before the sub mix. The cutoff is modulated
    // per-sample by the shared random walk (RWFC depth is folded into the
    // FCUT knob's dry/wet — see apply_edit_param): the effective cutoff
    // coefficient svf_.cc_w wanders by +/-kRwCutoffHalfOctaves octaves
    // around the FCUT knob value, so the drone's top end "breathes" on the
    // same 0.1 s .. 10 s timescale as the detune / morph / drive walks.
    // When svf_wet == 0 (the default) the filter is transparent and this
    // whole block is skipped (regression-safe).
    //
    // The walk modulation is applied to the per-sample cutoff coefficient
    // (cc_w = cc * 2^(+/-half-oct * dev)), clamped to the stable range. The
    // deviation used is dev_a (the same channel as the detune walk, so the
    // top-end "breathes" in sync with the chorus).
    float y_filt = y;
    if (svf_active)
    {
      // Walk-modified cutoff coefficient: cc * 2^(+/-half-oct * dev_a).
      // The deviation (dev_a, [-1,+1]) is scaled by the RWFC depth
      // (p.rw_fc, 0..1) and the global walk depth (rw_amt) so the cutoff
      // wanders by at most kRwCutoffHalfOctaves octaves at full depth.
      const float rw_fc_dev = (rw_active && p.rw_fc > 0.f)
          ? rw_[1].dev_a * rw_amt * p.rw_fc  // Timbre stream (rw_[1])
          : 0.f;
      const float oct_dev = Svf::kRwCutoffHalfOctaves * rw_fc_dev;
      // 2^oct_dev via exp2: 2^x = exp2(x). oct_dev is small (|x| <= 1), so
      // a 3rd-order Taylor in y = oct_dev * ln(2) is more than enough.
      const float ln2 = 0.6931471805599453f;
      const float e = oct_dev * ln2;
      const float scale = 1.f + e + (e * e * 0.5f);
      float cc_w = svf_.cc * scale;
      if (cc_w < Svf::kCcMin) cc_w = Svf::kCcMin;
      else if (cc_w > Svf::kCcMax) cc_w = Svf::kCcMax;
      svf_.cc_w = cc_w;
      y_filt = svf_.process(y, svf_wet);
    }


    // ---- Enrichment-2: formant resonators (two one-pole IIR combs, F1+F2) ----
    // When FVOL (formant_wet) > 0, run the post-SVF main output through TWO
    // parallel one-pole IIR combs: an F1 comb (formant_delay1_smp_, the vowel's
    // first formant, 135..365 Hz peak) and an F2 comb (formant_delay2_smp_, the
    // vowel's second formant, 435..1145 Hz peak), both morphed by the FORM
    // knob across A-E-I-O-U. Each comb is a ring-buffer delay whose output is
    // summed back around one feedback tap (formant_fb_, 0..0.9, shared) --
    // ALWAYS STABLE (|fb| < 1). The two combs' "color" (fb * delayed sample,
    // i.e. the comb output minus the dry input) is summed and ADDED on top of
    // the intact main signal (parallel branch, NOT dry/wet). F2 is what makes
    // the vowels distinct: I/E (high F2) sound "bright/open", U/O (low F2)
    // "warm/dark". At FVOL = 0 (default) the stage is bypassed (regression-
    // safe). The color is scaled by kFormantGain so a full FVOL=100% adds a
    // clear vocal tint without overdriving the final soft-clip.
    if (p.formant_wet > 0.f)
    {
      const uint32_t mask = kFormantDelayMaxSmp - 1U;
      // F1 comb: write, read formant_delay1_smp_ back, advance.
      formant_comb_buf1_[formant_comb_w1_] = y_filt;
      const float yd1 = formant_comb_buf1_[(formant_comb_w1_ + (mask + 1U - formant_delay1_smp_)) & mask];
      formant_comb_w1_ = (formant_comb_w1_ + 1U) & mask;
      // F2 comb: write, read formant_delay2_smp_ back, advance.
      formant_comb_buf2_[formant_comb_w2_] = y_filt;
      const float yd2 = formant_comb_buf2_[(formant_comb_w2_ + (mask + 1U - formant_delay2_smp_)) & mask];
      formant_comb_w2_ = (formant_comb_w2_ + 1U) & mask;
      // Parallel branch: keep the intact main signal, add both comb colors
      // (F1 + F2, each scaled by the shared feedback and the stage gain).
      const float formant_color = formant_fb_ * (yd1 + yd2);
      y_filt = y_filt + formant_color * kFormantGain * p.formant_wet;
    }

    // ---- Enrichment-2: comb / resonator (IIR feedback through a delay) ----
    // When COMB (comb_fb) > 0, run the main output through a one-pole IIR
    // comb: a fixed-length ring-buffer delay (kCombDelaySmp, ~190 Hz peak)
    // whose output is summed back around the feedback loop. This produces a
    // resonant peak + harmonics (a "wooden / cavernous" tint). At COMB = 0
    // (default) the branch is a pure wire (regression-safe). The buffer is a
    // fixed member (kCombDelayMaxSmp floats); the read index is a fast mask.
    if (p.comb_fb > 0.f)
    {
      // Write the current sample, read the delayed sample kCombDelaySmp back.
      comb_buf_[comb_w_] = y_filt;
      const uint32_t r = (comb_w_ + (kCombDelayMaxSmp - kCombDelaySmp)) & (kCombDelayMaxSmp - 1U);
      const float yd = comb_buf_[r];
      comb_w_ = (comb_w_ + 1U) & (kCombDelayMaxSmp - 1U);
      // IIR comb: y = in + fb * delayed (one feedback tap).
      y_filt = y_filt + p.comb_fb * yd;
    }
    // ---- advance the A-H-R envelopes by one sample (PER-VOICE) -------------
    // Each cluster note has its OWN envelope (attack when its note starts,
    // release when it is toggled off / stolen / allNoteOff), so the notes
    // fade in and out independently — the heart of the cluster. (Per-sample
    // so the 2 s phases are sample-accurate at any block size.)
    //
    // While advancing, also build env_sum_ : the sum of the per-voice
    // envelope levels (0..kVoiceCount). This is how "many" notes are sounding;
    // it feeds the POLYPHONIC LOUDNESS COMPENSATION (env_comp_ = 1/N, a
    // per-sample one-pole smooth below) that scales the summed PER-VOICE voice
    // signals (sum_main / sum_ring) so N notes read at ~one note's loudness
    // instead of N times louder. (The shared sub bass does NOT use env_comp_:
    // it is ONE pair, not N per-voice voices — see the "Shared sub bass"
    // block. It DOES use env_sum_ as the denominator of its env-weighted
    // ANCHOR mean.)
    // (The per-voice envelope itself is applied in the accumulation loop above,
    // NOT here: each voice is gated by its own v.eg.env.)
    {
      float s = 0.f;
      for (uint8_t vi = 0; vi < kVoiceCount; vi++)
      {
        Voice &v = voice_[vi];
        v.eg.advance(alpha_a, alpha_r);
        s += v.eg.env;
      }
      env_sum_ = s;
    }
    // One-pole follow the cluster loudness (1/N) so the makeup gain moves
    // smoothly with note count / the 2 s envelope, not per-sample.
    // Target = 1/env_sum with env_sum floored at 1 (a single note is never
    // attenuated; silence -> 1 = no effect on the zero signal). Linear 1/N
    // compensation so the shared saturator sees the same input level
    // regardless of voice count: N=1 -> 1.0 (mono unchanged), N=2 -> 0.5,
    // N=3 -> 0.333, N=4 -> 0.25. This prevents the tanh from generating more
    // harmonics at higher N. (The 1/sqrt(N) binomial series was tried first
    // but read too "pumpy" on the 1->2 note boundary; 1/N is gentler and the
    // shared saturator's drive headroom already covers the single-note case.)
    {
      const float n = (env_sum_ > 1.f) ? env_sum_ : 1.f;
      const float target = 1.0f / n;
      env_comp_ += kClusterCompAlpha * (target - env_comp_);
    }

    // scale output to a musical level (saturator max output is ~0.76)
    // and apply the A-H-R envelope level.
    //
    // Four parallel branches:
    //   - y_filt (A+B through folder/saturator/SVF) * main_level  -> MAIN knob
    //   - the CHORD voice — runs through its OWN folder+saturator pass (same
    //     SATU/ASYM/FOLD knobs) so it keeps the unit's drive "color", and is
    //     mixed in parallel so the MAIN knob ducks only A+B. Scaled by
    //     kMixChordDirect so it lands near the A+B level (not the old ~0.05).
    //   - sub (sub-octave sine) and sub2 (f/4) — low end, bypass soft-clip.
    // Per-branch worst-case peaks (saturator outputs bounded in (-1, 1)):
    //   |y_filt * 0.5f|    < 0.5  (A+B through saturator)
    //   |chord_sat * 0.8f| < 0.8  (chord through its own saturator)
    //   |sub * 0.5f|       < 0.5  (sine bounded in [-1, 1])
    //   |sub2 * 0.4f|      < 0.4  (LPF'd, gated voice bounded in (-1, 1))
    //   |sum|              < 2.2  (worst case, branches rarely peak together;
    //                               the hard clip below guards digital overflow)
    // At the default SUB1 = 50%, the sub branch is at 0.25 amplitude; SUB2
    // defaults to 0 (off). A hard clip prevents digital overflow when all
    // branches peak simultaneously.
    //
    // (chord_drive_clamped / chord_fold_clamped / chord_sat were removed — the
    //  chord/interval concept is gone.)
    // ---- RMOD: A*B ring-mod DERIVED voice (NOT in A+B, but ducks with it) ----
    // (wav_a * wav_b) * ring_mix is a spectral "difference" (the two
    // wavetables' partials shifted by +/- each other's frequencies) — it
    // adds metallic "ring" / "beating" without disturbing the A+B
    // fundamental. Mixed in parallel (like the chord) so RMOD=100% adds a
    // full-level metallic "ring" on top of the intact A+B tone, instead of
    // replacing it (which is what an in-A+B mix would do). At ring_mix = 0
    // (default) the branch is 0 (regression-safe).
    //
    // Unlike the chord, RMOD is a DERIVED voice of A+B (it is literally
    // wav_a * wav_b), so it is ducked by the MAIN level knob: at MAIN=0 the
    // whole main-voice cluster (A+B + its FM/FM-JIT/RMOD byproducts) goes
    // silent together. This was a bug — MAIN only scaled y_filt, so
    // MAIN=0 + RMOD>0 still produced an audible metallic tone out of a
    // "muted" voice. (FM A / FM B themselves modulate the A/B phasors, not
    // levels: they reshape wav_a/wav_b and hence the ring spectrum, but
    // cannot raise its amplitude — |wav_a * wav_b| <= 1 always.)
    // (ring_out is computed in the per-voice loop above from the summed
    //  per-voice ring-mod: rmod_active ? sum_ring * p.ring_mix * p.main_level.)
    // Final output: the summed mix is lifted by the makeup gain kOutDrive
    // (~+8 dB) and passed through the SAME tanh soft-clip the reference
    // oscillators use (kOutSat = 1.4125, see platform/nts-1_mkii/waves/osc.h).
    // This replaces the old hard clip, which both read ~-12 dBFS too quiet
    // (the kMix* budget is a peak guard, not a loudness target) and produced
    // a square-wave edge when all five branches peaked together. The tanh
    // rolls off smoothly above the near-linear region (effective total gain
    // kOutDrive * kOutSat ~= +13 dB in the linear band, saturating to 1),
    // keeping the digital overflow guard while matching the platform's
    // reference loudness. See the kOutDrive / kOutSat comments in
    // osc_constants.h (tune kOutDrive there, not the kMix* budget).
    // ---- Constant sidechain ducking of the MAIN voice by SUB1/SUB2 ---------
    // (Formerly the DCK knob; now a fixed amount kDuckConstant -- the drone's
    // low end must always read as the anchor.) A one-pole abs-peak follower
    // tracks the combined SUB1 + SUB2 output energy (|sub + sub2| with their
    // levels, i.e. the low end that actually reaches the mix); the follower
    // then DUCKS the MAIN voice (y_filt * kMixOsc * MAIN and the RMOD branch)
    // so the main tone "sinks" out of the way of the low end -- the classic
    // sidechain "pump", inverted: the low end is the constant anchor and the
    // main voice yields. This is the audible drone behaviour: at a held note
    // the sub sines are steady, so the follower settles to a constant duck
    // gain (the main tone sits a few dB lower while the subs are present) and
    // any low-end movement (the SUBB breath swing on the shared sub pair, note
    // on/off, the SUB2 pitch gate) moves the main tone with it. The duck gain
    // is floored at kDuckFloor
    // (0.7, ~-3 dB) so the main voice never disappears. The duck is ALWAYS
    // active (the follower advances every sample); the RMOD branch ducks with
    // y_filt: it is a DERIVED voice of A+B (wav_a * wav_b), so the whole
    // main-voice cluster (A+B + its FM/JIT/RMOD byproducts) sinks together --
    // the same logic the MAIN knob applies (see the RMOD comment above).
    {
      // Follower source: the (level-scaled) sub-voice sum, i.e. the low end
      // that actually reaches the mix (sub * SUB1, sub2_out * SUB2).
      const float sub_src = sub * p.sub_level + sub2_out * p.sub2_level;
      const float inst = (sub_src < 0.f) ? -sub_src : sub_src;
      env_follower_ += (inst - env_follower_) * duck_alpha_;
      float duck_gain = 1.f - kDuckConstant * env_follower_ * kDuckScale;
      if (duck_gain < kDuckFloor) duck_gain = kDuckFloor;
      if (duck_gain > 1.f) duck_gain = 1.f;
      // The COMMON post-stage output (main + ring, ducked by the shared low end)
      // is shared by all voices. The A-H-R is now PER-VOICE: each voice was
      // already gated by its own envelope (v.eg.env) in the accumulation loop
      // above, so there is NO common output envelope here. N simultaneous notes
      // each fade in/out on their own; the cluster reads as one blended drone
      // whose level is the SUM of the individually-enveloped voices (each
      // already scaled by env_comp_ = 1/env_sum for polyphonic makeup — the
      // shared sub bass is NOT env_comp_ scaled, it is ONE pair). A single
      // note -> env = its own level (mono exact); N notes -> each note
      // contributes its own envelope, no shared-dip on the 1→2 boundary.
      const float raw = (y_filt * p.main_level * duck_gain
                       + sub * p.sub_level * kMixSub
                       + sub2_out * p.sub2_level * kMixSub2
                       + ring_out * kMixRing * duck_gain) * kOutDrive;
      out[0] = clip1m1f(fastertanh2f(kOutSat * raw));
    }
  }
}
// K-rate per-block prepass extracted from process() (see the call site there):
// web-sim edit-parameter request consumption (K-rate atomic apply of the
// select_/PARAM_VAL pair), rndTick() (Rnd debounce + re-roll), rndMorphTick()
// (BPM-synced re-roll morph). All of it runs once per block, before the
// per-sample loop, so osc_mix.h keeps its "pure audio path" property.
inline void Osc::blockPrepass(uint32_t frames)
{

#ifdef __EMSCRIPTEN__
  // ---- Web-sim Rnd re-roll request (main-thread UI -> audio worklet) --------
  // The web simulator's All / Part / Orbi buttons run on the MAIN thread and
  // call Osc::simRndRequest(mode), which packs (mode, valid) into
  // sim_rnd_request_ (see osc.h). The main-thread and worklet-thread
  // processors are SEPARATE COPIES (wasm.cc), so the main thread cannot arm
  // the worklet's rnd_fire_ directly -- this is the bridge. Consume the
  // request here (worklet thread) by arming the worklet's own debounce with
  // the requested mode, NOT debounced (a single click is a single event,
  // exactly like the hardware Fire-button path). The consumption must happen
  // BEFORE rndTick() below so the re-roll fires in the very same block.
  if (sim_rnd_request_ & (1U << 31))
  {
    const uint8_t mode = static_cast<uint8_t>(sim_rnd_request_ & 0x7U);
    sim_rnd_request_ = 0; // clear before arming (no re-fire)
    rnd_mode_ = (mode <= kRndModeOrbi) ? mode : kRndModeAll;
    rndArm(false);
  }
#endif

  // ---- Rnd debounce tick (audio clock) --------------------------------------
  // Advance the audio clock and consume any pending Rnd re-roll (immediate or
  // the final one after a 0.5 s silence window). Runs in BOTH builds: the
  // hardware Rnd slot (unit.cc) and the web simulator's *Random* button both
  // arm the debounce (rndArm / rndRequest), and this is the single place the
  // re-roll actually fires. See the Rnd debounce comment block in osc.h.
  rndTick(frames);

  // ---- Rnd re-roll morph (BPM-synced smooth transition) --------------------
  // A re-roll that fired this block (rndTick above) started a morph toward
  // its fresh targets; advance the morph clock here and re-apply the
  // in-flight parameters (smoothstep-interpolated raw values) through the
  // same apply_edit_param() path a manual PARAM_VAL edit uses. Runs AFTER
  // rndTick so a fresh fire is picked up in the very same block. When no
  // morph is active this is a single branch check (zero cost). See
  // Osc::rndMorphStart / rndMorphTick in osc.h.
  rndMorphTick(frames);

  // (chord interval pitch crossfade removed — the chord/interval concept is gone.)

  // (All pitch glide / portamento was removed in the polyphonic build: each
  //  cluster note starts at its exact pitch with a random per-voice start
  //  phase, so blockPrepass advances no glide.)
}
