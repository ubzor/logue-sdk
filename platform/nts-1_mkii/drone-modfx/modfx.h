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
 *  DRONE-MOD (modfx) DSP: a 3-stage modulation stack for the DRONE path.
 *  All 7 header slots are live (TIME / DEPTH / DRIFT / CMODE / CHORUS /
 *  FLNG / TAPE, see header.c). The per-sample audio path is defined inline below:
 *    in (stereo) -> ENSE (Juno-60 BBD stereo chorus) ->
 *    FLNG (thru-zero flanger) -> TAPE (wow+flutter) -> makeup (0.9) -> out.
 *  DRIFT is a K-rate LCG random-walk on the three wet levels (shared ceiling,
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
    FLNG,       // thru-zero flanger level (0..1)
    TAPE,       // tape wow+flutter level (0..1)
    NUM_PARAMS  // = 7
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  // (header.c: TIME 512, DEPTH 511, DRIFT 0, CMODE 512, CHORUS 1023,
  //  FLNG 0, TAPE 0).
  struct Params
  {
    float time;   // 0..1 (LFO tempo -> ENSE chorus LFO rate)
    float depth;  // 0..1 (per-stage modulation depth -> chorus LFO spread)
    float drift;  // 0..1 (shared self-motion ceiling, 0 = static)
    float cmode;  // 0..1 (chorus mode, 3 equal bands: I / I+II / II)
    float ense;   // 0..1 (chorus dry/wet: 0=dry, 1=full)
    float flng;   // 0..1 (flanger wet)
    float tape;   // 0..1 (tape wow+flutter wet)

    void reset()
    {
      time = 0.f;     // 0/1023 (slowest LFO: flanger 0.02 Hz = 50 s, chorus 60 s)
      depth = 0.f;    // 0/1023 (no modulation by default, regression-safe)
      drift = 0.f;    // 0 (static, regression-safe)
      cmode = 0.5f;   // 512/1023 (band 1 = I+II, classic Juno)
      ense = 0.f;     // 0/1023 (dry by default; user enables chorus)
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
    // FLNG: kFlngLineCount MONO delay lines (bucket-brigade), each kFlngLineLenSmp.
    flng_buf_  = p;  p += modfx_consts::kFlngLineCount * modfx_consts::kFlngLineLenSmp;
    for (uint32_t i = 0; i < modfx_consts::kFlngLineCount; ++i) flng_w_[i] = 0U;
    // TAPE: interleaved L/R pair.
    tape_buf_  = p;  p += 2 * modfx_consts::kTapeLineLenSmp;

    // Zero-clear the rest (FLNG + TAPE lines).
    if (buffer_)
    {
      // ENSE delay lines already cleared via .clear(); clear the remaining.
      const uint32_t ense_bytes = 2U * modfx_consts::kEnseLineLenSmp;
      for (uint32_t i = ense_bytes; i < getBufferSize(); ++i) buffer_[i] = 0.f;
    }

    // Line heads + FLNG filter state.
    for (uint32_t i = 0; i < modfx_consts::kFlngLineCount; ++i) flng_w_[i] = 0U;
    flng_hpf_x_ = 0.f; flng_hpf_y_ = 0.f; flng_lpf_l_ = 0.f; flng_lpf_r_ = 0.f;
    tape_w_ = 0U;
    tape_wow_l_ = 0.f;
    tape_wow_r_ = 0.f;

    params_.reset();

    lcg_state_ = modfx_consts::kLcgSeed;
    flng_lfo_phase_ = 0.f;

    // ENSE Juno-chorus LFOs. q31 phase accumulators; the increment (w_) is
    // recomputed every block in K-rate from TIME (LFO rate sweep). Seed here
    // at the base Juno rates so the very first block before the first
    // K-rate pass has a sane rate.
    ense_lfo1_phase_ = 0;
    ense_lfo2_phase_ = 0;
    ense_lfo1_w_ = (int32_t)(modfx_consts::kEnseLFOMin * 2147483648.0f / modfx_consts::kSr);
    ense_lfo2_w_ = (int32_t)(modfx_consts::kEnseLFOMin * modfx_consts::kEnseLfoDetune * 2147483648.0f / modfx_consts::kSr);

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
  // ---- DRIFT walk (one per level stage: ENSE/FLNG/TAPE) -------------------
  static constexpr int kDriftCount = 3;
  float drift_target_[kDriftCount];
  float drift_cur_[kDriftCount];
  float drift_tau_[kDriftCount];
  uint32_t drift_next_gap_[kDriftCount];

  // ---- LFO phase accumulators (ENSE / FLNG) ---------------------------------
  // ENSE uses two q31 triangle LFOs: main at the base Juno 0.513 Hz, sub
  // detuned x1.0618; both swept live by TIME (the increments w_ are recomputed
  // K-rate). FLNG uses a hand-rolled float phase accumulator.
  // ENSE LFOs are hand-rolled q31 phase accumulators (same as dsp::SimpleLFO
  // but without the CMSIS/arm_math.h dependency needed for the web sim).
  int32_t ense_lfo1_phase_ = 0;  // Juno LFO 1 phase (q31)
  int32_t ense_lfo1_w_ = 0;      // Juno LFO 1 increment (q31, set K-rate from TIME)
  int32_t ense_lfo2_phase_ = 0;  // Juno LFO 2 phase (q31)
  int32_t ense_lfo2_w_ = 0;      // Juno LFO 2 increment (q31, set K-rate from TIME)
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
  float *flng_buf_;  // flanger delay lines: kFlngLineCount MONO lines x kFlngLineLenSmp
  float *tape_buf_;  // tape wow delay lines [2 * kTapeLineLenSmp]
  uint32_t flng_w_[modfx_consts::kFlngLineCount]; // per-line write head, FLNG
  uint32_t tape_w_;  // line head, TAPE
  float tape_wow_l_; // per-channel smoothed wow-walk state (L)
  float tape_wow_r_; // per-channel smoothed wow-walk state (R)

  // FLNG filter state (cFlanger-inspired: HPF on feedback + LPF on wet output).
  float flng_hpf_x_ = 0.f;  // HPF: previous input (feedback tap)
  float flng_hpf_y_ = 0.f;  // HPF: previous output (filtered feedback)
  float flng_lpf_l_ = 0.f;  // LPF: L wet output state
  float flng_lpf_r_ = 0.f;  // LPF: R wet output state
  float *buffer_; // SDRAM base (getBufferSize() floats, owned by the runtime)

  // ---- Per-sample DSP helper functions (static inline, no libm) ----
  // These are defined here (inside the class scope) so that the inline
  // modfx_process_impl() below can call them directly (no separate dsp header).

  // Advance one LFO phase accumulator by (freq/sr) cycles and return its sine
  // (4th-order Taylor; no libm). `phase` is in CYCLES [0,1); `freq` is in
  // cycles/sec (Hz). step = freq/sr cycles per sample. The sine is computed
  // from ang = 2*pi*phase (radians). OLD bug: step was 2*pi*freq/sr, which
  // over-rotated the phase by 2*pi -> the LFO ran at 2*pi x the intended rate
  // (0.02 Hz -> 0.1256 Hz, a 50 s sweep read as an ~8 s beat).
  static float lfoStep(float &phase, float freq)
  {
    const float step = freq / modfx_consts::kSr;
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
    // frac in [0,1): the fractional part of the PRE-WRAP anchor i0f.
    // floor(i0f) is i0f's integer part rounded DOWN (toward -inf).
    // OLD bug: (int)(i0f) truncates toward zero, so for i0f in (-1,0) it
    // gave 0 and frac = i0f - 0 was NEGATIVE (e.g. -0.3), which extrapolated
    // a + (b-a)*frac outside [a,b] -> a broadband click/hum on every read-tap
    // wraparound, alternating L/R (the stereo LFO offsets make L and R wrap
    // at different instants -> "each ear in turn").
    // floor via: trunc toward -inf. For i0f >= 0: (int)i0f. For i0f < 0:
    // (int)i0f - 1 (because trunc gives 0 or -0 for -0.x, but floor is -1).
    const float trunc_i0f = (float)(int)i0f;
    const float floor_i0f = (i0f < 0.f && i0f != trunc_i0f) ? (trunc_i0f - 1.0f) : trunc_i0f;
    float frac = i0f - floor_i0f; // now always in [0,1)
    if (frac < 0.f) frac += 1.f;  // safety net (should never trigger)
    const float a = buf[i0 + ch];
    const float b = buf[i1 + ch];
    return a + (b - a) * frac;
  }

  // Flanger write with FEEDBACK: store (x + fb * one-slot-back) at the new
  // head. This is the Reiss/McPherson flanger topology -- the feedback tap is
  // RE-INJECTED into the delay line (not summed at the output), so the
  // resonant "jet" build-up happens inside the line and stays smooth. `head`
  // is advanced once per sample (both channels), exactly like lineWrite.
  static void flngWrite(float *buf, uint32_t len, uint32_t &head, float xl, float xr,
                        float fb, float prevl, float prevr)
  {
    head = (head + 1U) % len;
    buf[head]     = xl + fb * prevl;
    buf[head + 1] = xr + fb * prevr;
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

  // The per-sample audio path (upmix -> ENSE -> FLNG -> TAPE ->
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

  // TIME knob -> ENSE chorus LFO rate (shared exp curve, same as FLNG).
  // The main-voice LFO frequency comes from the common
  // kEnseLFOMin * exp(kEnseLFOlnR * TIME) curve (0.0167 Hz = 60 s at TIME=0
  // -> 0.6 Hz = 1.7 s at TIME=1). The sub-voice LFO is DETUNED against the
  // main by kEnseLfoDetune (x1.0618): the two voices share one "heartbeat"
  // but slowly drift in and out of phase, producing the classic chorus
  // "swelling" (beating) instead of two static layers.
  const float ense_lfo1_freq = modfx_consts::kEnseLFOMin * modfx_expf(modfx_consts::kEnseLFOlnR * t); // Hz (main)
  const float ense_lfo2_freq = ense_lfo1_freq * modfx_consts::kEnseLfoDetune;                        // Hz (sub, detuned)
  // q31 increment = freq * 2^31 / sr. Recomputed every block so TIME moves
  // the rate live with no phase jump: the phase accumulator keeps running,
  // only its step size changes.
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

  // FLNG (multi-line / bucket-brigade): small feedback + center delay. FOUR mono
  // lines, each with its own LFO phase (kFlngLinePhase), average into a smooth
  // thru-zero comb WASH. Small feedback (FLNG × 0.15, Q ~1.18) smooths the comb
  // peaks; the 4 de-tuned lines do not align at the LFO turnaround. Center delay
  // scales with DEPTH^2 (48..360 smp = 1..7.5 ms); each line's LFO sweeps the
  // read tap ±20% × DEPTH^2.
  float flng_fb = p.flng > 0.f ? (p.flng * modfx_consts::kFlngFbMax) : 0.f;
  if (flng_fb > modfx_consts::kFlngFbMax) flng_fb = modfx_consts::kFlngFbMax;
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

    // ---- FLNG: thru-zero flanger (multi-line / bucket-brigade) ----------------
    // Based on the cFlanger reference (calgoheen/cgo_modules Flanger.h):
    //   - 4 mono delay lines, average into a smooth comb WASH.
    //   - Feedback (FLNG x 0.15, Q ~1.18) re-injected through a HPF (~200 Hz)
    //     so bass does NOT build up in the feedback loop (cFlanger's
    //     "safeBass" -- the fix for the "distorted peaks" / "harsh" symptom).
    //   - Wet output goes through a LPF (~2 kHz) to tame the comb's high-end
    //     harshness (cFlanger's "warmth" filter).
    //   - Wet is x 0.5 (normalization: dry+wet sums to ~unity, not 2x).
    //   - LFO is TRIANGLE (constant-velocity sweep, no "dwelling" at the
    //     peaks where the interpolation artifacts are worst; cFlanger uses
    //     triangle, not sine).
    //   - LFO sweeps +/-10% x DEPTH^2 around the DEPTH^2 center (48..360 smp).
    //   - R is half a cycle out of L (stereo comb).
    // out = dry + FLNG * 0.5 * LPF(mean of 4 line wet taps). fln_wet == 0 -> wire.
    if (fln_wet > 0.f)
    {
      // Triangle LFO (constant velocity) instead of sine: no dwelling at peaks.
      lfoStep(flng_lfo_phase_, flng_freq);
      const float lfo_tri_l = lfoTri(flng_lfo_phase_);
      const float lfo_tri_r = lfoTri(phaseShift(flng_lfo_phase_, 0.5f)); // R half-cycle out

      const uint32_t LEN = modfx_consts::kFlngLineLenSmp;
      const float flng_len_f = static_cast<float>(LEN);
      const float sweep = flng_delay_smp * 0.15f * d2; // +/-15% x DEPTH^2 (deeper wash)
      const float ninv = 0.75f / (float)modfx_consts::kFlngLineCount; // 0.75 wet gain / 4 lines (deeper wash)

      float wetsum_l = 0.f, wetsum_r = 0.f;
      for (uint32_t li = 0; li < modfx_consts::kFlngLineCount; ++li)
      {
        float *line = flng_buf_ + li * LEN;   // mono line li
        const uint32_t H = flng_w_[li];
        const uint32_t Hp = (H > 0U) ? (H - 1U) : (LEN - 1U); // one slot back (feedback tap)

        const float lfo_l = lfo_tri_l;  // all lines share the same LFO phase (kFlngLinePhase = {0,0,0,0})
        const float lfo_r = lfo_tri_r;  // R is half a cycle out

        // cFlanger: feedback goes through a HPF (safeBass) before re-injection.
        // One-pole HPF: y[n] = (1-a)*(x[n]-x[n-1]) + y[n-1], a = exp(-2*pi*fc/sr).
        // fc = 200 Hz @ 48 kHz -> a = exp(-2*pi*200/48000) = exp(-0.02618) = 0.97416.
        const float hpf_a = 0.97416f; // precomputed: exp(-2*pi*200/48000)
        const float x = L;
        const float prev = line[Hp];
        // HPF the feedback tap (per line, shared state -- use a single HPF for
        // all 4 lines since they all process the same signal L==R).
        const float fb_hpf = (1.f - hpf_a) * (prev - flng_hpf_x_);
        flng_hpf_x_ = prev;
        flng_hpf_y_ = hpf_a * flng_hpf_y_ + fb_hpf;
        // Write (x + fb * HPF(prev)) at the new head.
        flngWrite(line, LEN, flng_w_[li], x, x, flng_fb, flng_hpf_y_, flng_hpf_y_);

        const float mod_l = flng_delay_smp + sweep * lfo_l;
        const float mod_r = flng_delay_smp + sweep * lfo_r;
        const float d_l = (mod_l < 1.f) ? 1.f : ((mod_l > flng_len_f - 1.f) ? (flng_len_f - 1.f) : mod_l);
        const float d_r = (mod_r < 1.f) ? 1.f : ((mod_r > flng_len_f - 1.f) ? (flng_len_f - 1.f) : mod_r);
        const float w_l = lineReadF(line, LEN, 0, flng_w_[li], d_l);
        const float w_r = lineReadF(line, LEN, 0, flng_w_[li], d_r);
        wetsum_l += w_l;
        wetsum_r += w_r;
      }
      // cFlanger: wet output goes through a LPF (warmth, ~2 kHz) to tame harshness.
      // One-pole LPF: y[n] = a*x[n] + (1-a)*y[n-1], a = 1-exp(-2*pi*fc/sr).
      // fc = 2000 Hz @ 48 kHz -> a = 1-exp(-2*pi*2000/48000) = 1-exp(-0.2618) = 0.2301.
      const float lpf_a = 0.5768f; // precomputed: 1-exp(-2*pi*5000/48000) (5 kHz cutoff: more "sparkle" in the wash)
      flng_lpf_l_ = lpf_a * (wetsum_l * ninv) + (1.f - lpf_a) * flng_lpf_l_;
      flng_lpf_r_ = lpf_a * (wetsum_r * ninv) + (1.f - lpf_a) * flng_lpf_r_;
      // out = dry + FLNG * LPF(wet_mean)  (ninv already includes 0.5 normalization)
      const float y_l = L + fln_wet * flng_lpf_l_;
      const float y_r = R + fln_wet * flng_lpf_r_;
      L = y_l; R = y_r;
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
