#pragma once
/*
    BSD 3-Clause License

    Copyright (c) 2023, KORG INC.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
      list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//*/

/*
 *  File: modfx.h
 *
 *  TEST instance of a modulation-effect unit: verifies on the NTS-1 mkII
 *  whether the 3rd (edit-mode) parameter of a modfx unit is actually
 *  reachable (hold MOD + turn encoder) and whether setParameter() fires for
 *  it. The DSP is a 4-mode TEST selector (slot 2, "TEST"):
 *
 *    0 = PASS — dry pass-through (regression baseline, like the dummy).
 *    1 = INV  — phase inversion (out = -in): instantly audible.
 *    2 = REV  — block reversal: each incoming stereo block is written into
 *               the SDRAM buffer and read back in reverse order (looping).
 *               Sounds very wrong / "bubbly" — unmistakable confirmation
 *               that the parameter is live.
 *    3 = MUTE — silence.
 *
 *  TIME (slot 0) and DPTH (slot 1) are declared in header.c but are NOT
 *  consumed by this test DSP (they exist only so the unit has the same
 *  fixed-knob shape as the final unit). The whole point is the 3rd slot.
 *
 *  This file is a TEST scaffold, not the final DRONE-MOD DSP.
 *
 */
#include "processor.h"
#include "unit_modfx.h" // include base definitions for modfx units
#include "modfx_constants.h" // tuning constants + libm-free helpers (modfx_consts::*)

class Modfx : public Processor
{
public:
  uint32_t getBufferSize() const override final { return 0x4000U; } // 16k (formant + comb ring buffers)

  // audio parameters (header.c slot order)
  enum
  {
    TIME = 0U,  // LFO tempo ceiling (0..1)
    DEPTH,      // per-stage modulation depth (0..1)
    DRIFT,      // shared self-motion ceiling (0..1, 0 = static)
    ENSE,       // stereo chorus level (0..1)
    TREMO,      // harmonic/optical tremolo level (0..1)
    FLNG,       // thru-zero flanger level (0..1)
    TAPE,       // tape wow+flutter level (0..1)
    FORM,       // formant vowel position (0..1, morph A-E-I-O-U)
    FLVL,       // formant stage level (0..1, 0 = dry)
    COMB,       // comb resonator feedback (0..1 -> 0..kCombFbMax)
    NUM_PARAMS
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  // (header.c: TIME 256, DEPTH 511, DRIFT 0, ENSE 1023, TREMO 307, FLNG 0,
  //  TAPE 0, FORM 511, FLVL 0, COMB 0).
  struct Params
  {
    float time;   // 0..1 (kTime -> LFO frequency curve)
    float depth;  // 0..1 (per-stage modulation depth)
    float drift;  // 0..1 (shared self-motion ceiling, 0 = static)
    float ense;   // 0..1 (stereo chorus wet)
    float tremo;  // 0..1 (tremolo wet)
    float flng;   // 0..1 (flanger wet)
    float tape;   // 0..1 (tape wow+flutter wet)
    float form;   // 0..1 (formant vowel morph position)
    float flvl;   // 0..1 (formant stage wet)
    float comb;   // 0..kCombFbMax (comb resonator feedback)

    void reset()
    {
      time = 0.25f;   // 256/1023
      depth = 0.5f;   // 511/1023
      drift = 0.f;    // 0 (static, regression-safe)
      ense = 1.0f;    // 1023/1023 (full chorus)
      tremo = 0.3f;   // 307/1023
      flng = 0.f;     // 0 (off)
      tape = 0.f;     // 0 (off)
      form = 0.5f;    // 511/1023 (middle vowel, but the stage is DRY until FLVL>0)
      flvl = 0.f;     // 0 (formant dry/transparent)
      comb = 0.f;     // 0 (comb off)
    }

    Params() { reset(); }
  };

