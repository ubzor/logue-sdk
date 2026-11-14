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
 *  File: delay.h
 *
 *  DRONE-DEL (delfx) DSP: a single-tap feed-back delay for the DRONE path.
 *
 *  Parameter model (header.c): the three FIXED knobs are TIME / DEPTH / MIX
 *  (slots 0/1/2, the 3rd slot is a dry/wet pair). Slot 3 (PARAM4, strings) is
 *  a template placeholder, unused by the audio path.
 *
 *  Semantics (the juandagilc/Audio-Effects "Delay" DSP scheme, adapted to the
 *  logue-sdk Processor model -- no JUCE, no APVTS, K-rate smoothed params):
 *
 *    TIME  -> delay time. 10-bit 0..1023 linear-mapped 10 ms .. 1.0 s
 *             (short times give crisp rhythmic taps, long times give
 *             atmospheric repeats; the line holds 2.73 s, so the 1.0 s max
 *             always fits even with the deepest feedback tail).
 *    DEPTH -> feedback (repeats level). 10-bit 0..1023 linear 0 .. 0.95
 *             (0.95 is the stability ceiling for a single-tap loop).
 *    MIX   -> dry/wet. drywet raw -1000..1000 -> mix -1..1,
 *             wet = (1 + mix)/2, dry = (1 - mix)/2  (0 = full dry,
 *             +1 = full wet).
 *
 *  Signal path (stereo input / stereo output, one block of `frames`):
 *
 *      L/R in -> mono downmix (0.5*(L+R)) -> DELAY LINE (mono ring, 2.73 s)
 *             |                              \
 *             |      read = readFrac(delay_samples)   (linear-interp tap)
 *             |      write = dry_in + read * fb       -> line.write()
 *             |           \______________________^  (recursive single-tap echo)
 *
 *      out_L = in_L * dry_gain + tap * wet_gain
 *      out_R = in_R * dry_gain + tap * wet_gain
 *
 *  The write tap INCLUDES the feedback so the next echo is the scaled
 *  re-injected signal -- a true recursive delay (the reference processBlock
 *  reads at the (smoothed) delay position, scales by feedback, adds the dry
 *  input, writes the sum, and outputs dry*dry_gain + read*wet_gain).
 *
 *  MONO echo: one shared mono line (the DRONE signal is already
 *  stereo-smear'd upstream by the MOD stage, and the REV stage widens it
 *  again downstream -- a mono echo here keeps the SDRAM small and the
 *  repeats tight, exactly like Modfx's TAPWOB / WARP shared-mono lines).
 *  L and R read the SAME tap (no stereo decorrelation: by design).
 *
 *  K-RATE smoothing: feedback and the wet/dry gains move the loop gain and
 *  are the two parameters most sensitive to zipper noise. They are one-pole
 *  smoothed per block (10 ms tau) so a knob turn glides in over ~10 ms --
 *  zipper-free, and no feedback blow-up transient. The delay time itself is
 *  NOT smoothed: a log-mapped step read is inaudible for a drone, and the
 *  line is a ring so a position jump just moves the tap (no re-pitch chirp).
 *
 *  STEREO: the DRONE signal is true stereo by the time it reaches DEL (MOD
 *  has stereo-smear'd it), so the delay runs a SEPARATE mono line per
 *  channel (L and R) -- the L and R echoes stay decorrelated and the image
 *  is preserved (NOT a mono downmix, which would collapse the stereo).
 *  The two lines share the same delay time / feedback / mix; because the
 *  inputs differ, the line states diverge and the echo stays stereo.
 *
 *  RAM: two mono lines of kLineLenSmp floats each (1.37 s @ 48 kHz per
 *  channel, enough for the 1.0 s max delay + the deepest feedback tail)
 *  = 131072 floats = 0.5 MB, from the allocated SDRAM buffer
 *  (getBufferSize() = 0x40000 bytes = 1 MB; the rest is spare -- the chain
 *  slices one contiguous allocation per stage).
 *
 *  A small power-of-2 mono ring buffer is implemented inline below (no
 *  dsp::DelayLine dependency: that header transitively pulls fixed_math.h ->
 *  cortexm.h -> arm_math.h (CMSIS), which is unavailable in the web-simulator
 *  (emscripten) build. The ring needs only setMemory / clear / write /
 *  readFrac, all of which are a few lines here.)
 */
#include "processor.h"
#include "unit_delfx.h" // include base definitions for delfx units

class Delay : public Processor
{
public:
  uint32_t getBufferSize() const override final { return 0x80000U; } // 2 MB (2 x 65536 floats = stereo)

  enum
  {
    TIME = 0U,
    DEPTH,
    MIX,
    PARAM4,
    NUM_PARAMS
  };

  // ---- DSP tuning constants ---------------------------------------------------
  static constexpr float kTimeMinSec = 0.010f;   // TIME raw 0    -> 10 ms
  static constexpr float kTimeMaxSec = 1.000f;   // TIME raw 1023 -> 1.0 s (linear)
  static constexpr float kFbMax = 0.95f;         // DEPTH raw 1023 -> 95% feedback
  static constexpr uint32_t kLineLenSmp = 65536U;  // 2^16 floats = 1.37 s @ 48 kHz (per channel)
  // K-rate one-pole smoothing (10 ms tau). The per-block step is PRECOMPUTED in
  // init()/reset() via a 4th-order Taylor (no libm in the audio path -- the
  // firmware provides no expf/powf, see check-und in the Makefile):
  //   alpha_s = 1 - exp(-dt/tau),  dt/tau = 1/(48000 * 0.010) = 1/480.
  //   block_step = 1 - (1-alpha_s)^128 = 1 - exp(-128/480) = 1 - exp(-0.2667).
  // Both Taylor approximations are accurate to <1e-4 over these ranges.
  static constexpr float kParamSmoothTauSec = 0.010f; // 10 ms smoothing time constant

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float time;  // 0..1 (TIME knob; process log-maps to kTimeMinSec..kTimeMaxSec)
    float depth; // 0..1 (DEPTH knob -> feedback 0..kFbMax)
    float mix;   // -1..1 (MIX drywet knob)
    uint32_t param4;

    void reset()
    {
      time = 0.5f; // header.c TIME init 512 -> ~0.5 -> ~316 ms (log midpoint)
      depth = 0.5f; // header.c DEPTH init 512 -> 50% feedback
      mix = -1.f;   // default to FULL DRY (bypass-transparent): the unit is a
                    // pure wire until the user cranks MIX up. This avoids the
                    // "enable step" when toggling DEL on: with mix = 0 (50/50)
                    // the user would hear an immediate level/tonal change.
      param4 = 1;
    }

    Params() { reset(); }
  };

  enum
  {
    PARAM4_VALUE0 = 0,
    PARAM4_VALUE1,
    PARAM4_VALUE2,
    PARAM4_VALUE3,
    NUM_PARAM4_VALUES,
  };

  inline void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case TIME:
      // 10bit 0-1023 parameter
      params_.time = param_10bit_to_f32(value); // 0 .. 1023 -> 0.0 .. 1.0
      break;

    case DEPTH:
      // 10bit 0-1023 parameter
      params_.depth = param_10bit_to_f32(value); // 0 .. 1023 -> 0.0 .. 1.0
      break;

    case MIX:
      // Single digit base-10 fractional value, bipolar dry/wet
      params_.mix = value / 1000.f; // -100.0 .. 100.0 -> -1.0 .. 1.0
      break;

    case PARAM4:
      // strings type parameter, receiving index value
      params_.param4 = value;
      break;

    default:
      break;
    }
  }

  inline const char *getParameterStrValue(uint8_t index, int32_t value) const override final
  {
    // Note: String memory must be accessible even after function returned.
    //       It can be assumed that caller will have copied or used the string
    //       before the next call to getParameterStrValue

    static const char *param4_strings[NUM_PARAM4_VALUES] = {
        "VAL 0",
        "VAL 1",
        "VAL 2",
        "VAL 3",
    };

    switch (index)
    {
    case PARAM4:
      if (value >= PARAM4_VALUE0 && value < NUM_PARAM4_VALUES)
        return param4_strings[value];
      break;
    default:
      break;
    }

    return nullptr;
  }

  // life-cycle methods
  void init(float *allocated_buffer) override final
  {
    buffer_ = allocated_buffer;
    line_[0] = nullptr;
    line_[1] = nullptr;
    line_w_[0] = 0U;
    line_w_[1] = 0U;
    if (buffer_)
    {
      // Two mono rings at the front of the SDRAM slice (L and R channels;
      // the rest is spare). Each is kLineLenSmp floats.
      line_[0] = buffer_;
      line_[1] = buffer_ + kLineLenSmp;
      for (uint32_t i = 0U; i < 2U * kLineLenSmp; ++i) buffer_[i] = 0.f;
    }
    params_.reset();
    computeBlockStep();
    // Seed the K-rate smoothed state at the reset params so the first block
    // does not glide from 0 (dry gain jumps from 1.0 -> 0.0 on a wet default).
    seedSmoothing();
  }

  void teardown() override final
  {
    buffer_ = nullptr;
    line_[0] = nullptr;
    line_[1] = nullptr;
    line_w_[0] = 0U;
    line_w_[1] = 0U;
  }

  // Reset the delay line + K-rate state. Per the Processor::reset contract the
  // exposed parameter VALUES are kept (only the transient DSP state is cleared).
  void reset() override final
  {
    if (line_[0])
      for (uint32_t i = 0U; i < 2U * kLineLenSmp; ++i) line_[0][i] = 0.f;
    line_w_[0] = 0U;
    line_w_[1] = 0U;
    computeBlockStep();
    seedSmoothing();
  }

  // audio processing callbacks
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    // K-rate smoothing of the loop gain / wet-dry gains (once per block).
    // The block_step was precomputed in init()/reset() (no libm in the audio
    // path). The raw params are read once here; the smoothed values drive the
    // whole block.
    const float fb_target = params_.depth * kFbMax;
    const float mix0 = clip1m1f(params_.mix);
    const float dry_target = (1.f - mix0) * 0.5f;
    const float wet_target = (1.f + mix0) * 0.5f;

    fb_cur_ += (fb_target - fb_cur_) * block_step_;
    dry_gain_cur_ += (dry_target - dry_gain_cur_) * block_step_;
    wet_gain_cur_ += (wet_target - wet_gain_cur_) * block_step_;

    // Delay time: linear-map the 0..1 knob to kTimeMinSec..kTimeMaxSec, then to
    // sample count. (No libm: a single multiply.)
    const float t_sec = kTimeMinSec + (kTimeMaxSec - kTimeMinSec) * params_.time;
    const float delay_samples = t_sec * getSampleRate();

    const float fb = fb_cur_;
    const float dry = dry_gain_cur_;
    const float wet = wet_gain_cur_;

    // Bypass-transparent fast path: when wet is EXACTLY zero (MIX = -100%),
    // the delay is a pure wire (no echo, no line updates). This avoids the
    // "enable step" the user hears when toggling DEL on with MIX = -100%:
    // without this, the DSP would still read/write the line (consuming CPU and
    // potentially picking up stale state) even though wet = 0.
    //
    // No threshold: -99% (wet = 0.5%) must still run the DSP (the user
    // intentionally set a small wet amount, so the echo is audible).
    if (wet == 0.f)
    {
      for (uint32_t i = 0; i < frames; ++i)
      {
        out[2 * i]     = in[2 * i];     // L dry
        out[2 * i + 1] = in[2 * i + 1]; // R dry
      }
      return;
    }

    // Linear-interp tap offset (the fractional part of the delay, shared by
    // both channels since the delay time is identical).
    const uint32_t delay_int = (uint32_t)delay_samples;
    const float frac = delay_samples - (float)delay_int;

    for (uint32_t i = 0; i < frames; ++i)
    {
      const float in_l = in[2 * i];
      const float in_r = in[2 * i + 1];

      // Per-channel recursive single-tap echo. Each channel has its OWN line
      // (true stereo: L and R diverge because their inputs differ). The ring
      // fills FORWARD (write head advances by 1 each sample), so a tap that is
      // `delay` samples OLD sits BEHIND the write head: index = w - delay.
      const float tap_l = readLine(0, delay_int, frac);
      const float tap_r = readLine(1, delay_int, frac);
      writeLine(0, in_l + tap_l * fb);
      writeLine(1, in_r + tap_r * fb);

      // Dry/wet crossfade (the gains sum to 1.0 -> no level change, no clip).
      out[2 * i] = in_l * dry + tap_l * wet;
      out[2 * i + 1] = in_r * dry + tap_r * wet;
    }
  }

