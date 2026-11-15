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
  // clock (rndTick) and the Rnd re-roll morph (rndMorphTick), and advance
  // the pitch glide. Everything here runs at K-rate (once per block, before
  // the per-sample loop) and is extracted from process() so that this file
  // stays a pure per-sample audio path (see the file header comment).
  blockPrepass(frames);
  // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
  const Params p = params_;

  // ---- Per-voice base w0 (the raw note-rate phasor advance) --------------------
  // In the polyphonic cluster each voice has its OWN w0 (voice_[vi].w0),
  // advanced by the per-voice GLIDE block in the loop below. There is no
  // single shared w0 anymore. The static A/B detune is the SHARED factor pair
  // det_f_a_ / det_f_b_ (K-rate, apply_edit_param(DETUNE)) multiplied by each
  // voice's w0 in the loop; the random-walk detune (rw_dev_a/b_cents) is also
  // shared and added per-voice.
  //
  // (The A-H-R relaxation coefficients are cached once per block for cheap
  //  access by the per-voice envelope advance below.)
  const float alpha_a = eg_alpha_a_;
  const float alpha_r = eg_alpha_r_;

  const bool rw_active = p.rw_amount > 0.f;

  // wavetable morph: base (WAV1/WAV2 knob). The pitch-shift morph is now
  // PER-VOICE (computed from each voice's own w0 in the loop), so only the
  // knob base is cached here; the LFO + walk morph offsets are added per-sample.
  const float wave_base_a = p.wave_a;
  const float wave_base_b = p.wave_b;
  if (rw_active)
  {
    // Recompute each stream's interval / tracking from its own SPDx/SMTx
    // (setup() is cheap and caches; the shared rw_amount ceiling is passed
    // but only used for max_dev, which does not affect the period/tracking).
    // The first three streams are addressed by p.rw_rate[i] / p.rw_smooth[i]
    // (SPD1..3 / SMT1..3, 3-element arrays). The fourth stream (Breath, i=3)
    // has its OWN knobs (RW_LSP / RW_LSM -> p.rw_lsp / p.rw_lsm), set at
    // K-rate in apply_edit_param — so it is set up separately below, NOT in
    // this loop (which would read out-of-bounds rw_rate[3] / rw_smooth[3]).
    for (uint8_t i = 0; i < 3; i++)
      rw_[i].setup(p.rw_rate[i], p.rw_amount, p.rw_smooth[i]);
    rw_[3].setup(p.rw_lsp, p.rw_amount, p.rw_lsm); // Breath stream (RW_LSP/RW_LSM)
    // Initialize each stream's targets to its current deviations on first
    // activation (avoids a visible jump when the RATE knob crosses 0).
    for (uint8_t i = 0; i < kRwStreamCount; i++)
    {
      if (rw_[i].target_a == 0.f && rw_[i].dev_a != 0.f)
        rw_[i].target_a = rw_[i].dev_a;
      if (rw_[i].target_b == 0.f && rw_[i].dev_b != 0.f)
        rw_[i].target_b = rw_[i].dev_b;
    }
  }

  // Random-walk scaled values (computed per-sample inside the loop below).
  // Each stream's dev_a/b (rw_[i].dev_a/b) are normalized to [-1, +1] and
  // advance per-sample via rw_[i].advance(). Each stream scales its own
  // deviations to its targets' units at the point of use:
  //   Tone (rw_[0])    -> detune (cents) + cross-FM
  //   Timbre (rw_[1])  -> wavetable morph (tables) + SVF cutoff
  //   Density (rw_[2]) -> drive + wavefolder depth
  // The scaled values must be recomputed every sample (not cached per-block)
  // because rw_[i].advance() moves dev_a/b on every sample.
  const bool rw_wav_active = rw_active && p.rw_wav > 0.f;
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
    // post-stage. Per sample we (1) advance each voice's GLIDE, (2) advance
    // its phasors (with the SHARED random-walk detune/morph/FM/jitter), (3)
    // sum its A/B wavetable outputs, subs and ring-mod into shared accumu-
    // lators, then (4) run the SINGLE common post-stage (folder -> saturator
    // -> SVF -> formant -> comb -> duck -> makeup + soft-clip) on the sums.
    // The A-H-R envelope is per-voice (each note fades in/out on its own);
    // the duck follower is shared (one drone's low end).
    // ================================================================
    // ---- Per-voice GLIDE (GRIMM-style exponential portamento) ---------
    // Each voice glides from its old pitch to a new one (glide_to) by
    // exponentially decaying glide_dist (in 1/4096-octave units) toward 0:
    //   d = (gl_k_ * x) >> 16;  x -= d;  snap when |x| < kGlideSnap.
    // gl_k_ is the per-sample Q16 fraction of the way left (K-rate, from the
    // GLIDE knob). The CURRENT pitch is computed ABSOLUTELY (not
    // cumulatively):
    //   vw0 = glide_to * 2^(-glide_dist / 4096)
    // where glide_dist > 0 means "still below target, gliding UP" and
    // glide_dist < 0 means "still above target, gliding DOWN". At
    // glide_dist = 0, vw0 = glide_to (arrived). v.w0 is NEVER modified
    // during the glide — it holds the target and is used once the glide
    // completes. This avoids the cumulative-multiplication blow-up that
    // would occur if v.w0 were scaled by 2^(x/4096) every sample (at an
    // octave offset x/4096 = 1, so 2^1 = 2 per sample → exponential
    // runaway in a few dozen samples).
    //
    // 2^f is computed as 2^int(f) * 2^frac(f) where frac ∈ [0,1) (or
    // [-1,0) for negative f). 2^int(f) is a power-of-two float (exact).
    // 2^frac is a 4th-order Taylor in e = frac * ln2 (|e| < 0.693 →
    // converges, error < 1e-6). No libm needed.
    {
      const int32_t gk = (int32_t)gl_k_; // Q16 per-sample glide fraction
      for (uint8_t vi = 0; vi < kVoiceCount; vi++)
      {
        Voice &v = voice_[vi];
        if (v.glide_active && v.glide_dist != 0)
        {
          int32_t x = v.glide_dist;
          if (gk > 0)
          {
            int32_t d = (int32_t)(((int64_t)gk * x) >> 16);
            if (d == 0) d = (x > 0) ? 1 : -1; // at least a step
            x -= d;
            if (x > -kGlideSnap && x < kGlideSnap)
            {
              x = 0;
              v.glide_active = false;
              v.w0 = v.glide_to; // arrived: snap v.w0 to the target
            }
            v.glide_dist = x;
          }
          else
          {
            // GLIDE = 0 (off): snap straight to the target.
            v.glide_active = false;
            v.glide_dist = 0;
            v.w0 = v.glide_to;
          }
        }
        else if (v.glide_dist != 0)
        {
          // Not gliding (shouldn't happen) but a stale offset: clear it.
          v.glide_dist = 0;
        }
      }
    }

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
    // Timbre stream (rw_[1]): wavetable morph A/B.
    const float rw_dev_a_tables = rw_wav_active ? rw_[1].dev_a * rw_amt * kRwMorphScaleTables : 0.f;
    const float rw_dev_b_tables = rw_wav_active ? rw_[1].dev_b * rw_amt * kRwMorphScaleTables : 0.f;
    // Density stream (rw_[2]): saturator drive + wavefolder depth.
    const float rw_dev_drive    = rw_drv_active ? rw_[2].dev_a * rw_amt * kRwDriveScale : 0.f;
    // Multiplicative: eff_fold = p.fold_depth * (1 + dev_b * p.rw_fld * rw_amt).
    // p.rw_fld is 0..1 (RFLD knob). At RFLD=0 -> eff_fold = p.fold_depth (no mod).
    // At RFLD=100% & RATE=100% -> eff_fold wanders from 0 to 2*p.fold_depth
    // (clamped to 1). The rw_amt factor keeps the folder walk gated by the
    // global RATE AMT depth, consistent with the detune/morph/drive walks.
    // When p.fold_depth = 0, eff_fold = 0 regardless (no fold).
    const float rw_fold_mod = rw_fld_active ? rw_[2].dev_b * p.rw_fld * rw_amt : 0.f;

    // ---- Breath stream (rw_[3]): voice-level wandering -------------------
    // The fourth stream drives the four voice LEVELS (A / B / chord / sub)
    // so they slowly "breathe" in and out of prominence. Its four channels
    // (dev_a/b/c/d) wander independently (each with its own period), so the
    // four voices trade the floor at different times — the heart of a drone.
    // The per-voice level factor is (1 + rw_lvl * rw_amount_lin * 2 * dev),
    // clamped to [0, 1]: at RW_LVL = 0 (default) all factors are 1.0 (static
    // levels, regression-safe: identical to the pre-breath sound). The linear
    // rw_amount_lin (not the quadratic rw_amount) is used so that at
    // RATE=50% the breath can still reach full depth (dev=-1 -> lvl=0) and
    // a voice can FULLY disappear; the quadratic rw_amount would cap breath
    // at 25% depth (lvl >= 0.75) at RATE=50%. The x2 factor means at
    // RW_LVL=100% & RATE=100% the swing is 2.0, so dev=-1 gives lvl=0 and
    // dev=+1 gives lvl=2 (clamped to 1).
    // SUB2 (the f/4 low-end anchor) is intentionally NOT breathed: it stays
    // static so the low end never "flutters".
    float lvl_a = 1.f;
    float lvl_b = 1.f;
    float lvl_sub = 1.f;
    // (lvl_c was removed — the chord/interval concept is gone.)
    if (rw_active && p.rw_lvl > 0.f)
    {
      rw_[3].advance(p.rw_amount, osc_white); // ceiling for advance() stays quadratic (unchanged)
      const float swing = p.rw_lvl * p.rw_amount_lin * 2.f; // 0..2 — full depth at RATE=100% & RVLL=100%
      lvl_a   = 1.f + swing * rw_[3].dev_a;
      lvl_b   = 1.f + swing * rw_[3].dev_b;
      lvl_sub = 1.f + swing * rw_[3].dev_d;
      if (lvl_a < 0.f) lvl_a = 0.f; else if (lvl_a > 1.f) lvl_a = 1.f;
      if (lvl_b < 0.f) lvl_b = 0.f; else if (lvl_b > 1.f) lvl_b = 1.f;
      if (lvl_sub < 0.f) lvl_sub = 0.f; else if (lvl_sub > 1.f) lvl_sub = 1.f;
    }

    // ---- Simulator-only shape-LFO (shared, phase-coherent) ------------------
    // On hardware lfo_ is delivered by the platform via setShapeLfo(); in the
    // web sim we synthesize it here. All voices share this one LFO.