  inline void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case TIME:
      params_.time = param_10bit_to_f32(value);
      break;
    case DEPTH:
      params_.depth = param_10bit_to_f32(value);
      break;
    case DRIFT:
      params_.drift = param_10bit_to_f32(value);
      break;
    case ENSE:
      params_.ense = param_10bit_to_f32(value);
      break;
    case TREMO:
      params_.tremo = param_10bit_to_f32(value);
      break;
    case FLNG:
      params_.flng = param_10bit_to_f32(value);
      break;
    case TAPE:
      params_.tape = param_10bit_to_f32(value);
      break;
    case FORM:
      params_.form = param_10bit_to_f32(value);
      recompute_formant_comb(); // K-rate: formant F1/F2 delays + shared feedback
      break;
    case FLVL:
      params_.flvl = param_10bit_to_f32(value);
      recompute_formant_comb(); // K-rate (keep the cached feedback consistent)
      break;
    case COMB:
      params_.comb = param_10bit_to_f32(value) * modfx_consts::kCombFbMax;
      break;
    default:
      break;
    }
  }

  inline const char *getParameterStrValue(uint8_t index, int32_t value) const override final
  {
    // Note: String memory must be accessible even after function returned.
    //       It can be assumed that the caller will have copied or used the string
    //       before the next call to getParameterStrValue.
    //
    // Only FORM is a strings-type slot (the vowel); every other slot is
    // type_none and formats as a percentage in the web simulator
    // (drone-chain/wasm.cc getParameterValueString). Return nullptr for all
    // slots except FORM so the platform / sim fall through to % for the rest.
    if (index == FORM)
    {
      static const char vowels[5] = {'A', 'E', 'I', 'O', 'U'};
      static char buf[8];
      int vi = (int)(param_10bit_to_f32(value) * 4.999f);
      if (vi < 0) vi = 0;
      else if (vi > 4) vi = 4;
      buf[0] = vowels[vi];
      buf[1] = '\0';
      return buf;
    }
    return nullptr;
  }

  // life-cycle methods
  void init(float *allocated_buffer) override final
  {
    buffer_ = allocated_buffer;
    params_.reset();

    // Slice the SDRAM buffer across the three ring buffers (formant F1 + F2 +
    // comb). The total is (2 * kFormantDelayMaxSmp + kCombDelayMaxSmp) floats
    // = (2*256 + 256) = 768 floats << getBufferSize() (16k), so there is ample
    // headroom. If no buffer was allocated (the web sim always provides one),
    // the pointers stay null and the formant/comb stages are bypassed.
    if (buffer_ != nullptr)
    {
      formant_buf1_ = buffer_;
      formant_buf2_ = buffer_ + modfx_consts::kFormantDelayMaxSmp;
      comb_buf_ = buffer_ + 2U * modfx_consts::kFormantDelayMaxSmp;
      for (uint32_t i = 0; i < modfx_consts::kFormantDelayMaxSmp; ++i)
      {
        formant_buf1_[i] = 0.f;
        formant_buf2_[i] = 0.f;
      }
      for (uint32_t i = 0; i < modfx_consts::kCombDelayMaxSmp; ++i)
        comb_buf_[i] = 0.f;
    }
    else
    {
      formant_buf1_ = nullptr;
      formant_buf2_ = nullptr;
      comb_buf_ = nullptr;
    }
    formant_comb_w1_ = 0U;
    formant_comb_w2_ = 0U;
    comb_w_ = 0U;

    lcg_state_ = modfx_consts::kLcgSeed;
    ense_lfo_phase_ = 0.f;
    tremo_lfo_phase_ = 0.f;
    flng_lfo_phase_ = 0.f;

    // DRIFT walk state (all off by default).
    for (int i = 0; i < kDriftCount; ++i)
    {
      drift_target_[i] = 0.f;
      drift_cur_[i] = 0.f;
      drift_tau_[i] = 1.f;
      drift_next_gap_[i] = 1U;
    }

    tape_lpf_ = 0.f;
    tremo_low_ = 0.f;
    tremo_high_ = 0.f;
    flng_hp_ = 0.f;
    flng_fb_ = 0.f;
    ense_pre_lpf_l_ = 0.f;
    ense_pre_lpf_r_ = 0.f;
    ense_delay_l_ = 0.f;
    ense_delay_r_ = 0.f;
    ense_post_lpf_l_ = 0.f;
    ense_post_lpf_r_ = 0.f;
    ense_ap_l_ = 0.f;
    ense_ap_r_ = 0.f;

    recompute_formant_comb(); // seed the formant delays/feedback from the defaults
  }

  void teardown() override final { buffer_ = nullptr; }

  // audio processing callbacks (per-sample path lives inline below in this class)
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    modfx_process_impl(in, out, frames);
  }

  // Diagnostic: public peak variables (for debugging gain issues).
  volatile float mod_input_peak_ = 0.f;
  volatile float mod_output_peak_ = 0.f;

  // K-rate: recompute the formant comb delays (F1/F2) and shared feedback
  // from the FORM / FLVL knobs. Same math as drone-osc's recompute_formant_comb
  // (the formant stage was moved here from the oscillator).
  void recompute_formant_comb()
  {
    const float pos = params_.form;   // 0..1
    const float x = pos * 4.f;        // 0..4 (continuous stop coordinate)
    const int i0 = (int)x;            // 0..3 (i0 == 4 only at x == 4)
    const float frac = x - (float)i0; // 0..1 (0 at x == 4)
    float f1 = 0.f, f2 = 0.f;
    if (i0 >= 0 && i0 < 4)
    {
      f1 = modfx_consts::kFormantStops[i0][0] * (1.f - frac) + modfx_consts::kFormantStops[i0 + 1][0] * frac;
      f2 = modfx_consts::kFormantStops[i0][1] * (1.f - frac) + modfx_consts::kFormantStops[i0 + 1][1] * frac;
    }
    else
    {
      f1 = modfx_consts::kFormantStops[4][0]; // x == 4 exactly -> U
      f2 = modfx_consts::kFormantStops[4][1];
    }
    // Delay = sr / (2 * f): the x2 factor puts the comb's fundamental peak at
    // f/2 (sub-octave of the formant) so the resonance sits in a musical band.
    // Clamped to [8, 250] samples (FPU division, not libm).
    const float sr = modfx_consts::kSr;
    uint32_t d1 = (uint32_t)(sr / (2.f * f1));
    uint32_t d2 = (uint32_t)(sr / (2.f * f2));
    if (d1 < modfx_consts::kFormantDelayMinSmp) d1 = modfx_consts::kFormantDelayMinSmp;
    else if (d1 > modfx_consts::kFormantDelayClampSmp) d1 = modfx_consts::kFormantDelayClampSmp;
    if (d2 < modfx_consts::kFormantDelayMinSmp) d2 = modfx_consts::kFormantDelayMinSmp;
    else if (d2 > modfx_consts::kFormantDelayClampSmp) d2 = modfx_consts::kFormantDelayClampSmp;
    formant_delay1_smp_ = d1;
    formant_delay2_smp_ = d2;
    // Shared feedback = FLVL (0..1) scaled to 0..kFormantFbMax (stable, < 1).
    formant_fb_ = params_.flvl * modfx_consts::kFormantFbMax;
  }

