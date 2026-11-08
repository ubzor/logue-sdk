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

  // ---- Rnd debounce tick (audio clock) --------------------------------------
  // Advance the audio clock and consume any pending Rnd re-roll (immediate or
  // the final one after a 0.5 s silence window). Runs in BOTH builds: the
  // hardware Rnd slot (unit.cc) and the web simulator's *Random* button both
  // arm the debounce (rndArm / rndRequest), and this is the single place the
  // re-roll actually fires. See the Rnd debounce comment block in osc.h.
  rndTick(frames);

  // ---- Pitch glide (portamento) ---------------------------------------------
  // If a note change is in flight, advance the base phase increment LINEARLY
  // from glide_from_ toward glide_to_ and refresh every derived voice (A/B
  // detuned pair, sub-1, sub-2, chord, chord-2, wavetable morph position) so
  // the WHOLE sound slides in pitch together. Runs per sample while active so
  // the 2 s glide is sample-accurate at any block size. When it completes, w0_
  // snaps exactly to the target and the derived voices settle to their final
  // values; the glide is then disarmed so the rest of the block (and every
  // later block) uses the plain cached path with zero extra work.
  //
  // Note: this runs BEFORE the params_ / w0_ caches below, so the block picks
  // up the (possibly gliding) w0_ immediately. A re-target from setPitch()
  // (mid-glide) is picked up on the next block (K-rate -> audio thread), which
  // is the natural ~2.9 ms latency of every parameter in this unit.
  if (glide_active_)
  {
    const uint32_t elapsed = glide_elapsed_ + frames;
    if (elapsed >= kGlideSamples)
    {
      w0_ = glide_to_; // snap to the target
      recalc_pitch_derived();
      glide_active_ = false;
      glide_elapsed_ = 0;
    }
    else
    {
      glide_elapsed_ = elapsed;
      const float t = (float)elapsed / (float)kGlideSamples; // 0..1, linear
      w0_ = glide_from_ + (glide_to_ - glide_from_) * t;
      // Use the FRESH params_ for this frame: if a K-rate edit (CINT / CINT2 /
      // DETUNE / WAV*) landed in the same block it already updated params_ on
      // the worklet thread before this sample, so reading it here (not the
      // stale `p` cache built below) avoids a one-frame stale derived voice.
      {
        const Params p = params_;
        const float w = w0_;
        update_w0_det();
        chord_.update(w, p.chord_cents);
        chord2_.update(w, p.chord2_cents);
        update_w0_sub();
        update_w0_sub2();
        waves_.update(w);
      }
    }
  }

#ifdef __EMSCRIPTEN__
  // Consume any pending direct edit-parameter request from the UI thread
  // (see simEditRequest). The (select_, PARAM_VAL) pair is applied
  // atomically here so the DSP never sees an intermediate select_ value --
  // the hardware's unit_set_param_value() path does the same thing (K-rate,
  // one knob turn = one apply).
  {
    const uint32_t req = sim_edit_request_;
    if (req & (1U << 31))
    {
      const uint8_t idx = (uint8_t)(req & 0x3FU);
      const int32_t raw = (int32_t)((req >> 8) & 0x3FFU);
      if (idx < kEditParamCount)
      {
        edit_value_[idx] = raw;
        select_ = idx;
        apply_edit_param(kEditParamFirst + idx, raw);
      }
      sim_edit_request_ = 0;
    }
  }