#ifdef EMSCRIPTEN
    lfo_ = lfo_sim_.process();
#endif
    const float lfo_morph = lfo_ * WaveTable::kLfoMorphDepth;

    // ---- Per-voice loop: phasors, wavetables, subs, ring-mod ----------------
    // Each of the kVoiceCount cluster voices runs the SAME DSP chain with its
    // own w0 (possibly gliding). The A/B detune uses the SHARED static factors
    // (det_f_a_/det_f_b_) + the SHARED random-walk detune (rw_dev_a/b_cents);
    // the FM index, JIT, morph and walk are all SHARED (one drone); only the
    // phasors, FM-lookback, sub phasors, sub-2 state and A-H-R are per-voice.
    // Accumulators feed the single common post-stage below.
    float sum_main = 0.f;   // sum over voices of (wav_a*lvl_a + wav_b*lvl_b)
    float sum_sub  = 0.f;   // sum over voices of the sub-octave sine
    float sum_sub2 = 0.f;   // sum over voices of the f/4 sub-2 output
    float sum_ring = 0.f;   // sum over voices of (wav_a * wav_b)
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

        // Current base phase increment: if gliding, compute ABSOLUTELY as
        // glide_to * 2^(-glide_dist/4096); otherwise use v.w0 (the target,
        // which equals glide_to once the glide completes).
        float vw0;
        if (v.glide_active && v.glide_dist != 0)
        {
          // f = -glide_dist / 4096  (negative when glide_dist > 0 → gliding UP)
          // Clamp to [-12, 12] (±12 semitones) to stay within the kTwoPow LUT
          // range. A larger interval (beyond an octave of offset) clamps so a
          // very wide glide "runs out" at the extremes instead of indexing the
          // LUT out of bounds (which would corrupt vw0 -> silence / garbage).
          float f = -(float)v.glide_dist / 4096.0f;
          if (f > 12.0f) f = 12.0f;
          else if (f < -12.0f) f = -12.0f;
          // Split into integer + fractional: 2^f = 2^int(f) * 2^frac(f)
          const int32_t fi = (int32_t)f; // truncates toward 0
          const float frac = f - (float)fi; // ∈ (-1, 1)
          // 2^frac via Taylor: e = frac * ln2, |e| < 0.693
          const float ln2 = 0.6931471805599453f;
          const float e = frac * ln2;
          const float e2 = e * e;
          const float two_frac = 1.f + e + e2 * 0.5f + (e2 * e) * (1.f / 6.f)
                                + (e2 * e2) * (1.f / 24.f); // 4th-order
          // 2^fi: exact power-of-two from the namespace-scope kTwoPow LUT
          // (osc_constants.h). fi ∈ [-12, 12] after the clamp above, so
          // (fi + 12) is always a valid index.
          const int idx = (fi >= -12 && fi <= 12) ? (fi + 12) : ((fi > 0) ? 24 : 0);
          vw0 = v.glide_to * kTwoPow[idx] * two_frac;
        }
        else
        {
          vw0 = v.w0; // not gliding (or glide just completed → v.w0 = glide_to)
        }

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

        // ---- Sub-octave (f/2) phasor: shared LFO-FM detune -----------------
        {
          const float lfo_fm_cents = lfo_ * kSubLfoFmCents;
          v.ph_sub += vw0 * kSubW0Scale * Detune::factor(lfo_fm_cents);
          v.ph_sub -= fast_floorf(v.ph_sub);
        }
        // ---- Sub-2 (f/4) phasor: stable low-end anchor, no LFO-FM ----------
        v.ph_sub2 += vw0 * 0.25f;
        v.ph_sub2 -= fast_floorf(v.ph_sub2);

        // ---- Wavetable samples (per-voice morph shift from its own w0) ------
        const float pitch_n = (vw0 * WaveTable::kSampleRate) / WaveTable::kRefHz;
        const float shift = (pitch_n < 0.f) ? 0.f : (pitch_n > 1.f) ? WaveTable::kWaveMorphShift : (WaveTable::kWaveMorphShift * pitch_n);
        const float wav_a_pos = wave_base_a + lfo_morph + rw_dev_a_tables + shift;
        const float wav_b_pos = wave_base_b + lfo_morph + rw_dev_b_tables + shift;
        const float wav_a = WaveTable::resolve_wave(wav_a_pos, v.ph_a);
        const float wav_b = WaveTable::resolve_wave(wav_b_pos, v.ph_b);
        v.prev_wav_a = wav_a; // FM lookback cache (next sample's modulator)
        v.prev_wav_b = wav_b;

        // ---- Accumulate the common-post-stage inputs (per-voice Breath lvl) --
        // PER-VOICE ENVELOPE: each voice is gated by its OWN A-H-R envelope
        // (v.eg.env). This is what makes the polyphonic cluster work: a new
        // note (env 0→1 over 2 s) ADDS to the cluster instead of dragging the
        // whole thing through a common MAX that dips on the 1→2 note boundary.
        // (The old common env_max = max over all voices read the cluster's
        // level as its loudest note, so the instant a 2nd note started from
        // env=0 the shared gain could momentarily collapse -> "silence" on the
        // 2nd note, especially with GLIDE>0 where the new note's glide and the
        // envelope reset coincide. Per-voice gating removes that shared-dip.)
        const float eg = v.eg.env;
        sum_main += (wav_a * lvl_a + wav_b * lvl_b) * eg;
        sum_sub  += osc_sinf(v.ph_sub) * eg;
        sum_ring += (wav_a * wav_b) * eg;
        // Sub-2: per-voice Sub2Voice state (LPF + gate); level = this voice's
        // envelope (the common post-stage scales by p.sub2_level * lvl_sub).
        sum_sub2 += v.sub2.process(v.ph_sub2, vw0, eg);
      }
    }

    // A+B mix (the shared post-stage input); per-voice Breath levels are
    // already folded into sum_main.
    //
    // POLYPHONIC LOUDNESS COMPENSATION (Variant A): every summed voice signal
    // (sum_main / sum_sub / sum_sub2 / sum_ring) grows linearly with the number
    // of sounding notes, but the downstream kMix* budget (kMixOsc = 0.5, ...)
    // was designed for ONE note. Without compensation N notes add coherently
    // (worst case ~x4) and, fed through fold/saturate at the same FOLD/SATU,
    // clip and "shout" (fastertanh2f pegs at +1). So we scale each summed
    // signal by env_comp_ ~= 1/sqrt(env_sum): N=1 -> 1 (mono EXACTLY
    // unchanged), N=4 -> ~0.5 (one note's level back). This is applied BEFORE
    // the shared fold/saturator/SVF so the shared post-stage sees a
    // single-note-magnitude signal and the whole cluster reads as one DRONE
    // at the intended loudness. (env_comp_ is the smoothed 1/sqrt(N) from
    // the envelope-advance block above, so the 2 s attack/release don't pump
    // it per sample.)
    const float mix  = kMixOsc * (sum_main * env_comp_);
    // RMOD: A*B ring-mod DERIVED voice (summed per-voice), MAIN-ducked.
    const float ring_out = rmod_active ? (sum_ring * env_comp_) * p.ring_mix * p.main_level : 0.f;
    // Sub-octave / sub-2 (shared sums; scaled by their levels in the mix).
    const float sub  = sum_sub  * env_comp_;
    const float sub2 = sum_sub2 * env_comp_;

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
    // Fold kMixOsc into the saturator input (mix already carries it): the
    // shared post-stage output is kMixOsc * (folder/saturator/SVF) * main, so
    // y = kMixOsc * saturate(...). One multiply here instead of in the final mix.
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
    // it feeds the POLYPHONIC LOUDNESS COMPENSATION (env_comp_ = 1/sqrt(N),
    // a per-sample one-pole smooth below) that scales the summed voice signals
    // so N notes read at ~one note's loudness instead of N times louder.
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
    // One-pole follow the cluster loudness (1/sqrt(N)) so the makeup gain
    // moves smoothly with note count / the 2 s envelope, not per-sample.
    // Target = 1/sqrt(env_sum) with env_sum floored at 1 (a single note is
    // never attenuated; silence -> 1 = no effect on the zero signal).
    {
      const float n = (env_sum_ > 1.f) ? env_sum_ : 1.f;
      // 1/sqrt(n) via 1/sqrt(1 + (n-1)): with t = n - 1 in [0, 3], use
      //   (1 + t)^-1/2 ~= 1 - t/2 + 3 t^2/8 - 5 t^3/16 + 35 t^4/128
      // (binomial series, |t|<=3 converges and at t=3 the 5th term is ~0.02 of
      //  the value — negligible against the tanh soft-clip headroom). For
      // n <= 1 (t<=0) this is exact in the [0,1] region (t=0 -> 1).
      const float t = n - 1.f;
      const float target =
          1.f + t * (-0.5f) + (t * t) * (3.f / 8.f)
              + (t * t * t) * (-5.f / 16.f) + (t * t * t * t) * (35.f / 128.f);
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
    // any low-end movement (Breath stream on SUB1, note glides, SUB2 gate)
    // moves the main tone with it. The duck gain is floored at kDuckFloor
    // (0.7, ~-3 dB) so the main voice never disappears. The duck is ALWAYS
    // active (the follower advances every sample); the RMOD branch ducks with
    // y_filt: it is a DERIVED voice of A+B (wav_a * wav_b), so the whole
    // main-voice cluster (A+B + its FM/JIT/RMOD byproducts) sinks together --
    // the same logic the MAIN knob applies (see the RMOD comment above).
    {
      // Follower source: the (level-scaled) sub-voice sum, i.e. the low end
      // that actually reaches the mix (sub * SUB1, sub2 * SUB2).
      const float sub_src = sub * p.sub_level + sub2 * p.sub2_level;
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
      // already scaled by env_comp_ = 1/sqrt(env_sum) for polyphonic makeup).
      // A single note -> env = its own level (mono exact); N notes -> each
      // note contributes its own envelope, no shared-dip on the 1→2 boundary.
      const float raw = (y_filt * p.main_level * duck_gain
                       + sub * p.sub_level * kMixSub
                       + sub2 * p.sub2_level * kMixSub2
                       + ring_out * kMixRing * duck_gain) * kOutDrive;
      out[0] = clip1m1f(fastertanh2f(kOutSat * raw));
    }
  }
}
// K-rate per-block prepass extracted from process() (see the call site there):
// web-sim edit-parameter request consumption (K-rate atomic apply of the
// select_/PARAM_VAL pair), rndTick() (Rnd debounce + re-roll), rndMorphTick()
// (BPM-synced re-roll morph), and the pitch-glide advance. All of it runs
// once per block, before the per-sample loop, so osc_mix.h keeps its
// "pure audio path" property.
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

  // (The single-note LINEAR pitch glide was removed in the polyphonic build.
  //  Each cluster voice now glides INDEPENDENTLY (GRIMM-style exponential) in
  //  the per-sample loop of process() — see the "Per-voice GLIDE" block there.
  //  blockPrepass no longer advances any glide.)
}
