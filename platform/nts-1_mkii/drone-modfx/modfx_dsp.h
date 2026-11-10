#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: modfx_dsp.h
 *
 *  DRONE-MOD: the per-sample audio path (Modfx::modfx_process_impl). #included
 *  at the bottom of modfx.h (after the class is complete).
 *
 *  Signal path (stereo, interleaved): in (mono from OSC) -> upmix(L,R)
 *    -> ENSE  (stereo chorus: 2 detuned voices, modulated one-pole delays)
 *    -> TREMO (harmonic/optical tremolo: LPF/HPF split, antiphase LFO)
 *    -> FLNG  (thru-zero flanger: HPF, modulated short delay + feedback)
 *    -> TAPE  (wow: LCG random-walk delay; flutter: LCG noise level mod)
 *    -> FORM  (formant F1+F2 one-pole IIR combs, morph by FORM, wet FLVL)
 *    -> COMB  (IIR comb resonator, fixed delay, feedback COMB)
 *    -> makeup gain -> out
 *
 *  Every level stage is a dry/wet 0..1 blend: at 0 the stage is a pure wire
 *  (regression-safe). DRIFT (the shared self-motion ceiling, like RATE in the
 *  oscillator) wanders those levels on their own, but only when DRIFT > 0.
 *
 *  libm discipline (check-und): NO transcendental calls (no sinf/cosf/expf/
 *  powf/sqrtf/logf). Sines are a 4th-order Taylor of the phase accumulator;
 *  one-pole alphas use modfx_one_minus_expf (Taylor); the LCG is pure integer.
 */

#include "modfx_constants.h"

// Advance one LFO phase accumulator by (2*pi*freq/sr) and return its sine
// (4th-order Taylor; no libm).
static inline float modfx_lfo_step(float &phase, float freq)
{
  const float step = 6.283185307179586f * freq / modfx_consts::kSr; // 2*pi*freq/sr
  phase += step;
  if (phase >= 1.f) phase -= 1.f;  // wrap to [0,1) (fast modular, no floorf)
  const float ang = 6.283185307179586f * phase; // radians (0..2*pi)
  const float x2 = ang * ang;
  return ang * (1.f - x2 / 6.f + x2 * x2 / 120.f); // Taylor sin
}

// One-pole low-pass (alpha in [0,1]) on a running state. No libm.
static inline float modfx_onelpf(float &state, float x, float alpha)
{
  state += alpha * (x - state);
  return state;
}

// One-pole high-pass (alpha in [0,1]) on a running state. No libm.
static inline float modfx_onehpf(float &state, float x, float alpha)
{
  state += alpha * (x - state);
  return x - state;
}

