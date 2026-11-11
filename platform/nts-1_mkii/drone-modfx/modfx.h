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
  uint32_t getBufferSize() const override final { return modfx_consts::kModSramBytes; } // 6 delay lines

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
    NUM_PARAMS
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  // (header.c: TIME 256, DEPTH 511, DRIFT 0, ENSE 1023, TREMO 307, FLNG 0,
  //  TAPE 0).
  struct Params
  {
    float time;   // 0..1 (kTime -> LFO frequency curve)
    float depth;  // 0..1 (per-stage modulation depth)
    float drift;  // 0..1 (shared self-motion ceiling, 0 = static)
    float ense;   // 0..1 (stereo chorus wet)
    float tremo;  // 0..1 (tremolo wet)
    float flng;   // 0..1 (flanger wet)
    float tape;   // 0..1 (tape wow+flutter wet)

    void reset()
    {
      time = 0.25f;   // 256/1023
      depth = 0.5f;   // 511/1023
      drift = 0.f;    // 0 (static, regression-safe)
      ense = 1.0f;    // 1023/1023 (full chorus)
      tremo = 0.3f;   // 307/1023
      flng = 0.f;     // 0 (off)
      tape = 0.f;     // 0 (off)
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
    default:
      break;
    }
  }

  inline const char *getParameterStrValue(uint8_t, int32_t) const override final
  {
    // All slots are type_none and format as a percentage in the web simulator
    // (drone-chain/wasm.cc getParameterValueString).
    return nullptr;
  }

  // life-cycle methods
  void init(float *allocated_buffer) override final
  {
    // Slice the single SDRAM allocation (getBufferSize() floats) into the six
    // interleaved delay lines (ENSE L/R, FLNG L/R, TAPE L/R). On the NTS-1 this
    // comes from unit.cc's sdram_alloc; in the web chain, Chain::init slices
    // the chain's single allocation and passes the MOD slice here.
    buffer_ = allocated_buffer;
    float *p = allocated_buffer;
    const float per = 2.f * (static_cast<float>(modfx_consts::kEnseLineLenSmp) +
                             static_cast<float>(modfx_consts::kFlngLineLenSmp) +
                             static_cast<float>(modfx_consts::kTapeLineLenSmp));
    ense_buf_ = p;
    p += 2 * modfx_consts::kEnseLineLenSmp;
    flng_buf_ = p;
    p += 2 * modfx_consts::kFlngLineLenSmp;
    tape_buf_ = p;
    p += 2 * modfx_consts::kTapeLineLenSmp;
    (void)per;

    // Zero-clear the line storage (avoid garbage on first block -> DC / noise).
    if (buffer_)
      for (uint32_t i = 0; i < getBufferSize(); ++i) buffer_[i] = 0.f;

    // Line heads (int, newest-written slot), wrapping per line.
    ense_w_ = 0U;
    flng_w_ = 0U;
    tape_w_ = 0U;
    tape_wow_l_ = 0.f;
    tape_wow_r_ = 0.f;

    params_.reset();

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
    ense_pre_lpf_l_ = 0.f;
    ense_pre_lpf_r_ = 0.f;
    ense_post_lpf_l_ = 0.f;
    ense_post_lpf_r_ = 0.f;
    ense_ap_l_ = 0.f;
    ense_ap_r_ = 0.f;
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

private:
  // ---- DRIFT walk (one per level stage: ENSE/TREMO/FLNG/TAPE) ----------------
  static constexpr int kDriftCount = 4;
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

  // ---- First-order filter / feedback states ----------------------------------
  float tape_lpf_;    // flutter noise one-pole LPF state
  // ENSE chorus pre/post LPF (Juno-style aliasing suppression), per channel.
  float ense_pre_lpf_l_;  // pre-delay LPF (~2 kHz), left channel
  float ense_pre_lpf_r_;  // pre-delay LPF (~2 kHz), right channel
  float ense_post_lpf_l_; // post-chorus LPF (~6 kHz), left channel
  float ense_post_lpf_r_; // post-chorus LPF (~6 kHz), right channel
  float ense_ap_l_;       // allpass filter state, left channel (unit gain diffusion)
  float ense_ap_r_;       // allpass filter state, right channel

  // ---- Real delay lines (SDRAM, see getBufferSize()/init) --------------------
  // Each stage owns an interleaved L/R pair of line buffers (length = the
  // stage's k..LineLenSmp). These replace the old modulated one-pole "delays",
  // which were lowpasses and read as mutes. Fractional reads (LFO-modulated
  // delay length) are linear-interpolated between the two nearest samples.
  float *ense_buf_;  // chorus delay lines [2 * kEnseLineLenSmp]
  float *flng_buf_;  // flanger delay lines [2 * kFlngLineLenSmp]
  float *tape_buf_;  // tape wow delay lines [2 * kTapeLineLenSmp]
  uint32_t ense_w_;  // line head (int, newest-written slot), ENSE
  uint32_t flng_w_;  // line head, FLNG
  uint32_t tape_w_;  // line head, TAPE
  float tape_wow_l_; // per-channel smoothed wow-walk state (L)
  float tape_wow_r_; // per-channel smoothed wow-walk state (R)

  float *buffer_; // SDRAM base (getBufferSize() floats, owned by the runtime)

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

  // Pure sine of a phase in [0,1) (no phase advance, no libm). Used to read a
  // phase-shifted copy of an LFO without disturbing the shared accumulator.
  static float lfoSine(float phase)
  {
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

  // ---- Real delay-line helpers (SDRAM line buffers, no libm) -----------------
  // A line is a pair of per-channel circular buffers (interleaved L/R, length
  // 2*len). The HEAD is an INTEGER (exact wrap, no float drift over 48k sps);
  // a per-sample fractional read offset `off` in [0,1) is tracked separately for
  // the fractional (LFO-modulated) delay reads. `write` (the head) is advanced
  // once per audio sample, storing both channels at the new head so L/R stay
  // in sync.
  static void lineWrite(float *buf, uint32_t len, uint32_t &head, float xl, float xr)
  {
    head = (head + 1U) % len;
    buf[head]     = xl; // ch 0 = L
    buf[head + 1] = xr; // ch 1 = R
  }

  // Fractional read: the sample `delay` (float, in samples) behind the newest
  // written sample, linearly interpolated. `head` is the newest-written slot.
  // `delay` is clamped to [1, len-1].
  static float lineReadF(float *buf, uint32_t len, int ch, uint32_t head, float delay)
  {
    if (delay < 1.f) delay = 1.f;
    const float lenf = (float)len;
    if (delay > lenf - 1.f) delay = lenf - 1.f;
    const float i0f = (float)head - delay; // newest is at `head`; one slot back is head-1
    // i0f is in (-(len-1), len-1]; one wrap each direction is sufficient.
    float w0 = i0f;
    if (w0 < 0.f) w0 += lenf;
    if (w0 >= lenf) w0 -= lenf;
    const uint32_t i0 = (uint32_t)w0;
    const uint32_t i1 = (i0 + 1U >= len) ? 0U : (i0 + 1U);
    // frac from the true pre-wrap anchor i0f (NOT the wrapped index, whose
    // slot may already hold an overwritten sample). i0 == (int)floor(i0f)
    // holds because i0f < len and the floor of i0f wraps exactly to i0.
    const float frac = i0f - (float)(int)(i0f);
    const float a = buf[i0 + ch];
    const float b = buf[i1 + ch];
    return a + (b - a) * frac;
  }

  // Shift an LFO phase accumulator by a fixed fraction of a cycle (for the
  // per-channel stereo LFO offsets). No libm.
  static float phaseShift(float phase, float shift)
  {
    float p = phase + shift;
    while (p >= 1.f) p -= 1.f;
    while (p < 0.f) p += 1.f;
    return p;
  }

  // The per-sample audio path (upmix -> ENSE -> TREMO -> FLNG -> TAPE ->
  // makeup -> out). Defined INLINE inside the class so that
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
  // ENSE chorus pre/post LPF alphas (one-pole, Juno-style aliasing suppression).
  const float ense_pre_lpf_alpha  = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kEnsePreLpfHz  / sr);
  const float ense_post_lpf_alpha = 1.f - modfx_one_minus_expf(-6.283185307179586f * modfx_consts::kEnsePostLpfHz / sr);

  // FLNG: feedback + delay length. Delay scales with DEPTH^2 (gentle -> pronounced).
  float flng_fb = p.flng > 0.f ? (p.depth * modfx_consts::kFlngFbMax) : 0.f;
  if (flng_fb > 0.85f) flng_fb = 0.85f;
  const float d2 = p.depth * p.depth;
  const float flng_delay_smp = modfx_consts::kFlngDelayMinSmp +
      d2 * (modfx_consts::kFlngDelayMaxSmp - modfx_consts::kFlngDelayMinSmp);

  // Chorus (ENSE) base delays (samples) + LFO spread (from DEPTH).
  const float ense_spread_smp = p.depth * modfx_consts::kEnseMaxSpreadSmp;
  const float ense_base_l_smp = modfx_consts::kEnseDelayBaseL * 0.001f * sr;
  const float ense_base_r_smp = modfx_consts::kEnseDelayBaseR * 0.001f * sr;

  // TAPE: wow delay base (samples) + flutter/wow depth (from DEPTH).
  const float tape_wow_smp = modfx_consts::kTapeWowBaseSmp;
  const float tape_wow_amt = p.depth * modfx_consts::kTapeWowAmt;
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
    if (p.drift > 0.f)
  {
    const float d = p.drift; // 0..1 (shared self-motion ceiling)
    // K-rate DRIFT random-walk: each of the 4 level stages gets a target
    // offset within [manual * (1 - d), manual * (1 + d)]. Targets are set on
    // an exponential schedule (gaps 2..12 s) and tracked with a one-pole
    // (5..20 s), so the levels drift slowly and smoothly. An L1 budget
    // (kDriftBudget) clamps the SUM of the per-stage drift amplitudes so the
    // wandering "percolates" between layers instead of all four moving at
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
    // L1 budget: clamp the sum of the 4 drift amplitudes to kDriftBudget.
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
    // (DRIFT itself is the 5th "stage" but it is the ceiling, not a level;
    //  the 4 levels are ENSE/TREMO/FLNG/TAPE.)
  }
  if (ense_wet < 0.f) ense_wet = 0.f; else if (ense_wet > 1.f) ense_wet = 1.f;
  if (tre_wet < 0.f) tre_wet = 0.f; else if (tre_wet > 1.f) tre_wet = 1.f;
  if (fln_wet < 0.f) fln_wet = 0.f; else if (fln_wet > 1.f) fln_wet = 1.f;
  if (tap_wet < 0.f) tap_wet = 0.f; else if (tap_wet > 1.f) tap_wet = 1.f;

    const float in_scaled_l = in_l * modfx_consts::kModInputGain;
    const float in_scaled_r = in_r * modfx_consts::kModInputGain;
    float L = in_scaled_l;
    float R = in_scaled_r;

    // ---- ENSE: stereo chorus (2 detuned REAL delay voices + pre/post LPF + allpass)
    // Real delay lines (lineReadF/lineWrite): the L voice is in-phase, the R
    // voice is the golden-ratio conjugate (x1.0618, longer base, phase-inverted
    // LFO) so the two never re-align -> a true stereo chorus image. The delays
    // run always (so re-enabling is seamless); at ense_wet == 0 the blend is a
    // pure wire (dry), which is regression-safe.
    {
      const float ense_lfo = lfoStep(ense_lfo_phase_, ense_freq);
      const float max_delay = static_cast<float>(modfx_consts::kEnseLineLenSmp) - 1.f;
      // Pre-LPF both channels, then store them at the SAME head position (the
      // cursor advances once per sample so L/R stay in sync).
      const float pre_l = onelpf(ense_pre_lpf_l_, L, ense_pre_lpf_alpha);
      const float pre_r = onelpf(ense_pre_lpf_r_, R, ense_pre_lpf_alpha);
      lineWrite(ense_buf_, modfx_consts::kEnseLineLenSmp, ense_w_, pre_l, pre_r);
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        const float base = (ch == 0) ? ense_base_l_smp : (ense_base_r_smp * modfx_consts::kEnseVoice2);
        const float lfo = (ch == 0) ? ense_lfo : -ense_lfo;     // R voice phase-inverted
        float delay = base + ense_spread_smp * lfo;
        if (delay < 1.f) delay = 1.f;
        if (delay > max_delay) delay = max_delay;

        const float delayed = lineReadF(ense_buf_, modfx_consts::kEnseLineLenSmp, ch, ense_w_, delay);

        float &post = (ch == 0) ? ense_post_lpf_l_ : ense_post_lpf_r_;
        const float post_x = onelpf(post, delayed, ense_post_lpf_alpha); // post LPF

        float &ap = (ch == 0) ? ense_ap_l_ : ense_ap_r_;
        const float ap_x = allpass(ap, post_x, 0.5f); // unit-gain diffusion

        const float y = x + ense_wet * (ap_x - x); // dry/wet (0 = wire)
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- TREMO: optical (pure AM) tremolo ------------------------------------
    // Classic optical tremolo: level = 1 - TREMO*DEPTH*depthMax*(0.5 - lfo) so
    // it DIPS toward the LFO trough and returns to unity at the peak. NO LPF/HPF
    // split (the old antiphase band split ate the bass -> the "mutes" symptom).
    // The R channel runs the LFO a quarter-cycle behind for a gentle shimmer.
    if (tre_wet > 0.f)
    {
      const float tre_depth = tre_wet * p.depth * modfx_consts::kTremoDepthMax; // 0..0.85
      lfoStep(tremo_lfo_phase_, tremo_freq); // advance the shared LFO once
      for (int ch = 0; ch < 2; ++ch)
      {
        // The R channel reads the LFO a quarter-cycle behind -> gentle shimmer.
        const float lfo = (ch == 0) ? lfoSine(tremo_lfo_phase_)
                                    : lfoSine(phaseShift(tremo_lfo_phase_, -modfx_consts::kTremoPhaseShiftR));
        const float gain = 1.f - tre_depth * (0.5f - 0.5f * lfo); // dip at trough, unity at peak
        const float x = (ch == 0) ? L : R;
        const float y = x * gain;
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- FLNG: thru-zero flanger (short REAL delay swept by LFO + feedback) ---
    // Real short delay line (DEPTH^2-scaled length) modulated by an LFO (x3
    // rate); the R LFO runs half a cycle out -> a stereo comb. The feedback tap
    // (DEPTH-scaled) builds the flanger's resonant "sweep". The minimum delay
    // keeps the comb "through zero" (no harsh null at 0). wet == 0 -> wire.
    if (fln_wet > 0.f)
    {
      lfoStep(flng_lfo_phase_, flng_freq * 3.f); // x3 rate
      const float flng_len_f = static_cast<float>(modfx_consts::kFlngLineLenSmp);
      lineWrite(flng_buf_, modfx_consts::kFlngLineLenSmp, flng_w_, L, R); // store both, advance once
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;
        // Modulated delay length around the DEPTH-set centre (sweep = DEPTH^2).
        const float lfo = (ch == 0) ? lfoSine(flng_lfo_phase_)
                                    : lfoSine(phaseShift(flng_lfo_phase_, modfx_consts::kFlngPhaseShiftR));
        float mod_len = flng_delay_smp * (1.f + 0.5f * lfo * (d2 + 0.1f));
        if (mod_len < 1.f) mod_len = 1.f;
        if (mod_len > flng_len_f - 1.f) mod_len = flng_len_f - 1.f;

        const float delayed = lineReadF(flng_buf_, modfx_consts::kFlngLineLenSmp, ch, flng_w_, mod_len);
        const float wet = x + delayed * flng_fb; // dry + feedback tap (resonant sweep)
        const float y = x + fln_wet * (wet - x); // dry/wet (0 = wire)
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- TAPE: wow (random-walk delay) + flutter (noise AM) --------------------
    // Wow: a per-channel LCG random-walk (one-pole smoothed) modulates the
    // length of a ~3 ms REAL delay line -> slow pitch/speed wander (independent
    // per channel, so L/R drift slightly apart). Flutter: the same LCG's
    // smoothed noise amplitude-modulates the level. wet == 0 -> wire.
    if (tap_wet > 0.f)
    {
      // Flutter: LCG white noise -> one-pole LPF -> level mod (slow, smooth).
      lcg_state_ = (modfx_consts::kLcgA * lcg_state_) & 0x7fffffffu;
      if (lcg_state_ == 0u) lcg_state_ = modfx_consts::kLcgSeed;
      const float noise = static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f;
      const float flut = onelpf(tape_lpf_, noise, modfx_consts::kTapeLpfAlpha);
      const float gain = 1.f + tape_flutter_amt * flut;

      const float tape_len_f = static_cast<float>(modfx_consts::kTapeLineLenSmp);
      lineWrite(tape_buf_, modfx_consts::kTapeLineLenSmp, tape_w_, L, R); // store both, advance once
      for (int ch = 0; ch < 2; ++ch)
      {
        const float x = (ch == 0) ? L : R;

        // Per-channel wow: LCG random-walk -> one-pole-smoothed -> delay mod.
        lcg_state_ = (modfx_consts::kLcgA * lcg_state_) & 0x7fffffffu;
        if (lcg_state_ == 0u) lcg_state_ = modfx_consts::kLcgSeed;
        const float walk = static_cast<float>(lcg_state_) * (2.f / 2147483647.f) - 1.f;
        float &wow_s = (ch == 0) ? tape_wow_l_ : tape_wow_r_;
        const float s = onelpf(wow_s, walk, 0.02f); // smoothed wander (slow)
        float delay = tape_wow_smp * (1.f + tape_wow_amt * s);
        if (delay < 1.f) delay = 1.f;
        if (delay > tape_len_f - 1.f) delay = tape_len_f - 1.f;

        const float wow = lineReadF(tape_buf_, modfx_consts::kTapeLineLenSmp, ch, tape_w_, delay);
        const float wet = wow * gain;
        const float y = x + tap_wet * (wet - x);
        if (ch == 0) L = y; else R = y;
      }
    }

    // ---- Makeup gain + write the stereo output -------------------------------
    // NO DC-block here: with REAL delay lines the stages no longer accumulate a
    // DC-leaning envelope, so a ~20 Hz one-pole HPF (tau ~2 s) would have eaten
    // the low-frequency DRONE signal for no benefit. kModMakeup (0.9) simply
    // leaves 1 dB of headroom against the chorus/flanger level peaks.
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