#endif

  // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
  const Params p = params_;

  // base phase increment (the raw note-rate phasor advance; tape wow/flutter
  // were removed, so there is no per-sample pitch-wander multiplier here).
  const float w0 = w0_;

  // ---- Random-walk detune deviation (target-based wandering, dual channel) ----
  // When rw_amount == 0 (knob at 0) the walk is disabled and w0_a / w0_b are
  // the cached static values (regression: byte-for-byte identical to the old
  // code). rw_amount is the quadratic-mapped knob value (see setParameter),
  // so rw_active is true for any non-zero knob position.
  //
  // The walk state (rw_.dev_a/b) is NORMALIZED to [-1, +1]; RATE AMT (the
  // quadratic-mapped rw_amount) is the depth ceiling, and the audio path
  // scales it to cents (kRwDetuneScaleCents = +/-10 cents) so neither voice
  // strays further from its DETUNE knob value than AMT allows. The two
  // channels wander with INDEPENDENT random targets (chosen on the same
  // period), so the inter-voice beat frequency shifts over time — an
  // asymmetric, ever-changing chorus.
  //
  // The deviations smoothly track random targets (see RandomWalk::advance)
  // that are regenerated every `interval` samples. RATE SPD sets that period
  // (0.1 s .. 10 s), RATE SMTH sets the tracking time constant (10 ms ..
  // 1.5 s); keeping them independent makes the audible depth stable
  // regardless of SPEED.
  // (The A-H-R envelope is advanced per-sample inside the loop below; the
  // relaxation coefficients are cached once per block for cheap access.)
  const float alpha_a = eg_alpha_a_;
  const float alpha_r = eg_alpha_r_;

  // ---- Second sub (SUB2) level (see osc_sub2.h for the voice DSP) ----
  const float sub2_level = p.sub2_level;

  // ---- Chord voice gain (precomputed once per block) ----
  // Linear gain: chord_gain = p.chord_level (0..1), like SUB1/SUB2. At lvl=0
  // the voice is silent. At lvl=1 (CHRD=100%) the voice reaches full
  // amplitude and is then shaped by its own folder+saturator (see the CHORD
  // branch in the final mix) before being scaled by kMixChordDirect.
  //
  // (Previously this used the normalized kMixChord*lvl/(1+lvl) form so the
  // chord could not push the shared A+B saturator into clipping — that
  // constraint no longer applies now that the chord has its OWN saturator
  // pass, and the old double-normalization was the root cause of the chord
  // reading far too quiet. See osc_chord.h for the kMixChord constant, now
  // unused by the audio path.)
  const float chord_gain = p.chord_level;

  const bool rw_active = p.rw_amount > 0.f;
  // A and B run at the base note ± the static DETU knob (symmetric ±c/2,
  // cached in w0_a_det_/w0_b_det_ at K-rate), plus the random-walk detune
  // (asymmetric, per-sample) when rw_active. When the walk is off, the
  // cached values are used directly (0 Taylor cost per sample).
  float w0_a = w0_a_det_; // static symmetric detune (osc A, +c/2)
  float w0_b = w0_b_det_; // static symmetric detune (osc B, -c/2)

  // wavetable morph: base (knob) + pitch-shift (K-rate cached), the LFO
  // offset is added per-sample below. waves_.wave_shift_ is recomputed on
  // setParameter(WAV1/WAV2/WAV3) / setPitch(); wave_a / wave_b / wave_c are
  // the knobs (the chord voice now has its own table, see WAV3).
  const float wave_base_a = p.wave_a + waves_.wave_shift_;
  const float wave_base_b = p.wave_b + waves_.wave_shift_;
  const float wave_base_c = p.wave_c + waves_.wave_shift_;
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
  // ---- CINT2: per-block cache (the per-sample chord2 advance is in the loop) ----
  // chord2_gain mirrors chord_gain (the CINT2 voice shares the CHRD level).
  // Used to gate the second chord's resolve_wave lookup (skip when silent).
  const float chord2_gain = p.chord_level;
  // SVF: the internal 12 dB/oct low-pass is engaged when the dry/wet (svf_wet)
  // is non-zero. When svf_wet == 0 (the default) the filter is transparent
  // and its state update is skipped (regression-safe). The walk-modified
  // cutoff coefficient (svf_.cc_w) is set per-sample below when active.
  const float svf_wet = p.svf_wet;
  const bool svf_active = svf_wet > 0.f;

  for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
  {
    // update random-walk deviations (advance once per sample when active).
    // The RATE AMT knob is the DEPTH CEILING of the whole walk: it scales
    // every random-walk modulation (detune below; wavetable morph / drive /
    // fold further down). The normalized deviations (rw_.dev_a/b, in [-1,+1])
    // are multiplied by p.rw_amount (0..1, quadratic-mapped from the knob)
    // at each point of use, so RATE = 0 means no wandering at all and
    // RATE = 100% gives full scale. Without this factor the knobs would be
    // binary (any non-zero RATE gave full-depth walk) — see the bug this
    // fixes (rw_amount was only used as an on/off gate, never as a scale).
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
      // recompute w0_a / w0_b from the combined (static + walk) per-voice
      // detune: A gets +c/2 (static) + rw_dev_a (walk), B gets -c/2 (static)
      // + rw_dev_b (walk). 4th-order Taylor via Detune::w0_detuned
      // (see osc_detune.h).
      const float half_det = 0.5f * p.detune_cents;
      w0_a = Detune::w0_detuned(w0, +half_det + rw_dev_a_cents);
      w0_b = Detune::w0_detuned(w0, -half_det + rw_dev_b_cents);
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
    float lvl_c = 1.f;
    float lvl_sub = 1.f;
    if (rw_active && p.rw_lvl > 0.f)
    {
      rw_[3].advance(p.rw_amount, osc_white); // ceiling for advance() stays quadratic (unchanged)
      const float swing = p.rw_lvl * p.rw_amount_lin * 2.f; // 0..2 — full depth at RATE=100% & RVLL=100%
      lvl_a   = 1.f + swing * rw_[3].dev_a;
      lvl_b   = 1.f + swing * rw_[3].dev_b;
      lvl_c   = 1.f + swing * rw_[3].dev_c;
      lvl_sub = 1.f + swing * rw_[3].dev_d;
      if (lvl_a < 0.f) lvl_a = 0.f; else if (lvl_a > 1.f) lvl_a = 1.f;
      if (lvl_b < 0.f) lvl_b = 0.f; else if (lvl_b > 1.f) lvl_b = 1.f;
      if (lvl_c < 0.f) lvl_c = 0.f; else if (lvl_c > 1.f) lvl_c = 1.f;
      if (lvl_sub < 0.f) lvl_sub = 0.f; else if (lvl_sub > 1.f) lvl_sub = 1.f;
    }

    // phasor update (plus optional phase-FM and JIT)
    // FM: each osc's phase is modulated by the OTHER osc's
    // wavetable output, scaled by a CYCLES-based index. The modulation
    // index (peak phase deviation, in cycles) is
    //   (other_wavetable_out) * eff_index
    // where eff_index = static (fm_a_static) + walk (dev * rw_amt * wander_depth).
    // The wavetable output is in [-1, +1]; the index is bounded to
    // +/-kFmMaxCents/2 cycles. The raw index is then passed through a
    // one-pole low-pass (kFmSmoothAlpha) that softens the fast part of
    // the modulation without touching the carrier itself.
    // This is frequency-INDEPENDENT: the same FM knob position gives the
    // same sideband spread on a 50 Hz drone and on a 2 kHz note.
    // At fm=0 & rfm=0 this adds 0. (The wavetable values are read AFTER the
    // phasor wrap below, so the modulated phasor and the wavetable sample
    // are from the same instant.)
    //
    // NOTE: we read the *previous* sample's wavetable output for the
    // modulation (a one-sample lookback) to avoid a data-dependency on the
    // current phasor (which we are about to advance). For a drone this is
    // inaudibly different and keeps the audio path branch-light.
    // We use the cached previous wavetable values (prev_wav_a_ / prev_wav_b_)
    // updated at the end of each iteration.
    {
      // ---- Phase-FM (FM1/FM2): static index + RFM walk "breathing" ----
      // eff_index = static (fm_a_static) + walk (dev * rw_amt * wander_depth).
      // The static part is ALWAYS present (no RATE gate) — turning FM on gives
      // constant sidebands immediately. The walk part (RFM) is RATE-gated and
      // adds a slow "breathing" on top, like RDRV on SATU. The combined index
      // is then scaled by the modulator's wavetable output (prev_wav * 0.5 is
      // already folded into the static/wander depths above, so here we
      // multiply by the full prev_wav in [-1,1] to get the per-sample index).
      // One-pole (kFmSmoothAlpha) tames the fast sign-flips; the state lives in
      // fm_smooth_a_/b_. At fm=0 & rfm=0 the
      // eff_index is 0 and the LPF relaxes to 0 (regression-safe).
      const float fm_a_wander = fm_a_wander ? (rw_[0].dev_b * rw_amt * fm_a_wander_depth) : 0.f;
      const float fm_b_wander = fm_b_wander ? (rw_[0].dev_a * rw_amt * fm_b_wander_depth) : 0.f;
      const float fm_a_eff = fm_a_static + fm_a_wander;  // effective index (cycles)
      const float fm_b_eff = fm_b_static + fm_b_wander;  // effective index (cycles)
      const float fm_a_raw = prev_wav_b_ * fm_a_eff;  // modulator ([-1,1]) * index
      const float fm_b_raw = prev_wav_a_ * fm_b_eff;  // modulator ([-1,1]) * index
      fm_smooth_a_ += kFmSmoothAlpha * (fm_a_raw - fm_smooth_a_);
      fm_smooth_b_ += kFmSmoothAlpha * (fm_b_raw - fm_smooth_b_);
      // ---- JIT: random phase jitter on A/B (per-sample white() draws) ----
      // (osc_white() - 0.5) * jit_cycles is a uniform offset in
      // [-jit/2, +jit/2] cycles, "smearing" the carrier into a dense partial.
      // At jit_cycles = 0 (default) both offsets are 0 (regression-safe).
      // Each voice draws its OWN white() so A and B are not phase-locked.
      const float jit_a = jit_active ? (osc_white() - 0.5f) * p.jit_cycles : 0.f;
      const float jit_b = jit_active ? (osc_white() - 0.5f) * p.jit_cycles : 0.f;
      // Combined per-sample phase increment: base detuned w0 + FM (phase mod,
      // smoothed) + JIT (random wobble). The floorf wrap below (modular,
      // x - floorf(x)) handles the signed FM/JIT offsets, so the phasor stays
      // in [0, 1) for any finite input (no negative-LUT-UB, see the wrap
      // comment below).
      phasor_a_ += w0_a + fm_smooth_a_ + jit_a;
      phasor_b_ += w0_b + fm_smooth_b_ + jit_b;
    }
    // Chord phasor: advances at w0 * kScale[interval] (see osc_chord.h).
    // No random-walk modulation — the chord voice is a stable "anchor"
    // like SUB2, not a beating voice. advance() returns false if the
    // interval is OFF (no oscillator).
    const bool chord_active = chord_.advance();
    // Second chord phasor (CINT2): same stable-anchor behavior as chord_.
    // advance() returns false if the second interval is 0 (OFF / unison).
    // chord2_gain mirrors chord_gain (the CINT2 voice shares the CHRD level);
    // it is used to gate the resolve_wave lookup below (skip when silent).
    const bool chord2_active = chord2_.advance();

    // sub-octave phasor: base rate is w0_sub_ (f/2 * 2^(sub_detune/1200)),
    // modulated by the platform shape-LFO (±kSubLfoFmCents around w0_sub_).
    // The LFO is applied as a per-sample detune via Detune::factor (4th-order
    // Taylor, see osc_detune.h) — the LFO value is small (±5 cents), so the
    // series is more than accurate enough.
    {
      const float lfo_fm_cents = lfo_ * kSubLfoFmCents;
      phasor_sub_ += w0_sub_ * Detune::factor(lfo_fm_cents);
    }

    // second sub phasor: pure f/4, no LFO-FM and no fixed detune (it is a
    // stable low-end anchor, not a beating voice). Advances exactly at
    // w0/4 (w0_sub2_ is cached in update_w0_sub2()).
    phasor_sub2_ += w0_sub2_;

    // wrap phasors into [0, 1).
    //
    // Use a modular floor (x - floorf(x)) instead of a single subtract.
    // The single-subtract form (if (p >= 1) p -= 1) only handles the
    // case where the per-sample increment is in (0, 1) and the phasor
    // starts in [0, 1). FM (FM_A / FM_B) adds a signed per-sample
    // phase offset of up to +/-kFmMaxCents/2 cycles (see the FM branch
    // above: prev_wav * eff_index), so when the previous
    // sample's wavetable output has the opposite sign to the index
    // the combined increment (w0_a + fm_a_idx) can be NEGATIVE,
    // pushing phasor_a_ below 0. The single subtract never corrects a
    // negative phasor, so it would feed a value < 0 into osc_wave_scanf()
    // whose `(uint32_t)x` cast is undefined behavior for negative floats
    // (out-of-bounds LUT read -> garbage/NaN -> NaN-lock in the SVF /
    // FM feedback loop). floorf normalizes to [0, 1) for ANY finite
    // input (positive, negative, or > 1), so the LUT is always queried
    // with a valid phase. Cost: one fast_floorf per phasor per sample (4 total);
    // it is a no-op when the phasor is already in [0, 1) (the common case at
    // FM = 0), so the stock sound is bit-identical. fast_floorf() (not libm
    // floorf) keeps the unit free of unresolved libm dynamic symbols.
    phasor_a_   -= fast_floorf(phasor_a_);
    phasor_b_   -= fast_floorf(phasor_b_);
    phasor_sub_ -= fast_floorf(phasor_sub_);
    phasor_sub2_-= fast_floorf(phasor_sub2_);

    // Simulator-only: synthesize the shape-LFO at audio rate (phase-coherent
    // sine). On hardware lfo_ is delivered by the platform via setShapeLfo()
    // and this line is compiled out, so the two paths share the same
    // morph code below. See osc_lfo_sim.h for the synthesizer.
#ifdef EMSCRIPTEN
    lfo_ = lfo_sim_.process();
#endif

    // generate wavetable samples in -1..1. Each voice's table index is the
    // base position (knob + pitch-shift) plus the LFO morph offset, so the
    // timbre "breathes" across the wavetable. WaveTable::resolve_wave() crossfades the
    // fractional part between adjacent tables, so the morph glides smoothly.
    const float lfo_morph = lfo_ * WaveTable::kLfoMorphDepth;
    // Wavetable scan positions: base (WAV1/WAV2 knob) + LFO morph (platform
    // shape-LFO) + random-walk morph (RW_WAV knob, via rw_dev_a/b_tables).
    // The walk morph is added so each voice wanders through the 91 Korg
    // wavetables around its knob position — an unpredictable, organic
    // timbre drift that the static LFO sweep alone cannot produce.
    const float wav_a_pos = wave_base_a + lfo_morph + rw_dev_a_tables;
    const float wav_b_pos = wave_base_b + lfo_morph + rw_dev_b_tables;
    const float wav_a = WaveTable::resolve_wave(wav_a_pos, phasor_a_);
    const float wav_b = WaveTable::resolve_wave(wav_b_pos, phasor_b_);
    // Cache this sample's wavetable outputs for the NEXT sample's FM
    // modulation (one-sample lookback, see the FM branch in the phasor
    // update above). When FM is off these writes are the only cost.
    prev_wav_a_ = wav_a;
    prev_wav_b_ = wav_b;

    // Chord voice: third oscillator at a selectable interval above the base
    // (CINT knob: OFF/5TH/OCT/O+5/M3). Uses the WAV1 (osc A) wavetable so
    // it blends with the main voice. Computed only when both the interval
    // is active (chord_active) and the level is non-zero (chord_gain > 0)
    // to keep the default path (CHORD = 0 or CINT = OFF) free of the extra
    // wavetable lookup. See osc_chord.h for the interval table.
    const float wav_c = (chord_active && chord_gain > 0.f)
        ? WaveTable::resolve_wave(wave_base_c + lfo_morph, chord_.phasor)
        : 0.f;
    // Second chord voice (CINT2): same wavetable (WAV3) + LFO morph as the
    // first chord, but at a different interval (chord2_.phasor). Gated by
    // chord2_active (interval > 0) AND chord2_gain > 0 (CHRD > 0) to keep the
    // default path (CINT2 = 0 or CHRD = 0) free of the extra lookup. Both
    // intervals share the CHRD level (chord_gain) and lvl_c (Breath stream
    // dev_c) in the final mix, so they breathe in sync. Each interval then
    // runs its OWN folder+saturator (CINT: CHSAT/CHFLD, CINT2: CHSAT2/CHFLD2).
    const float wav_c2 = (chord2_active && chord2_gain > 0.f)
        ? WaveTable::resolve_wave(wave_base_c + lfo_morph, chord2_.phasor)
        : 0.f;

    // sub-octave sine in -1..1 (LUT lookup, ~4 VFP ops)
    const float sub = osc_sinf(phasor_sub_);

    // ---- second sub (f/4): clean sine -> very soft saturator -> LPF -> gate ----
    // Only computed when the voice is actually audible (level > 0) to keep
    // the default path (SUB2 = 0) free of this cost.
    // See osc_sub2.h for the DSP details (sine -> soft saturator -> LPF -> gate).
    const float sub2 = sub2_.process(phasor_sub2_, w0, sub2_level);

    // mix the two main voices (A+B) equally, then run through the pre-drive
    // wavefolder (FOLD knob) and the asymmetric soft-clip saturator
    // (SATU/ASYM knobs). See osc_saturator.h for the DSP details.
    //
    // The chord voice is NOT in this mix: it is mixed in parallel in the
    // final mix below, through its OWN folder+saturator pass (same SATU/ASYM/
    // FOLD knobs), so the MAIN level knob can duck only the A+B main tone —
    // MAIN=0 silences A+B while CHORD (and the subs) keep sounding. Because
    // the chord has its own drive, it keeps the unit's timbre "color" (the
    // old double-normalized bypass path read far too quiet and "dry").
    const float mix = kMixOsc * (wav_a * lvl_a + wav_b * lvl_b);
    // (The static DETU knob is applied per-voice above, via w0_a / w0_b —
    //  not post-mix: A and B are detuned in opposite directions around the
    //  base note, producing a per-voice phase drift that sounds ~4x
    //  stronger than an equivalent post-mix sum detune.)
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
    const float folded = Saturator::fold(mix, eff_fold);
    // ---- Enrichment-2: second (cascaded) wavefolder (FOLD2) ----
    // Run the first fold's output through a SECOND Saturator::fold with the
    // FOLD2 depth, so full FOLD2 doubles the folding into a denser "worn"
    // timbre (even more even harmonics / a metallic edge). At FOLD2 = 0 the
    // second fold is a no-op (regression-safe).
    const float folded2 = (p.fold2_depth > 0.f)
        ? Saturator::fold(folded, p.fold2_depth)
        : folded;
    const float y = Saturator::saturate(folded2, eff_drive_clamped, p.asym);

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
    // ---- advance the A-H-R envelope by one sample ----
    // (per-sample so the 2 s phases are sample-accurate at any block size)
    eg_.advance(alpha_a, alpha_r);

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
    // Each chord interval through its OWN folder + saturator (stateless — same
    // functions as the A+B path, but driven by the interval's OWN knobs: CINT
    // uses CHSAT/CHFLD, CINT2 uses CHSAT2/CHFLD2, so both are independent of
    // the A+B SATU/FOLD and of each other).
    // chord_gain is linear (p.chord_level, 0..1); lvl_c is the per-sample
    // LFO level modulation. ASYM is SHARED (p.asym, same as A+B) per design.
    //
    // The chord's drive/fold are clamped exactly like the A+B path (no
    // random-walk modulation — RW_DRV/RW_FLD affect only the A+B voice).
    float chord_drive_clamped = p.chord_drive;
    if (chord_drive_clamped < 0.f) chord_drive_clamped = 0.f;
    else if (chord_drive_clamped > kSatuMax) chord_drive_clamped = kSatuMax;
    float chord_fold_clamped = p.chord_fold;
    if (chord_fold_clamped < 0.f) chord_fold_clamped = 0.f;
    else if (chord_fold_clamped > 1.f) chord_fold_clamped = 1.f;
    // CINT2's OWN drive/fold (CHSAT2/CHFLD2), clamped exactly like the CINT
    // voice's. At the defaults they equal chord_drive_clamped / chord_fold_clamped,
    // so the CINT2 branch reduces to the CINT branch's gain structure.
    float chord2_drive_clamped = p.chord2_drive;
    if (chord2_drive_clamped < 0.f) chord2_drive_clamped = 0.f;
    else if (chord2_drive_clamped > kSatuMax) chord2_drive_clamped = kSatuMax;
    float chord2_fold_clamped = p.chord2_fold;
    if (chord2_fold_clamped < 0.f) chord2_fold_clamped = 0.f;
    else if (chord2_fold_clamped > 1.f) chord2_fold_clamped = 1.f;
    // The two chord intervals each run their OWN folder+saturator pass now:
    // CINT through (CHSAT/CHFLD) and CINT2 through (CHSAT2/CHFLD2). They still
    // share the CHRD level (chord_gain) and the Breath stream's lvl_c (so they
    // breathe in sync — the chord is one object in level). ASYM is shared with
    // everything. Summing AFTER the saturator means each interval is driven at
    // its OWN level, so the higher interval (CINT2) can be softened/filtered
    // independently of the lower one. At CINT2 = 0 (default off) or CHRD = 0,
    // wav_c2 = 0 and the CINT2 branch is 0 (regression-safe).
    const float c1 = Saturator::saturate(
        Saturator::fold(wav_c * chord_gain * lvl_c, chord_fold_clamped),
        chord_drive_clamped, p.asym);
    const float c2 = (chord2_gain > 0.f)
        ? Saturator::saturate(
              Saturator::fold(wav_c2 * chord_gain * lvl_c, chord2_fold_clamped),
              chord2_drive_clamped, p.asym)
        : 0.f;
    const float chord_sat = c1 + c2;
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
    const float ring_out = rmod_active ? (wav_a * wav_b) * p.ring_mix * p.main_level : 0.f;
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
    // ---- Enrichment-2: envelope-follower self-ducking (DCK) -----------------
    // When DCK (duck_amt) > 0, a one-pole abs-peak follower tracks the main
    // A+B output (|y_filt|); the follower then DUCKS the two sub voices (SUB1 /
    // SUB2) so the low end "breathes out" of the way of the main voice's peaks
    // on dense settings (heavy SATU/FOLD). The duck gain is floored at
    // kDuckFloor (0.2) so the subs are never fully silenced. At DCK = 0
    // (default) duck_ stays 1.0 and the follower never advances (regression-safe).
    if (p.duck_amt > 0.f)
    {
      const float inst = (y_filt < 0.f) ? -y_filt : y_filt;
      env_follower_ += (inst - env_follower_) * duck_alpha_;
      duck_ = 1.f - p.duck_amt * env_follower_ * kDuckScale;
      if (duck_ < kDuckFloor) duck_ = kDuckFloor;
      if (duck_ > 1.f) duck_ = 1.f;
    }
    else
    {
      duck_ = 1.f; // DCK off: no ducking (regression-safe)
    }
    // Final output (with duck_ applied to the sub branches).
    const float raw = (y_filt * kMixOsc * p.main_level
                     + chord_sat * kMixChordDirect
                     + sub * p.sub_level * lvl_sub * kMixSub * duck_
                     + sub2 * sub2_level * kMixSub2 * duck_
                     + ring_out * kMixRing) * eg_.env * kOutDrive;
    out[0] = clip1m1f(fastertanh2f(kOutSat * raw));
  }
}