// The per-sample audio path. Declared `inline` so that the header can be
// #included by multiple translation units (the web-simulator's wasm.cc and
// chain.cc both pull in modfx.h through different absolute include paths,
// which defeats #pragma once and would otherwise produce a duplicate-symbol
// link error). On the ARM unit only one TU (unit.cc) includes modfx.h, so
// `inline` is harmless there (it just permits, not requires, multiple defs).
inline void Modfx::modfx_process_impl(const float *__restrict in, float *__restrict out, uint32_t frames)
{
  const Params p = params_;
  const float sr = modfx_consts::kSr;

  // ---- K-rate LFO frequencies (from TIME; Taylor exp, no libm) --------------
  const float t = p.time;
  const float ense_freq = modfx_consts::kEnseLFOMin * modfx_expf(modfx_consts::kEnseLFOlnR * t);
  const float tremo_freq = modfx_consts::kTremoLFOMin * modfx_expf(modfx_consts::kTremoLFOlnR * t);
  const float flng_freq = modfx_consts::kFlngLFOMin * modfx_expf(modfx_consts::kFlngLFOlnR * t);

  // ---- K-rate one-pole alphas / feedbacks -----------------------------------
  const float tremo_alpha = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kTremoSplitHz / sr);
  const float flng_hpf_alpha = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kFlngHPFHz / sr);
  float flng_fb = p.flng > 0.f ? (p.depth * modfx_consts::kFlngFbMax) : 0.f;
  if (flng_fb > 0.85f) flng_fb = 0.85f;

  // Flanger delay (samples): DEPTH^2 * kFlngMaxDepthMs, clamped, +1 offset.
  const float d2 = p.depth * p.depth;
  float flng_delay_ms = d2 * modfx_consts::kFlngMaxDepthMs;
  if (flng_delay_ms < modfx_consts::kFlngDelayMinMs) flng_delay_ms = modfx_consts::kFlngDelayMinMs;
  if (flng_delay_ms > modfx_consts::kFlngDelayMaxMs) flng_delay_ms = modfx_consts::kFlngDelayMaxMs;
  const float flng_delay_smp = flng_delay_ms * 0.001f * sr + 1.f;

  // Chorus delay spread (samples, from DEPTH) + base delays.
  const float ense_spread_smp = p.depth * modfx_consts::kEnseSpreadSmp;
  const float ense_base1_smp = modfx_consts::kEnseDelayBaseMs * 0.001f * sr;
  const float ense_base2_smp = modfx_consts::kEnseDelay2Ms * 0.001f * sr;

  // Tape wow delay (samples) + flutter amount.
  const float tape_wow_smp = modfx_consts::kTapeWowDelaySmp;
  const float tape_flutter_amt = p.depth * modfx_consts::kTapeFlutterAmt;

  // ---- Per-sample path ------------------------------------------------------
  for (uint32_t n = 0; n < frames; ++n)
  {
    const float in_mono = in[n]; // `in` is the mono OSC output (frames samples)

    // ---- ENSE: stereo chorus (2 detuned voices, modulated one-pole delays) --
    float ense_lfo = modfx_lfo_step(ense_lfo_phase_, ense_freq);
    // Wet amounts are NON-const: the DRIFT random-walk nudges them below
    // (ense_wet += drift_cur_[s], etc.), and the clamp pass rewrites them.
    float ense_wet = p.ense;
    float tre_wet = p.tremo;
    float fln_wet = p.flng;
    float tap_wet = p.tape;
    float flv_wet = p.flvl;
    float cmb_wet = p.comb;
    if (p.drift > 0.f)
  {
    const float d = p.drift; // 0..1 (shared self-motion ceiling)
    // K-rate DRIFT random-walk: each of the 7 level stages gets a target
    // offset within [manual * (1 - d), manual * (1 + d)]. Targets are set on
    // an exponential schedule (gaps 2..12 s) and tracked with a one-pole
    // (5..20 s), so the levels drift slowly and smoothly. An L1 budget
    // (kDriftBudget) clamps the SUM of the per-stage drift amplitudes so the
    // wandering "percolates" between layers instead of all seven moving at
    // once. At d == 0 the offsets are zero (static, regression-safe). The LCG
    // provides the per-step randomness (pure integer, no libm).
    const float kDriftWalk = (1.f - modfx_one_minus_expf(-1.f / 5.f)); // ~0.18 /sample
    for (int s = 0; s < kDriftCount; ++s)
    {
      lcg_state_ = (modfx_consts::kLcgA * lcg_state_) & 0x7fffffffu;
      if (lcg_state_ == 0u) lcg_state_ = modfx_consts::kLcgSeed;
      const float rnd = static_cast<float>(lcg_state_) * (2.f / 2147483647.f); // 0..1
      if (drift_next_gap_[s]-- == 0U)
      {
        // New target: a random offset in [-d, +d] (relative to the manual value).
        drift_target_[s] = (rnd * 2.f - 1.f) * d;
        // Exponential target gap: mean 2..12 s -> in samples at sr.
        const float gap_mean = modfx_consts::kDriftGapMinSec +
                               (modfx_consts::kDriftGapMaxSec - modfx_consts::kDriftGapMinSec) *
                                   (static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f + 1.f) * 0.5f;
        const float gap_smp = gap_mean * sr;
        drift_next_gap_[s] = (gap_smp > 1.f) ? (uint32_t)(gap_smp) : 1U;
        drift_tau_[s] = modfx_consts::kDriftWalkTauMin +
                        (modfx_consts::kDriftWalkTauMax - modfx_consts::kDriftWalkTauMin) *
                            (static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f + 1.f) * 0.5f;
      }
      // One-pole track toward the target (per-sample alpha from tau).
      const float alpha = 1.f - modfx_one_minus_expf(-1.f / (drift_tau_[s] * sr));
      drift_cur_[s] += alpha * (drift_target_[s] - drift_cur_[s]);
    }
    // L1 budget: clamp the sum of the 7 drift amplitudes to kDriftBudget.
    float sum = 0.f;
    for (int s = 0; s < kDriftCount; ++s)
      sum += (drift_cur_[s] > 0.f) ? drift_cur_[s] : -drift_cur_[s];
    if (sum > modfx_consts::kDriftBudget && sum > 0.f)
    {
      const float scale = modfx_consts::kDriftBudget / sum;
      for (int s = 0; s < kDriftCount; ++s)
        drift_cur_[s] *= scale;
    }
    // Apply the per-stage offsets to the wet amounts.
    ense_wet += drift_cur_[0];
    tre_wet += drift_cur_[1];
    fln_wet += drift_cur_[2];
    tap_wet += drift_cur_[3];
    flv_wet += drift_cur_[4];
    cmb_wet += drift_cur_[5];
    // (DRIFT itself is the 7th "stage" but it is the ceiling, not a level;
    //  the 7 levels are ENSE/TREMO/FLNG/TAPE/FORM(via flv)/FLVL/COMB.)
  }
  if (ense_wet < 0.f) ense_wet = 0.f; else if (ense_wet > 1.f) ense_wet = 1.f;
  if (tre_wet < 0.f) tre_wet = 0.f; else if (tre_wet > 1.f) tre_wet = 1.f;
  if (fln_wet < 0.f) fln_wet = 0.f; else if (fln_wet > 1.f) fln_wet = 1.f;
  if (tap_wet < 0.f) tap_wet = 0.f; else if (tap_wet > 1.f) tap_wet = 1.f;
  if (flv_wet < 0.f) flv_wet = 0.f; else if (flv_wet > 1.f) flv_wet = 1.f;
  if (cmb_wet < 0.f) cmb_wet = 0.f; else if (cmb_wet > 1.f) cmb_wet = 1.f;
    {
      const float spread1 = ense_spread_smp * (0.5f + 0.5f * ense_lfo);
      const float len1 = ense_base1_smp + spread1;
      const float a1 = 1.f / (1.f + len1 * 0.02f); // one-pole length -> alpha
      const float v1 = modfx_onelpf(tremo_low_, in_mono, a1);
      const float spread2 = ense_spread_smp * (0.5f - 0.5f * ense_lfo);
      const float len2 = ense_base2_smp + spread2;
      const float a2 = 1.f / (1.f + len2 * 0.02f);
      float v2 = modfx_onelpf(tremo_high_, in_mono, a2);
      v2 *= modfx_consts::kEnseVoice2; // voice 2 "pitch" (detune stand-in)
      tremo_low_ = in_mono + ense_wet * v1;   // L (scratch carrier)
      tremo_high_ = in_mono + ense_wet * v2;  // R (scratch carrier)
    }
    float L = tremo_low_;
    float R = tremo_high_;

    // ---- TREMO: harmonic/optical (LPF/HPF split, antiphase LFO) -------------
    {
      const float tr = modfx_lfo_step(tremo_lfo_phase_, tremo_freq);
      const float amtd = tre_wet * modfx_consts::kTremoDepthScale * (0.5f + 0.5f * tr);
      const float amtd2 = tre_wet * modfx_consts::kTremoDepthScale * (0.5f - 0.5f * tr);
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        float &st = (ch == 0) ? tremo_low_ : tremo_high_;
        const float low = modfx_onelpf(st, x, tremo_alpha); // LPF band
        const float high = x - low;                         // HPF residual
        float lowg = 1.f - amtd;
        if (lowg < modfx_consts::kTremoLowFloor) lowg = modfx_consts::kTremoLowFloor;
        const float highg = 1.f - amtd2;
        const float y = low * lowg + high * highg;
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- FLNG: thru-zero flanger (HPF + modulated short delay + feedback) ---
    if (fln_wet > 0.f)
    {
      const float fl_lfo = modfx_lfo_step(flng_lfo_phase_, flng_freq * 3.f); // x3 rate
      const float mod_len = flng_delay_smp * (1.f + 0.5f * fl_lfo);
      const float a = 1.f / (1.f + mod_len * 0.05f); // one-pole length -> alpha
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        float &st = (ch == 0) ? tremo_low_ : tremo_high_;
        const float hp = modfx_onehpf(st, x, flng_hpf_alpha); // first-order HPF
        const float delayed = modfx_onelpf(st, hp, a);        // modulated short delay
        const float wet = hp + delayed * flng_fb;             // + feedback tap
        const float y = x + fln_wet * (wet - x);              // dry/wet (0 = wire)
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- TAPE: wow (LCG random-walk delay) + flutter (LCG noise level mod) --
    if (tap_wet > 0.f)
    {
      lcg_state_ = (modfx_consts::kLcgA * lcg_state_) & 0x7fffffffu;
      if (lcg_state_ == 0u) lcg_state_ = modfx_consts::kLcgSeed;
      const float noise = static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f;
      const float flut = modfx_onelpf(tape_lpf_, noise, modfx_consts::kTapeLpfAlpha);
      const float gain = 1.f + tape_flutter_amt * flut;
      lcg_state_ = (modfx_consts::kLcgA * lcg_state_) & 0x7fffffffu;
      if (lcg_state_ == 0u) lcg_state_ = modfx_consts::kLcgSeed;
      const float walk = static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f;
      const float wow_alpha = 1.f / (1.f + tape_wow_smp * 0.02f * (1.f + 0.5f * walk));
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        float &st = (ch == 0) ? tremo_low_ : tremo_high_;
        const float wow = modfx_onelpf(st, x, wow_alpha);
        const float wet = wow * gain;
        const float y = x + tap_wet * (wet - x);
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- FORM: formant F1+F2 one-pole IIR combs (MOVED from drone-osc) ------
    if (flv_wet > 0.f && formant_buf1_ != nullptr && formant_buf2_ != nullptr)
    {
      const uint32_t mask = modfx_consts::kFormantDelayMaxSmp - 1U;
      formant_buf1_[formant_comb_w1_] = L;
      const float yd1 = formant_buf1_[(formant_comb_w1_ + (mask + 1U - formant_delay1_smp_)) & mask];
      formant_comb_w1_ = (formant_comb_w1_ + 1U) & mask;
      formant_buf2_[formant_comb_w2_] = L;
      const float yd2 = formant_buf2_[(formant_comb_w2_ + (mask + 1U - formant_delay2_smp_)) & mask];
      formant_comb_w2_ = (formant_comb_w2_ + 1U) & mask;
      const float formant_color = formant_fb_ * (yd1 + yd2); // shared feedback
      const float tint = formant_color * modfx_consts::kFormantGain * flv_wet;
      L += tint; // mono tint, applied identically to both channels
      R += tint;
    }

    // ---- COMB: IIR comb resonator (MOVED from drone-osc) --------------------
    if (cmb_wet > 0.f && comb_buf_ != nullptr)
    {
      comb_buf_[comb_w_] = L;
      const uint32_t cmask = modfx_consts::kCombDelayMaxSmp - 1U;
      const uint32_t r = (comb_w_ + (cmask + 1U - modfx_consts::kCombDelaySmp)) & cmask;
      const float yd = comb_buf_[r];
      comb_w_ = (comb_w_ + 1U) & cmask;
      const float tint = cmb_wet * yd; // IIR comb: one feedback tap
      L += tint;
      R += tint;
    }

    // ---- Makeup gain + write the stereo output ------------------------------
    out[2 * n] = L * modfx_consts::kModMakeup;
    out[2 * n + 1] = R * modfx_consts::kModMakeup;
  }
}