private:
  // Inline power-of-2 mono ring buffer (a minimal stand-in for dsp::DelayLine,
  // which we avoid to keep the web build free of the CMSIS arm_math.h chain).
  // kLineLenSmp is a power of two, so the mask (kLineLenSmp-1) is exact and the
  // write head advances by 1 per sample (ring fills forward).
  static constexpr uint32_t kLineMask = kLineLenSmp - 1U;
  uint32_t line_w_[2] = {0U, 0U}; // ring write heads (L/R; also the tap origin)
  float *line_[2] = {nullptr, nullptr}; // 2 x kLineLenSmp floats (buffer_ after init)

  // Read the (linearly interpolated) tap of channel `ch` at `delay` samples
  // OLD. The ring fills forward, so the tap is BEHIND the write head:
  // index = w - delay. (Reading ahead, w + delay, would alias to 0 in an
  // empty line and only wrap after the ring length -- wrong.)
  inline float readLine(uint32_t ch, uint32_t delay, float frac) const
  {
    const uint32_t w = line_w_[ch];
    const uint32_t t0 = (w - delay) & kLineMask;
    const uint32_t t1 = (t0 + 1U) & kLineMask;
    const float a = line_[ch][t0];
    const float b = line_[ch][t1];
    return a + (b - a) * frac;
  }

  // Write `v` to the write head of channel `ch` (the recursive echo input).
  inline void writeLine(uint32_t ch, float v)
  {
    const uint32_t w = line_w_[ch];
    line_[ch][w] = v;
    line_w_[ch] = (w + 1U) & kLineMask;
  }

  // Precomputed K-rate per-block one-pole step (no libm in the audio path).
  // Computed in init()/reset() via a 4th-order Taylor for 1 - exp(-x):
  //   block_step_ = 1 - (1 - alpha_s)^128 = 1 - exp(-128 * dt/tau)
  //                = 1 - exp(-128 / (48000 * 0.010)) = 1 - exp(-0.2667).
  // Taylor: 1 - exp(-x) = x - x²/2 + x³/6 - x⁴/24 (valid for |x| <= 0.3,
  // accuracy <1e-4). At x = 0.2667 the error is ~2e-5 -- inaudible.
  float block_step_ = 0.f;

  // Compute the per-block one-pole step (called from init()/reset()).
  void computeBlockStep()
  {
    // dt/tau = 1 / (sr * tau) = 1 / (48000 * 0.010) = 1/480.
    // Over a 128-smp block: x = 128 * (1/480) = 0.2667.
    const float x = 128.0f / (getSampleRate() * kParamSmoothTauSec);
    const float x2 = x * x;
    const float x3 = x2 * x;
    const float x4 = x2 * x2;
    // 1 - exp(-x) = x - x²/2 + x³/6 - x⁴/24
    block_step_ = x - x2 * 0.5f + x3 / 6.0f - x4 / 24.0f;
  }

  // Load the current (raw) feedback / wet-dry params into the smoothed state.
  void seedSmoothing()
  {
    fb_cur_ = params_.depth * kFbMax;
    const float mix0 = clip1m1f(params_.mix);
    dry_gain_cur_ = (1.f - mix0) * 0.5f;
    wet_gain_cur_ = (1.f + mix0) * 0.5f;
  }

  float *buffer_; // valid range is from buffer_ to buffer_ + getBufferSize() (exclusive)
  Params params_;

  // K-rate smoothed state (per-block one-pole, see process).
  float fb_cur_ = 0.f;       // smoothed feedback (0..kFbMax)
  float dry_gain_cur_ = 1.f; // smoothed dry gain
  float wet_gain_cur_ = 0.f; // smoothed wet gain
};
