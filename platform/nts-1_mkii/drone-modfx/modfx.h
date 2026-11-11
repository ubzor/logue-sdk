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
 *  DRONE-MOD (modfx) DSP: a 4-stage modulation stack for the DRONE path.
 *  All 8 header slots are live (TIME / DEPTH / DRIFT / CMODE / CHORUS / TREMO /
 *  FLNG / TAPE, see header.c). The per-sample audio path is defined inline below:
 *    in (stereo) -> ENSE (Juno-60 BBD stereo chorus) -> TREMO (optical) ->
 *    FLNG (thru-zero flanger) -> TAPE (wow+flutter) -> makeup (0.9) -> out.
 *  DRIFT is a K-rate LCG random-walk on the four wet levels (shared ceiling,
 *  L1 budget); the walk state is in place but the levels are currently driven
 *  from the manual knobs (DRIFT default 0 = static, regression-safe).
 *  See modfx_constants.h for the tuning and README.md for the full parameter /
 *  signal-path documentation.
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
    TIME = 0U,  // LFO tempo (0..1) -> ENSE chorus LFO rate
    DEPTH,      // per-stage modulation depth (0..1) -> chorus LFO spread
    DRIFT,      // shared self-motion ceiling (0..1, 0 = static)
    CMODE,      // chorus mode (0..1) -> 3 equal bands: I / I+II / II
    ENSE,       // dry/wet of the chorus (0..1). 0 = dry, 1 = full.
    TREMO,      // harmonic/optical tremolo level (0..1)
    FLNG,       // thru-zero flanger level (0..1)
    TAPE,       // tape wow+flutter level (0..1)
    NUM_PARAMS  // = 8
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  // (header.c: TIME 512, DEPTH 511, DRIFT 0, CMODE 512, ENSE 1023, TREMO 307,
  //  FLNG 0, TAPE 0).
  struct Params
  {
    float time;   // 0..1 (LFO tempo -> ENSE chorus LFO rate)
    float depth;  // 0..1 (per-stage modulation depth -> chorus LFO spread)
    float drift;  // 0..1 (shared self-motion ceiling, 0 = static)
    float cmode;  // 0..1 (chorus mode, 3 equal bands: I / I+II / II)
    float ense;   // 0..1 (chorus dry/wet: 0=dry, 1=full)
    float tremo;  // 0..1 (tremolo wet)
    float flng;   // 0..1 (flanger wet)
    float tape;   // 0..1 (tape wow+flutter wet)

    void reset()
    {
      time = 0.5f;    // 512/1023 (Juno-like mid LFO rate)
      depth = 0.5f;   // 511/1023
      drift = 0.f;    // 0 (static, regression-safe)
      cmode = 0.5f;   // 512/1023 (band 1 = I+II, classic Juno)
      ense = 1.0f;    // 1023/1023 (full chorus wet)
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
    case CMODE:
      params_.cmode = param_10bit_to_f32(value);
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
    // ENSE: two mono delay lines (Juno main + sub), each kEnseLineLenSmp floats.
    ense_main_delay_ = p; p += modfx_consts::kEnseLineLenSmp;
    ense_sub_delay_  = p; p += modfx_consts::kEnseLineLenSmp;
    ense_main_w_ = 0U;
    ense_sub_w_ = 0U;
    // FLNG / TAPE: interleaved L/R pairs.
    flng_buf_  = p;  p += 2 * modfx_consts::kFlngLineLenSmp;
    tape_buf_  = p;  p += 2 * modfx_consts::kTapeLineLenSmp;

    // Zero-clear the rest (FLNG + TAPE lines).
    if (buffer_)
    {
      // ENSE delay lines already cleared via .clear(); clear the remaining.
      const uint32_t ense_bytes = 2U * modfx_consts::kEnseLineLenSmp;
      for (uint32_t i = ense_bytes; i < getBufferSize(); ++i) buffer_[i] = 0.f;
    }

    // Line heads.
    flng_w_ = 0U;
    tape_w_ = 0U;
    tape_wow_l_ = 0.f;
    tape_wow_r_ = 0.f;

    params_.reset();

    lcg_state_ = modfx_consts::kLcgSeed;
    tremo_lfo_phase_ = 0.f;
    flng_lfo_phase_ = 0.f;

    // ENSE Juno-chorus LFOs. q31 phase accumulators; the increment (w_) is
    // recomputed every block in K-rate from TIME (LFO rate sweep). Seed here
    // at the base Juno rates so the very first block before the first
    // K-rate pass has a sane rate.
    ense_lfo1_phase_ = 0;
    ense_lfo2_phase_ = 0;
    ense_lfo1_w_ = (int32_t)(modfx_consts::kEnseLfoRate1 * 2147483648.0f / modfx_consts::kSr);
    ense_lfo2_w_ = (int32_t)(modfx_consts::kEnseLfoRate1 * modfx_consts::kEnseLfoDetune * 2147483648.0f / modfx_consts::kSr);

    // ENSE LPFs (1st-order, Juno RC anti-alias). Init alpha at mid-range.
    ense_main_post_lpf_l_ = 0.f;
    ense_main_post_lpf_r_ = 0.f;
    ense_sub_post_lpf_l_ = 0.f;
    ense_sub_post_lpf_r_ = 0.f;
    ense_lpf_alpha_ = 0.5f; // mid-range default

    // DRIFT walk state (all off by default).
    for (int i = 0; i < kDriftCount; ++i)
    {
      drift_target_[i] = 0.f;
      drift_cur_[i] = 0.f;
      drift_tau_[i] = 1.f;
      drift_next_gap_[i] = 1U;
    }

    tape_lpf_ = 0.f;
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
  // ENSE uses two q31 triangle LFOs: main at the base Juno 0.513 Hz, sub
  // detuned x1.0618; both swept live by TIME (the increments w_ are recomputed
  // K-rate). TREMO / FLNG
  // use the hand-rolled float phase accumulators.
  // ENSE LFOs are hand-rolled q31 phase accumulators (same as dsp::SimpleLFO
  // but without the CMSIS/arm_math.h dependency needed for the web sim).
  int32_t ense_lfo1_phase_ = 0;  // Juno LFO 1 phase (q31)
  int32_t ense_lfo1_w_ = 0;      // Juno LFO 1 increment (q31, set K-rate from TIME)
  int32_t ense_lfo2_phase_ = 0;  // Juno LFO 2 phase (q31)
  int32_t ense_lfo2_w_ = 0;      // Juno LFO 2 increment (q31, set K-rate from TIME)
  float tremo_lfo_phase_;
  float flng_lfo_phase_;

  // ---- LCG (Park-Miller-Carta) state -----------------------------------------
  uint32_t lcg_state_;

  // ---- Filter / feedback states ----------------------------------------------
  float tape_lpf_;    // flutter noise one-pole LPF state
  // ENSE Juno-chorus LPFs: hand-rolled 1st-order LPF (one-pole) states,
  // per-channel, per-voice. Same math as dsp::BiQuad::setFOLP + process_fo
  // but without the CMSIS dependency. State: y = ff0*x + z; z = ff1*x - fb1*y.
  // For setFOLP(k): ff0=ff1=k/(k+1), fb1=(k-1)/(k+1).
  // Simplified: for a 1st-order LPF with cutoff k=tan(pi*wc), the recurrence
  // is: y[n] = y[n-1] + alpha*(x[n] - y[n-1]) where alpha = (k-1)/(k+1) + k/(k+1)
  // Actually simplest: use the standard one-pole: y += alpha*(x-y), alpha from k.
  // We store the LPF state (z1) per channel per voice.
  float ense_main_post_lpf_l_ = 0.f;  // main voice post-LPF state, L
  float ense_main_post_lpf_r_ = 0.f;  // main voice post-LPF state, R
  float ense_sub_post_lpf_l_ = 0.f;   // sub voice post-LPF state, L
  float ense_sub_post_lpf_r_ = 0.f;   // sub voice post-LPF state, R
  // K-rate LPF alpha (computed per block from the TIME knob).
  float ense_lpf_alpha_ = 0.f;        // 1st-order LPF alpha (0..1)

  // ---- Real delay lines (SDRAM, see getBufferSize()/init) --------------------
  // ENSE: two mono delay lines (Juno main + sub voices, 512 smp each).
  // Hand-rolled (raw float* + write_idx) to avoid the CMSIS dependency.
  float *ense_main_delay_ = nullptr;  // Juno main voice (mono, 512 smp)
  float *ense_sub_delay_ = nullptr;   // Juno sub voice (mono, 512 smp)
  uint32_t ense_main_w_ = 0U;         // main delay write head
  uint32_t ense_sub_w_ = 0U;          // sub delay write head
  float *flng_buf_;  // flanger delay lines [2 * kFlngLineLenSmp]
  float *tape_buf_;  // tape wow delay lines [2 * kTapeLineLenSmp]
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
  // Triangle LFO of a phase in [0,1): -1..+1 with constant slope (no phase
  // advance, no libm). A triangle gives the constant-velocity sweep of a BBD
  // chorus (the smooth swishing wash); a sine dwells at the peaks and reads as
  // pitch-vibrato wobble. Used for the ENSE chorus LFO. Pure float math.
  static float lfoTri(float phase)
  {
    // phase in [0,1): 0 -> -1, 0.5 -> +1, 1 -> -1 (linear ramps).
    const float x = phase < 0.5f ? phase * 2.f : (1.f - phase) * 2.f;
    return 2.f * x - 1.f;
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
  const float tremo_freq = modfx_consts::kTremoLFOMin * modfx_expf(modfx_consts::kTremoLFOlnR * t);
  const float flng_freq = modfx_consts::kFlngLFOMin * modfx_expf(modfx_consts::kFlngLFOlnR * t);

  // ---- ENSE Juno-chorus K-rate setup ----------------------------------------
  // CMODE knob -> Juno voice blend, 3 EQUAL bands (Formant-Vowel style):
  //   band 0 (CMODE < 1/3)  -> mode I    (main voice only)
  //   band 1 (1/3..2/3)     -> mode I+II (both voices, classic Juno)
  //   band 2 (>= 2/3)       -> mode II   (sub voice only)
  // Equal thirds of 0..1 so every knob value lands cleanly in a band.
  const int ense_mode = (p.cmode < 1.f/3.f) ? 0 : (p.cmode < 2.f/3.f) ? 1 : 2;
  const float ense_gain_main = (ense_mode == 0) ? modfx_consts::kEnseGainI[0]
                            : (ense_mode == 1) ? modfx_consts::kEnseGainI2[0] : modfx_consts::kEnseGainII[0];
  const float ense_gain_sub  = (ense_mode == 0) ? modfx_consts::kEnseGainI[1]
                            : (ense_mode == 1) ? modfx_consts::kEnseGainI2[1] : modfx_consts::kEnseGainII[1];

  // TIME knob -> ENSE chorus LFO rate (exponential, no libm). The main-voice
  // LFO scales from the base Juno rate (0.513 Hz) centered on TIME=0.5:
  //   scale = exp(kEnseLfoRateLnR * (TIME - 0.5));  TIME=0 -> e^-2.25, TIME=1 -> e^2.25.
  // The sub-voice LFO is DETUNED against the main by kEnseLfoDetune (x1.0618):
  // the two voices share one "heartbeat" but slowly drift in and out of phase,
  // producing the classic chorus "swelling" (beating) instead of two static
  // layers. This is the target spec's "голоса расстроены (x1, x1.0618)".
  const float ense_lfo_scale = modfx_expf(modfx_consts::kEnseLfoRateLnR * (t - 0.5f));
  const float ense_lfo1_freq = modfx_consts::kEnseLfoRate1 * ense_lfo_scale;                    // Hz (main)
  const float ense_lfo2_freq = ense_lfo1_freq * modfx_consts::kEnseLfoDetune;                  // Hz (sub, detuned)
  // q31 increment = freq * 2^31 / sr (one cycle per 1/freq s). Recomputed
  // every block so TIME moves the rate live with no phase jump: the phase
  // accumulator keeps running, only its step size changes.
  ense_lfo1_w_ = (int32_t)(ense_lfo1_freq * 2147483648.0f / sr);
  ense_lfo2_w_ = (int32_t)(ense_lfo2_freq * 2147483648.0f / sr);

  // DEPTH knob -> LFO spread (how far the LFO modulates the delay).
  // DEPTH=0 -> no modulation (delay stays at center, chorus is "dry").
  // DEPTH=1 -> full modulation (delay sweeps min..max, chorus is "deep").
  // ENSE knob -> dry/wet crossfade (0=dry only, 1=full chorus).
  const float ense_depth = p.depth; // 0..1 (LFO spread)
  const float ense_master = p.ense; // 0..1 (dry/wet)
  // Fixed 1st-order LPF alpha (Juno RC anti-alias, mid cutoff). No longer
  // swept by TIME: CMODE selects the voice, TIME drives the LFO rate.
  ense_lpf_alpha_ = 0.5f;

  // FLNG: feedback + delay length. Delay scales with DEPTH^2 (gentle -> pronounced).
  float flng_fb = p.flng > 0.f ? (p.depth * modfx_consts::kFlngFbMax) : 0.f;
  if (flng_fb > 0.85f) flng_fb = 0.85f;
  const float d2 = p.depth * p.depth;
  const float flng_delay_smp = modfx_consts::kFlngDelayMinSmp +
      d2 * (modfx_consts::kFlngDelayMaxSmp - modfx_consts::kFlngDelayMinSmp);

  // ENSE Juno-chorus delay ranges (samples). The LFO sweeps within these.
  const float ense_main_dmin = modfx_consts::kEnseDelayMinMs * 0.001f * sr;
  const float ense_main_dmax = modfx_consts::kEnseDelayMaxMs * 0.001f * sr;
  const float ense_sub_dmin  = modfx_consts::kEnseSubDelayMinMs * 0.001f * sr;
  const float ense_sub_dmax  = modfx_consts::kEnseSubDelayMaxMs * 0.001f * sr;

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

    // Per-channel working vars (L/R) + per-stage wet amounts.
    float L = in_l;
    float R = in_r;
    float tre_wet = p.tremo;
    float fln_wet = p.flng;
    float tap_wet = p.tape;

    // ---- ENSE: Juno-60 stereo chorus (2 BBD voices, raw primitives) ----------
    // Architecture: peterall/junologue-chorus (MIT), adapted with hand-rolled
    // delay lines / LFOs / LPFs (no CMSIS dependency for the web sim).
    // Two mono delay lines (main + sub), each driven by a triangle LFO whose
    // rate is set K-rate from TIME (main = base Juno 0.513 Hz, sub detuned
    // x1.0618 -> shared heartbeat that drifts in/out of phase = chorus pulsing).
    // Each LFO reads the delay at two fractional taps (t and 1-t) and sums them
    // with the CMODE-selected per-voice gains. 1st-order LPF post-delay per
    // channel. CHORUS(dry/wet)=0 -> dry only. CHORUS=1 -> full chorus.
    if (ense_master > 0.f)
    {
      // Advance the two triangle LFOs (q31 phase accumulators).
      ense_lfo1_phase_ += ense_lfo1_w_;
      ense_lfo2_phase_ += ense_lfo2_w_;
      // Triangle unipolar 0..1 from q31 phase: |q31_to_f32(phase)|
      const float lfo1 = (ense_lfo1_phase_ < 0) ? -(ense_lfo1_phase_ * 4.6566128730773926e-10f)
                                                 :  (ense_lfo1_phase_ * 4.6566128730773926e-10f);
      const float lfo2 = (ense_lfo2_phase_ < 0) ? -(ense_lfo2_phase_ * 4.6566128730773926e-10f)
                                                 :  (ense_lfo2_phase_ * 4.6566128730773926e-10f);

      // SoftLimit (Juno BBD saturation, stmlib).
      const float in_mono = (L + R) * 0.5f;
      const float x2 = in_mono * in_mono;
      const float sl_in = in_mono * (27.f + x2) / (27.f + 9.f * x2);

      // Write to the mono delay lines (raw, power-of-2 mask).
      const uint32_t dl_mask = modfx_consts::kEnseLineLenSmp - 1U;
      ense_main_delay_[(ense_main_w_--) & dl_mask] = sl_in;
      ense_sub_delay_[(ense_sub_w_--) & dl_mask] = sl_in;

      // Read at the two LFO taps (fractional, linear interpolation).
      auto readFrac = [](float *buf, uint32_t w, uint32_t mask, float pos) -> float {
        const uint32_t base = (uint32_t)pos;
        const float frac = pos - base;
        const float s0 = buf[(w + base) & mask];
        const float s1 = buf[(w + base + 1U) & mask];
        return s0 + frac * (s1 - s0);
      };

      // LFO spread from DEPTH: delay = center +/- spread * DEPTH * (lfo - 0.5).
      // DEPTH=0 -> delay=center (no modulation). DEPTH=1 -> delay=min..max.
      const float main_center = (ense_main_dmin + ense_main_dmax) * 0.5f;
      const float main_spread = (ense_main_dmax - ense_main_dmin) * 0.5f * ense_depth;
      const float sub_center  = (ense_sub_dmin  + ense_sub_dmax)  * 0.5f;
      const float sub_spread  = (ense_sub_dmax  - ense_sub_dmin)  * 0.5f * ense_depth;
      const float main_t  = main_center + main_spread * (lfo1 - 0.5f);
      const float main_t2 = main_center + main_spread * (0.5f - lfo2);
      const float sub_t   = sub_center  + sub_spread  * (lfo1 - 0.5f);
      const float sub_t2  = sub_center  + sub_spread  * (0.5f - lfo2);

      const float main_wet = readFrac(ense_main_delay_, ense_main_w_, dl_mask, main_t) * ense_gain_main +
                             readFrac(ense_main_delay_, ense_main_w_, dl_mask, main_t2) * ense_gain_sub;
      const float sub_wet  = readFrac(ense_sub_delay_, ense_sub_w_, dl_mask, sub_t) * ense_gain_main +
                             readFrac(ense_sub_delay_, ense_sub_w_, dl_mask, sub_t2) * ense_gain_sub;

      // Post-LPF (1st-order one-pole, per channel for stereo phase image).
      const float wet_total_l = main_wet + sub_wet;
      const float wet_total_r = main_wet + sub_wet; // mono sum (Juno BBD is mono)
      ense_main_post_lpf_l_ += ense_lpf_alpha_ * (wet_total_l - ense_main_post_lpf_l_);
      ense_main_post_lpf_r_ += ense_lpf_alpha_ * (wet_total_r - ense_main_post_lpf_r_);
      const float wet_l = ense_main_post_lpf_l_ + ense_sub_post_lpf_l_;
      const float wet_r = ense_main_post_lpf_r_ + ense_sub_post_lpf_r_;

      // Dry/wet blend: ENSE = crossfade (0=dry only, 1=full chorus).
      // At ENSE=0: out = dry (wire). At ENSE=1: out = dry + wet.
      // DEPTH already applied to the LFO delay (spread), not the gain.
      L = L + ense_master * wet_l;
      R = R + ense_master * wet_r;
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