private:
  // ---- DRIFT walk (one per level stage: ENSE/TREMO/FLNG/TAPE/FORM/FLVL/COMB) --
  static constexpr int kDriftCount = 7;
  float drift_target_[kDriftCount];
  float drift_cur_[kDriftCount];
  float drift_tau_[kDriftCount];
  uint32_t drift_next_gap_[kDriftCount];

  // ---- LFO phase accumulators (ENSE / TREMO / FLNG) ---------------------------
  float ense_lfo_phase_;
  float tremo_lfo_phase_;
  float flng_lfo_phase_;

  // ---- LCG (Park-Miller-Carta) state -----------------------------------------
  uint32_t lcg_state_;

  // ---- Formant (F1+F2 one-pole IIR combs, MOVED from drone-osc) ---------------
  uint32_t formant_comb_w1_ = 0U;   // write index (F1 comb)
  uint32_t formant_comb_w2_ = 0U;   // write index (F2 comb)
  uint32_t formant_delay1_smp_ = 100U; // K-rate delay (F1, samples)
  uint32_t formant_delay2_smp_ = 50U;  // K-rate delay (F2, samples)
  float formant_fb_ = 0.f;           // K-rate shared feedback (0..kFormantFbMax)

  // ---- Comb resonator (IIR feedback through a delay, MOVED from drone-osc) ----
  uint32_t comb_w_ = 0U;            // write cursor

  // ---- First-order filter / feedback states ----------------------------------
  float tape_lpf_;    // flutter noise one-pole LPF state
  float tremo_low_;   // tremolo LPF band accumulator
  float tremo_high_;  // tremolo HPF band accumulator
  float flng_hp_;     // flanger HPF accumulator
  float flng_fb_;     // flanger feedback delay state
  // ENSE chorus pre/post LPF (Juno-style aliasing suppression).
  float ense_pre_lpf_l_; // pre-delay LPF (~2 kHz), left channel
  float ense_pre_lpf_r_; // pre-delay LPF (~2 kHz), right channel
  // ENSE one-pole "delay" voices: DEDICATED state (NOT tremo_low_/tremo_high_,
  // which are also the L/R scratch carriers consumed by TREMO/FLNG/TAPE;
  // sharing them corrupted the delay state with the chorus output each sample).
  float ense_delay_l_;   // voice 1 one-pole delay state (L)
  float ense_delay_r_;   // voice 2 one-pole delay state (R)
  float ense_post_lpf_l_; // post-chorus LPF (~6 kHz), left channel
  float ense_post_lpf_r_; // post-chorus LPF (~6 kHz), right channel
  float ense_ap_l_;       // allpass filter state, left channel (unit gain diffusion)
  float ense_ap_r_;       // allpass filter state, right channel

  float *buffer_; // SDRAM: formant_buf1_ + formant_buf2_ + comb_buf_ (sliced in init)
  float *formant_buf1_ = nullptr;  // kFormantDelayMaxSmp
  float *formant_buf2_ = nullptr;  // kFormantDelayMaxSmp
  float *comb_buf_ = nullptr;      // kCombDelayMaxSmp

  // ---- Per-sample DSP helper functions (static inline, no libm) ----
  // These are defined here (inside the class scope) so that the inline
  // modfx_process_impl() below can call them directly (no separate dsp header).

  // Advance one LFO phase accumulator by (2*pi*freq/sr) and return its sine
  // (4th-order Taylor; no libm).
  static float lfoStep(float &phase, float freq)
  {
    const float step = 6.283185307179586f * freq / modfx_consts::kSr;
    phase += step;
    if (phase >= 1.f) phase -= 1.f;
    const float ang = 6.283185307179586f * phase;
    const float x2 = ang * ang;
    return ang * (1.f - x2 / 6.f + x2 * x2 / 120.f);
  }

  // One-pole low-pass (alpha in [0,1]) on a running state. No libm.
  static float onelpf(float &state, float x, float alpha)
  {
    state += alpha * (x - state);
    return state;
  }

  // One-pole high-pass (alpha in [0,1]) on a running state. No libm.
  static float onehpf(float &state, float x, float alpha)
  {
    state += alpha * (x - state);
    return x - state;
  }

  // Soft-clip (same formula as fastertanh2f in common/utils/float_math.h).
  // Bounded: |out| < 1 for all finite |in|. No libm.
  static float softClip(float x)
  {
    const float xx = x * x;
    return x * (27.f + xx) / (27.f + 9.f * xx);
  }

  // First-order allpass filter (unit gain, phase shift only). No libm.
  // Canonical form: y = x - g * state'; state' = state + g * (x - state).
  // H(z) = (1 - g*z^-1) / (1 + g*z^-1), |H| = 1 for all frequencies (flat
  // magnitude, pure phase shift). For |g| < 1 the filter is stable.
  // (The previous implementation `y = x + g*(state - x)` was NOT an allpass:
  // it resonated the DC component and attenuated AC, which -- combined with
  // the DC-leaning output of the ENSE one-pole delays -- cancelled the audio
  // band and left the stage outputting near-silence.)
  static float allpass(float &state, float x, float g)
  {
    state += g * (x - state);
    return x - g * state;
  }

  // The per-sample audio path (upmix -> ENSE -> TREMO -> FLNG -> TAPE -> FORM
  // -> COMB -> makeup -> out). Defined INLINE inside the class so that
  // multiple translation units can each #include modfx.h through different
  // absolute paths without a duplicate-symbol link error (ODR: inline
  // member functions may be defined in every TU; the linker keeps one).
  inline void modfx_process_impl(const float *__restrict in, float *__restrict out, uint32_t frames)
{
  const Params p = params_;
  float in_peak = 0.f, out_peak = 0.f;
  const float sr = modfx_consts::kSr;

  // ---- K-rate LFO frequencies (from TIME; Taylor exp, no libm) --------------
  const float t = p.time;
  const float ense_freq = modfx_consts::kEnseLFOMin * modfx_expf(modfx_consts::kEnseLFOlnR * t);
  const float tremo_freq = modfx_consts::kTremoLFOMin * modfx_expf(modfx_consts::kTremoLFOlnR * t);
  const float flng_freq = modfx_consts::kFlngLFOMin * modfx_expf(modfx_consts::kFlngLFOlnR * t);

  // ---- K-rate one-pole alphas / feedbacks -----------------------------------
  const float tremo_alpha = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kTremoSplitHz / sr);
  const float flng_hpf_alpha = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kFlngHPFHz / sr);
  // ENSE chorus pre/post LPF alphas (one-pole, Juno-style aliasing suppression).
  const float ense_pre_lpf_alpha  = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kEnsePreLpfHz  / sr);
  const float ense_post_lpf_alpha = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kEnsePostLpfHz / sr);
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
    // `in` is INTERLEAVED STEREO (out[2n]=L, out[2n+1]=R) -- both the NTS-1
    // platform (modfx units are stereo, see drone-modfx/unit.cc) and the web
    // chain (Chain::process upmixes the mono OSC to L/R) deliver it this way.
    // The OLD code read `in[n]` (mono layout, frames samples) from this
    // 2*frames interleaved buffer: it consumed ONLY the even samples (the L
    // channel) and treated the odd (R) samples as the next frame of L --
    // a half-rate garbled signal. In the web chain L==R (the upmix is a
    // plain duplicate), so the L half still happened to be clean; on the
    // NTS-1, where the platform feeds true stereo, it would have been broken.
    const float in_l = in[2 * n];
    const float in_r = in[2 * n + 1];
    const float in_l_abs = (in_l < 0.f) ? -in_l : in_l;
    if (in_l_abs > in_peak) in_peak = in_l_abs;
    const float in_r_abs = (in_r < 0.f) ? -in_r : in_r;
    if (in_r_abs > in_peak) in_peak = in_r_abs;

    // ---- ENSE: stereo chorus (2 detuned voices, modulated one-pole delays) --
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

    // ENSE: only run the chorus voices when there is actual wet gain.
    // At ense_wet == 0 the stage is a pure wire (regression-safe, no LPF
    // artifact, no state accumulation that could cause DC offset / clipping).
    // Clouds-style: input gain (0.2x) + allpass diffusion + unitary dry/wet mix.
    // The input is attenuated BEFORE the chorus delays to prevent feedback-loop
    // overload in the one-pole filters. The allpass filters (unit gain) diffuse
    // the signal without adding gain. The dry/wet mix is unitary: out = in +
    // (wet - in) * amount, so the total level doesn't change with the knob.
    const float in_scaled_l = in_l * modfx_consts::kModInputGain;
    const float in_scaled_r = in_r * modfx_consts::kModInputGain;
    if (ense_wet > 0.f)
    {
      // Pre-LPF: soften the high end before the chorus delays (aliasing
      // suppression). Per-channel state so L and R diffuse independently
      // (the chorus image is what makes the stage sound stereo).
      const float pre_l = onelpf(ense_pre_lpf_l_, in_scaled_l, ense_pre_lpf_alpha);
      const float pre_r = onelpf(ense_pre_lpf_r_, in_scaled_r, ense_pre_lpf_alpha);

      const float ense_lfo = lfoStep(ense_lfo_phase_, ense_freq);
      const float spread1 = ense_spread_smp * (0.5f + 0.5f * ense_lfo);
      const float len1 = ense_base1_smp + spread1;
      const float a1 = 1.f / (1.f + len1 * 0.02f); // one-pole length -> alpha
      const float v1 = onelpf(ense_delay_l_, pre_l, a1);
      const float spread2 = ense_spread_smp * (0.5f - 0.5f * ense_lfo);
      const float len2 = ense_base2_smp + spread2;
      const float a2 = 1.f / (1.f + len2 * 0.02f);
      float v2 = onelpf(ense_delay_r_, pre_r, a2);
      v2 *= modfx_consts::kEnseVoice2; // voice 2 "pitch" (detune stand-in)

      // Allpass diffusion (unit gain, no level change). g = 0.5 (stable, |g| < 1).
      const float ap_g = 0.5f;
      const float wet_l = allpass(ense_ap_l_, v1, ap_g);
      const float wet_r = allpass(ense_ap_r_, v2, ap_g);

      // Post-LPF: suppress aliasing from the fractional-delay read.
      const float wet_lpf_l = onelpf(ense_post_lpf_l_, wet_l, ense_post_lpf_alpha);
      const float wet_lpf_r = onelpf(ense_post_lpf_r_, wet_r, ense_post_lpf_alpha);

      // Unitary dry/wet mix: out = in + (wet - in) * amount.
      // At ense_wet = 0: out = in (dry). At ense_wet = 1: out = wet (full chorus).
      const float chorus_l = in_scaled_l + (wet_lpf_l - in_scaled_l) * ense_wet;
      const float chorus_r = in_scaled_r + (wet_lpf_r - in_scaled_r) * ense_wet;
      tremo_low_ = chorus_l;   // L (scratch carrier, consumed by TREMO/FLNG/TAPE)
      tremo_high_ = chorus_r;  // R (scratch carrier)
    }
    else
    {
      // Wire: reset the one-pole state to the current (per-channel) input so
      // that a later ramp of ense_wet from 0 does not start from a stale
      // (accumulated) LPF value. This avoids a transient click / DC offset
      // when the user turns the ENSE knob up from zero.
      tremo_low_ = in_scaled_l;
      tremo_high_ = in_scaled_r;
      ense_pre_lpf_l_ = in_scaled_l;
      ense_pre_lpf_r_ = in_scaled_r;
      ense_delay_l_ = in_scaled_l;
      ense_delay_r_ = in_scaled_r;
      ense_post_lpf_l_ = in_scaled_l;
      ense_post_lpf_r_ = in_scaled_r;
      ense_ap_l_ = in_scaled_l;
      ense_ap_r_ = in_scaled_r;
    }
    float L = tremo_low_;
    float R = tremo_high_;

    // ---- TREMO: harmonic/optical (LPF/HPF split, antiphase LFO) -------------
    // Only run when there is actual wet gain. At tre_wet == 0 the stage is a
    // pure wire (regression-safe, no LPF/HPF state accumulation).
    if (tre_wet > 0.f)
    {
      const float tr = lfoStep(tremo_lfo_phase_, tremo_freq);
      const float amtd = tre_wet * modfx_consts::kTremoDepthScale * (0.5f + 0.5f * tr);
      const float amtd2 = tre_wet * modfx_consts::kTremoDepthScale * (0.5f - 0.5f * tr);
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        float &st = (ch == 0) ? tremo_low_ : tremo_high_;
        const float low = onelpf(st, x, tremo_alpha); // LPF band
        const float high = x - low;                         // HPF residual
        float lowg = 1.f - amtd;
        if (lowg < modfx_consts::kTremoLowFloor) lowg = modfx_consts::kTremoLowFloor;
        const float highg = 1.f - amtd2;
        const float y = low * lowg + high * highg;
        if (ch == 0) L = y; else R = y;
      }
    }
    else
    {
      // Wire: reset the one-pole state to the current input so that a later
      // ramp of tre_wet from 0 does not start from a stale (accumulated) LPF
      // value. This avoids a transient click / DC offset when the user turns
      // the TREMO knob up from zero.
      tremo_low_ = L;
      tremo_high_ = R;
    }

    // ---- FLNG: thru-zero flanger (HPF + modulated short delay + feedback) ---
    if (fln_wet > 0.f)
    {
      const float fl_lfo = lfoStep(flng_lfo_phase_, flng_freq * 3.f); // x3 rate
      const float mod_len = flng_delay_smp * (1.f + 0.5f * fl_lfo);
      const float a = 1.f / (1.f + mod_len * 0.05f); // one-pole length -> alpha
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        float &st = (ch == 0) ? tremo_low_ : tremo_high_;
        const float hp = onehpf(st, x, flng_hpf_alpha); // first-order HPF
        const float delayed = onelpf(st, hp, a);        // modulated short delay
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
      const float flut = onelpf(tape_lpf_, noise, modfx_consts::kTapeLpfAlpha);
      const float gain = 1.f + tape_flutter_amt * flut;
      lcg_state_ = (modfx_consts::kLcgA * lcg_state_) & 0x7fffffffu;
      if (lcg_state_ == 0u) lcg_state_ = modfx_consts::kLcgSeed;
      const float walk = static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f;
      const float wow_alpha = 1.f / (1.f + tape_wow_smp * 0.02f * (1.f + 0.5f * walk));
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        float &st = (ch == 0) ? tremo_low_ : tremo_high_;
        const float wow = onelpf(st, x, wow_alpha);
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

    // ---- Makeup gain + write the stereo output -------------------------------
    // NO DC-block here: a ~20 Hz one-pole HPF (tau ~2 s) ate the entire
    // low-frequency DRONE signal (the ENSE chorus voices are long one-pole
    // delays whose outputs carry a DC-leaning envelope; the HPF let only the
    // slow level modulation through, so MOD-on sounded near-silent). The
    // DC offset that can accumulate in the one-pole stages is small and is
    // removed by the platform's / sim's output stage; kModMakeup (0.9) keeps
    // headroom against it.
    out[2 * n]     = L * modfx_consts::kModMakeup;
    out[2 * n + 1] = R * modfx_consts::kModMakeup;
    const float out_abs = (out[2 * n] < 0.f) ? -out[2 * n] : out[2 * n];
    const float out_abs2 = (out[2 * n + 1] < 0.f) ? -out[2 * n + 1] : out[2 * n + 1];
    if (out_abs > out_peak) out_peak = out_abs;
    if (out_abs2 > out_peak) out_peak = out_abs2;
  }

  // Publish the block peaks (diagnostic: chain.h exposes them via
  // getModInputPeak()/getModOutputPeak()).
  mod_input_peak_ = in_peak;
  mod_output_peak_ = out_peak;
}

  Params params_;
};
// (The per-sample path modfx_process_impl lives INLINE in this class above,
// so no separate modfx_dsp.h include is needed; the legacy modfx_dsp.h is
// dead code and not included anywhere.)
