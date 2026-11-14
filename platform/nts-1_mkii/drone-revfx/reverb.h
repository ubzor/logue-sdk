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
 *  File: reverb.h
 *
 *  DRONE-REV (revfx) DSP: a stereo Schroeder / FreeVerb-style reverb for the
 *  DRONE path.
 *
 *  Parameter model (header.c): the three FIXED knobs are TIME / DEPTH / MIX
 *  (slots 0/1/2, the 3rd slot is a dry/wet pair). Slot 3 (PARAM4, strings) is
 *  a template placeholder, unused by the audio path.
 *
 *  Semantics (the classic FreeVerb / Schroeder-Moorer scheme, adapted to the
 *  logue-sdk Processor model -- no Faust, no JUCE, K-rate smoothed params):
 *
 *    TIME  -> "room size". 10-bit 0..1023 linear-mapped to a comb-length
 *             scale 0.03 .. 1.0 (fraction of the 200..310 smp reference comb
 *             lengths). Small scale = tight, washy space; large scale = big
 *             hall. (The scale, not an absolute RT60, is what the knob drives;
 *             the comb feedback is held ~0.78 so the tail density is constant
 *             and only the "size" changes.)
 *    DEPTH -> damping (darkness of the tail). 10-bit 0..1023 linear 0..1.
 *             Drives the low-pass coefficient inside each comb's feedback loop
 *             (Schroeder low-pass comb): 0 = bright / metallic, 1 = very dark.
 *             This is FreeVerb's `damp`.
 *    MIX   -> dry/wet. drywet raw -1000..1000 -> mix -1..1,
 *             wet = (1 + mix)/2, dry = (1 - mix)/2  (0 = 50/50, +1 = full wet).
 *
 *  STEREO: the input is TRUE stereo (the DRONE signal has been stereo-smear'd
 *  upstream by the MOD stage, so L != R by the time it reaches REV). The
 *  reverb therefore runs a SEPARATE filter network per channel (L and R each
 *  get their own 8 combs + 4 allpasses) so the L and R tails stay decorrelated
 *  and the image is preserved -- NOT a mono downmix (which would collapse the
 *  stereo field). The comb/allpass LENGTHS are identical across channels (the
 *  Schroeder tuning must stay the same in L and R), but because the INPUTS
 *  differ, the filter STATES diverge and the reverb stays stereo.
 *
 *  Topology (per channel, one block of `frames` interleaved stereo samples):
 *
 *      x -> [AP0] -> [AP1] -> [AP2] -> [AP3]   (4 Schroeder allpasses in
 *                                                series, diffusion/scattering)
 *        -> summed into 8 PARALLEL low-pass-feedback combs
 *        -> wet = (comb0 + comb1 + ... + comb7) * kCombSumGain
 *
 *      out_L = in_L * dry_gain + wet_L * wet_gain
 *      out_R = in_R * dry_gain + wet_R * wet_gain
 *
 *  Allpass (Schroeder, gain g, |g|<1):  y = g*xp + x - xp*y1
 *  Low-pass-feedback comb (Schroeder damped comb, feedback f, damping d):
 *      y  = x + f*lp_prev ;  lp = y + d*(lp_prev - y)
 *
 *  K-RATE smoothing: the damping and the wet/dry gains move the loop gain and
 *  are the parameters most sensitive to zipper noise. They are one-pole
 *  smoothed per block (10 ms tau) so a knob turn glides in over ~10 ms --
 *  zipper-free, and no feedback blow-up transient. The comb LENGTH scale (TIME)
 *  is NOT smoothed per-sample (a tap-position jump on the ring is inaudible
 *  for a reverb); it is applied directly from the 0..1 scale (recomputed once
 *  per block, no libm -- a single multiply).
 *
 *  NO libm: the firmware provides no expf/powf/logf/sinf (see check-und in
 *  the Makefile). Every coefficient is a multiply/add. The per-block one-pole
 *  smoothing step is PRECOMPUTED in init()/reset() via a 4th-order Taylor for
 *  1 - exp(-x) (exactly like drone-delfx/delay.h's computeBlockStep()).
 *
 *  RAM: per channel -- 8 comb rings + 4 allpass rings. The comb base lengths
 *  (200..310 smp) fit in 8192-float (2^13) power-of-2 rings (0.17 s @ 48 kHz)
 *  so the scaled integer tap (scale 0.03..1.0 of the base length) is always in
 *  range. The allpass lengths (157/255/523/341 smp) fit in 1024-float (2^10)
 *  rings. Per channel: 8*8192 + 4*1024 = 65536 + 4096 = 69632 floats; x2
 *  channels = 139264 floats (~0.55 MB total, well under getBufferSize() ==
 *  0x40000 bytes = 1 MB -- the SAME SDRAM budget as the DEL stage, with
 *  headroom). The chain slices one contiguous allocation per stage; the web
 *  simulator allocates the full getBufferSize() (std::vector), the ARM unit
 *  gets it from unit.cc's sdram_alloc.
 *
 *  Small power-of-2 mono ring buffers are implemented inline below (no
 *  dsp::DelayLine dependency: that header transitively pulls fixed_math.h ->
 *  cortexm.h -> arm_math.h (CMSIS), which is unavailable in the web-simulator
 *  (emscripten) build. Each ring needs only clear / write / read, all a few
 *  lines here.)
 */
#include "processor.h"
#include "unit_revfx.h" // include base definitions for delfx units

// File-scope tuning tables (static const -> internal linkage, so they do NOT
// appear as undefined dynamic symbols in the .so -- see check-und in the Makefile).
// kNumCombs / kNumAllpass are defined in the class; mirror the sizes here.
static const uint32_t kNumCombs_   = 8U;
static const uint32_t kNumAllpass_ = 4U;
// Comb base lengths (samples @ 48 kHz) -- Schroeder/FreeVerb near-prime spread.
static const uint32_t kCombBaseSmp_[kNumCombs_] =
    {200U, 228U, 252U, 276U, 284U, 292U, 300U, 310U};
// Allpass base lengths (samples @ 48 kHz) -- classic Schroeder diffusion set.
static const uint32_t kApBaseSmp_[kNumAllpass_] = {157U, 255U, 523U, 341U};
// Allpass gains (|g|<1; sign alternates for better scattering).
static const float kApGain_[kNumAllpass_] = {-0.5f, 0.5f, -0.5f, 0.5f};

class Reverb : public Processor
{
public:
  uint32_t getBufferSize() const override final { return 0xA0000U; } // 640 KB (163840 floats)

  enum
  {
    TIME = 0U,
    DEPTH,
    MIX,
    PARAM4,
    NUM_PARAMS
  };

  // ---- DSP tuning constants ---------------------------------------------------
  static constexpr uint32_t kNumCombs = 8U;    // parallel low-pass-feedback combs
  static constexpr uint32_t kNumAllpass = 4U;  // series allpasses (diffusion)
  // Power-of-2 ring lengths (so the index mask is exact; see the ring helpers).
  static constexpr uint32_t kCombRingLen = 8192U;  // 2^13 floats (0.17 s @ 48 kHz)
  static constexpr uint32_t kApRingLen = 1024U;    // 2^10 floats (0.021 s @ 48 kHz; max AP len 523)
  static constexpr uint32_t kCombRingMask = kCombRingLen - 1U;
  static constexpr uint32_t kApRingMask = kApRingLen - 1U;
  // Comb feedback (loop gain). Held near 0.78 so the tail is dense and the
  // TIME knob only changes "size" (comb length scale), not the decay.
  static constexpr float kCombFeedback = 0.78f;
  // Sum-of-8-combs scale (keeps the wet bus from clipping: 8 parallel combs each
  // ~0.5 amplitude -> x0.25 gives ~1.0 peak; the wet_gain crossfade then limits
  // the final level, exactly like drone-delfx/delay.h's dry+wet==1.0 rule).
  static constexpr float kCombSumGain = 0.25f;
  // Comb-length scale range (the TIME knob maps 0..1 -> kScaleMin..kScaleMax).
  static constexpr float kScaleMin = 0.03f; // TIME raw 0    -> tiny room
  static constexpr float kScaleMax = 1.00f; // TIME raw 1023 -> full-size room
  // Damping range (the DEPTH knob maps 0..1 -> kDampMin..kDampMax, the 1-pole
  // LPF coefficient inside the comb loop). 0 = bright, ~0.7 = dark/muffled.
  static constexpr float kDampMin = 0.0f;
  static constexpr float kDampMax = 0.70f;
  // K-rate one-pole smoothing (10 ms tau). The per-block step is PRECOMPUTED in
  // init()/reset() via a 4th-order Taylor for 1 - exp(-x) (no libm in the audio
  // path -- the firmware provides no expf/powf, see check-und in the Makefile):
  //   block_step = 1 - (1 - alpha_s)^128 = 1 - exp(-128 / (48000 * 0.010)).
  static constexpr float kParamSmoothTauSec = 0.010f; // 10 ms smoothing time constant

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float time;   // 0..1 (TIME knob; process maps to kScaleMin..kScaleMax)
    float depth;  // 0..1 (DEPTH knob; process maps to kDampMin..kDampMax)
    float mix;    // -1..1 (MIX drywet; 0 = 50/50, +1 = full wet)
    uint32_t param4; // unused (PARAM4 strings slot)

    void reset()
    {
      time = 0.5f;  // header.c TIME init 512 -> 50% -> mid-size room
      depth = 0.5f; // header.c DEPTH init 512 -> 50% -> mid damping
      mix = -1.f;   // default to FULL DRY (bypass-transparent): the unit is a
                    // pure wire until the user cranks MIX up. This avoids the
                    // "enable step" when toggling REV on: with mix = 0 (50/50)
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
    // Slice the single SDRAM allocation into the 2x8 comb rings + 2x8
    // allpass rings (each channel: 8 comb + 8 allpass x/y rings). The buffer
    // is getBufferSize() bytes = 0xA0000 (640 KB = 163840 floats); we use all
    // of them (8*8192*2 + 8*1024*2 = 163840).
    if (buffer_)
    {
      // Clear the whole allocation first (regression-safe: first reads before
      // the rings are filled must return 0, not uninitialized SDRAM).
      const uint32_t total_samples = getBufferSize() / sizeof(float);
      for (uint32_t i = 0U; i < total_samples; ++i)
        buffer_[i] = 0.f;

      float *p = buffer_;
      const uint32_t comb_block = kNumCombs * kCombRingLen;        // 8*8192 = 65536 floats
      const uint32_t ap_block = (2U * kNumAllpass) * kApRingLen;   // 8*1024 = 8192 floats (x+y)
      // Channel 0 (L): 8 comb rings, then 8 allpass rings (4 x-lines + 4 y-lines).
      comb_buf_[0] = p; p += comb_block;
      for (uint32_t a = 0U; a < kNumAllpass; ++a) apx_buf_[a][0] = p + a * kApRingLen;
      p += ap_block;
      for (uint32_t a = 0U; a < kNumAllpass; ++a) apy_buf_[a][0] = p + a * kApRingLen;
      p += ap_block;
      // Channel 1 (R): same layout.
      comb_buf_[1] = p; p += comb_block;
      for (uint32_t a = 0U; a < kNumAllpass; ++a) apx_buf_[a][1] = p + a * kApRingLen;
      p += ap_block;
      for (uint32_t a = 0U; a < kNumAllpass; ++a) apy_buf_[a][1] = p + a * kApRingLen;
      p += ap_block;
      for (uint32_t i = 0U; i < kNumCombs; ++i)
      {
        comb_w_[i][0] = 0U;
        comb_w_[i][1] = 0U;
      }
      for (uint32_t i = 0U; i < kNumAllpass; ++i)
      {
        apx_w_[i][0] = 0U;
        apx_w_[i][1] = 0U;
        apy_w_[i][0] = 0U;
        apy_w_[i][1] = 0U;
      }
    }
    params_.reset();
    computeBlockStep();
    // Seed the K-rate smoothed state at the reset params so the first block
    // does not glide from 0 (dry gain jumps from 1.0 -> 0.5 on a 50/50 default).
    seedSmoothing();
  }

  void teardown() override final { buffer_ = nullptr; }

  // Reset the filter state. Per the Processor::reset contract the exposed
  // parameter VALUES are kept (only the transient DSP state is cleared).
  void reset() override final
  {
    if (buffer_)
    {
      const uint32_t total_samples = getBufferSize() / sizeof(float);
      for (uint32_t i = 0U; i < total_samples; ++i)
        buffer_[i] = 0.f;
    }
    for (uint32_t i = 0U; i < kNumCombs; ++i)
    {
      comb_w_[i][0] = 0U;
      comb_w_[i][1] = 0U;
    }
    for (uint32_t i = 0U; i < kNumAllpass; ++i)
    {
      apx_w_[i][0] = 0U;
      apx_w_[i][1] = 0U;
      apy_w_[i][0] = 0U;
      apy_w_[i][1] = 0U;
    }
    computeBlockStep();
    seedSmoothing();
  }

  // audio processing callbacks
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    // K-rate smoothing of the loop gain / wet-dry gains (once per block).
    // block_step_ was precomputed in init()/reset() (no libm in the audio
    // path). The raw params are read once here; the smoothed values drive the
    // whole block.
    const float fb = kCombFeedback; // constant loop gain (see kCombFeedback)
    const float damp = kDampMin + (kDampMax - kDampMin) * params_.depth; // 0..0.70
    const float scale = kScaleMin + (kScaleMax - kScaleMin) * params_.time; // 0.03..1.0
    const float mix0 = clip1m1f(params_.mix);
    const float dry_target = (1.f - mix0) * 0.5f;
    const float wet_target = (1.f + mix0) * 0.5f;

    // Smooth the loop-sensitive coefficients (damping, wet/dry). The comb
    // feedback is constant (kCombFeedback) so it needs no smoothing.
    damp_cur_ += (damp - damp_cur_) * block_step_;
    dry_gain_cur_ += (dry_target - dry_gain_cur_) * block_step_;
    wet_gain_cur_ += (wet_target - wet_gain_cur_) * block_step_;

    const float d = damp_cur_;
    const float dry = dry_gain_cur_;
    const float wet = wet_gain_cur_;

    // Bypass-transparent fast path: when wet is EXACTLY zero (MIX = -100%),
    // the reverb is a pure wire (no reverb tail, no comb/allpass state updates).
    // This avoids the "enable step" the user hears when toggling REV on with
    // MIX = -100%: without this, the DSP would still compute the allpass + comb
    // network (consuming CPU and updating internal state) even though wet = 0.
    //
    // No threshold: -99% (wet = 0.5%) must still run the DSP (the user
    // intentionally set a small wet amount, so the reverb tail is audible).
    if (wet == 0.f)
    {
      for (uint32_t i = 0U; i < frames; ++i)
      {
        out[2 * i]     = in[2 * i];     // L dry
        out[2 * i + 1] = in[2 * i + 1]; // R dry
      }
      return;
    }

    // Per-channel comb tap lengths (the scaled integer tap, recomputed once per
    // block from the (un-smoothed) TIME scale -- a tap jump is inaudible here).
    // The ring is a power of two, so a tap up to kCombRingLen-1 is exact.
    for (uint32_t c = 0U; c < kNumCombs; ++c)
    {
      uint32_t t = (uint32_t)((float)kCombBaseSmp_[c] * scale);
      if (t >= kCombRingLen) t = kCombRingLen - 1U; // safety (scale<=1.0 keeps t<310)
      comb_tap_[c] = t;
    }

    for (uint32_t i = 0U; i < frames; ++i)
    {
      const float in_l = in[2 * i];
      const float in_r = in[2 * i + 1];

      // ---- Allpass chain (diffusion) for each channel ------------------------
      // Farrow allpass (a true all-pass, |H(e^jw)|==1, stable for |g|<1):
      //   y[n] = g*x[n] + x[n-D] - g*y[n-D]
      // It needs BOTH the delayed input xD and the delayed output yD, so each
      // stage keeps two rings: apx_buf_ (holds x) and apy_buf_ (holds y). The
      // write heads advance by 1 each sample and store the stage's input/output.
      float ap_out_l = in_l, ap_out_r = in_r;
      for (uint32_t a = 0U; a < kNumAllpass; ++a)
      {
        const float g = kApGain_[a];
        const uint32_t D = kApBaseSmp_[a];

        // Channel L.
        {
          const uint32_t wx = apx_w_[a][0];
          const uint32_t wy = apy_w_[a][0];
          const float xD = apx_buf_[a][0][(wx - D) & kApRingMask]; // x[n-D]
          const float yD = apy_buf_[a][0][(wy - D) & kApRingMask]; // y[n-D]
          const float y = g * ap_out_l + xD - g * yD;
          apx_buf_[a][0][wx] = ap_out_l; // store the stage input
          apx_w_[a][0] = (wx + 1U) & kApRingMask;
          apy_buf_[a][0][wy] = y;         // store the stage output
          apy_w_[a][0] = (wy + 1U) & kApRingMask;
          ap_out_l = y;
        }
        // Channel R.
        {
          const uint32_t wx = apx_w_[a][1];
          const uint32_t wy = apy_w_[a][1];
          const float xD = apx_buf_[a][1][(wx - D) & kApRingMask];
          const float yD = apy_buf_[a][1][(wy - D) & kApRingMask];
          const float y = g * ap_out_r + xD - g * yD;
          apx_buf_[a][1][wx] = ap_out_r;
          apx_w_[a][1] = (wx + 1U) & kApRingMask;
          apy_buf_[a][1][wy] = y;
          apy_w_[a][1] = (wy + 1U) & kApRingMask;
          ap_out_r = y;
        }
      }

      // ---- Low-pass-feedback comb bank (parallel) for each channel ----------
      // Damped comb: y = x + fb*lp_prev ; lp = y + d*(lp_prev - y). The ring
      // stores y (the comb output), so the delayed tap buf[w - tap] is the
      // comb input `tap` smp ago. x = the (allpass-filtered) input.
      float wet_l = 0.f, wet_r = 0.f;
      for (uint32_t c = 0U; c < kNumCombs; ++c)
      {
        const uint32_t tap = comb_tap_[c];
        // Channel L.
        {
          const uint32_t w = comb_w_[c][0];
          const uint32_t r = (w - tap) & kCombRingMask;
          const float lp_read = comb_buf_[0][r]; // damped delayed tap (true delay)
          const float y = ap_out_l + fb * lp_read;
          const float lp = y + d * (lp_read - y);
          comb_buf_[0][w] = lp; // store the damped value
          comb_w_[c][0] = (w + 1U) & kCombRingMask;
          wet_l += y;
        }
        // Channel R.
        {
          const uint32_t w = comb_w_[c][1];
          const uint32_t r = (w - tap) & kCombRingMask;
          const float lp_read = comb_buf_[1][r]; // damped delayed tap (true delay)
          const float y = ap_out_r + fb * lp_read;
          const float lp = y + d * (lp_read - y);
          comb_buf_[1][w] = lp; // store the damped value
          comb_w_[c][1] = (w + 1U) & kCombRingMask;
          wet_r += y;
        }
      }

      wet_l *= kCombSumGain;
      wet_r *= kCombSumGain;

      // ---- Dry/wet crossfade (gains sum to 1.0 -> no level change, no clip) --
      out[2 * i] = in_l * dry + wet_l * wet;
      out[2 * i + 1] = in_r * dry + wet_r * wet;
    }
  }

private:
  // Inline power-of-2 mono ring buffers (a minimal stand-in for dsp::DelayLine,
  // which we avoid to keep the web build free of the CMSIS arm_math.h chain).
  // Each ring's length is a power of two, so the mask is exact and the write
  // head advances by 1 per sample (ring fills forward; a tap `tap` samples OLD
  // sits BEHIND the write head: index = w - tap).
  //
  // comb_buf_[ch][c]: 8 rings of kCombRingLen floats (the comb feedback line).
  // Allpass: the Farrow form y = g*x + xD - g*yD needs BOTH the delayed
  // input xD and the delayed output yD, so each stage (a) x channel (ch)
  // has TWO kApRingLen rings: apx_buf_ (holds x) and apy_buf_ (holds y).
  // That is kNumAllpass*2 rings per channel = 4*2 = 8 rings of kApRingLen.
  float *comb_buf_[2] = {nullptr, nullptr};
  float *apx_buf_[kNumAllpass][2] = {{nullptr, nullptr}}; // allpass x-lines (L/R)
  float *apy_buf_[kNumAllpass][2] = {{nullptr, nullptr}}; // allpass y-lines (L/R)
  uint32_t comb_w_[kNumCombs][2] = {{0U, 0U}};  // comb ring write heads (L/R)
  uint32_t apx_w_[kNumAllpass][2] = {{0U, 0U}};  // allpass x-line write heads (L/R)
  uint32_t apy_w_[kNumAllpass][2] = {{0U, 0U}};  // allpass y-line write heads (L/R)

  // Per-channel comb tap lengths (samples), recomputed once per block from the
  // TIME scale (see process). Same for both channels (L==R lengths).
  uint32_t comb_tap_[kNumCombs] = {0U};

  // Precomputed K-rate per-block one-pole step (no libm in the audio path).
  // Computed in init()/reset() via a 4th-order Taylor for 1 - exp(-x):
  //   block_step_ = 1 - (1 - alpha_s)^128 = 1 - exp(-128 * dt/tau)
  //                = 1 - exp(-128 / (48000 * 0.010)) = 1 - exp(-0.2667).
  // Taylor: 1 - exp(-x) = x - x2/2 + x3/6 - x4/24 (valid for |x| <= 0.3,
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
    // 1 - exp(-x) = x - x2/2 + x3/6 - x4/24
    block_step_ = x - x2 * 0.5f + x3 / 6.0f - x4 / 24.0f;
  }

  // Load the current (raw) params into the smoothed state.
  void seedSmoothing()
  {
    const float mix0 = clip1m1f(params_.mix);
    dry_gain_cur_ = (1.f - mix0) * 0.5f;
    wet_gain_cur_ = (1.f + mix0) * 0.5f;
    damp_cur_ = kDampMin + (kDampMax - kDampMin) * params_.depth;
  }

  float *buffer_; // valid range is from buffer_ to buffer_ + getBufferSize() (exclusive)
  Params params_;

  // K-rate smoothed state (per-block one-pole, see process).
  float damp_cur_ = 0.f;       // smoothed damping (0..kDampMax)
  float dry_gain_cur_ = 0.5f;  // smoothed dry gain
  float wet_gain_cur_ = 0.5f;  // smoothed wet gain
};

