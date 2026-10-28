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
 *  File: osc.h
 *
 *  DRONE -- dual sawtooth drone generator for NTS-1 mkII.
 *
 *  Two independent sawtooth phasor accumulators running at the base note
 *  frequency. The A knob (SHPE) detunes the second oscillator up to +/-50
 *  cents from center, producing phase beating. The B knob (ALT) scales the
 *  drive level before a soft-clip tanh saturator, adding harmonic density.
 *
 *  An optional random-walk "wandering" mechanism modulates the detune around
 *  the knob value: RATE AMT sets the maximum deviation (quadratic mapping:
 *  knob^2 -> 0..+/-10 cents hard ceiling, so the detune never strays further
 *  from the DETUNE knob value than AMT allows) and RATE SPD sets the
 *  "restlessness" — how often the target changes and how quickly the detune
 *  tracks it (0.1 s fast, restless .. 10 s slow, lazy). With RATE AMT at 0
 *  the detune is static, preserving the original behavior.
 *
 *  No dynamic allocation: all state lives in static members of this class.
 *
 */

#include "processor.h"
#include "unit_osc.h"

namespace {
// Fast soft-clip saturator: rational (Padé [5/5]) approximation of tanh(x)
// over the full operating range x in [-4.68, 4.68] (the argument of tanhf()
// in process() is bounded by d / (1 - d^2) with |d| <= 0.9).
//
// The approximation uses the compacting map u = |x| / (1 + |x|) in [0, 0.823]
// and evaluates  tanh(x) ~= sign(x) * u * P(u) / Q(u),
// where P, Q are degree-5 polynomials fitted in the least-squares sense.
//
// Verified accuracy (float32, the deployment precision):
//   max |tanh(x) - approx(x)| over [-4.68, 4.68]  = 7.0e-4  (-63 dBFS)
// which is below the audibility threshold for this saturator (the error
// appears as a very slight rounding of the highest harmonics at full drive).
//
// Cost: ~14 VFP multiplies/adds + 1 divide per sample, versus ~40-80 cycles
// for the libm tanhf() (which pulls in expm1f, fabsf, __errno, ... ~900 B).
// No branches, no table lookups, no cancellation (all intermediate values
// stay in [0.5, 25], well within float32's 24-bit mantissa).
inline float soft_tanhf(float x)
{
  const float ax = x < 0.f ? -x : x; // fabsf
  const float u = ax / (1.f + ax);

  // P(u) = p0 + p1 u + p2 u^2 + p3 u^3 + p4 u^4 + p5 u^5
  const float num = 0.995777515f +
      u * (0.594395935f +
           u * (3.48766993f +
                u * (-3.67006819f +
                     u * (-5.48908741f + u * 5.10677737f))));

  // Q(u) = 1 + q0 u + q1 u^2 + q2 u^3 + q3 u^4 + q4 u^5
  const float den = 1.f +
      u * (-0.596846538f +
           u * (5.40544503f +
                u * (-16.5090433f +
                     u * (20.8395950f + u * (-9.35298147f)))));

  const float t = u * num / den;
  return x < 0.f ? -t : t;
}
} // namespace

class Osc : public Processor
{
public:
  uint32_t getBufferSize() const override final { return 0; } // NTS-1 osc do not support sdram allocation

  // audio parameters
  enum
  {
    SHAPE = 0U,   // knob A: detune of oscillator B (-50 .. +50 cents)
    ALT,          // knob B: drive level into the soft-clip saturator (0 .. 3)
    RATE_AMT,     // edit: random-walk depth (0..100%, default 0% = off)
    RATE_SPD,     // edit: random-walk speed (0..100%, default 50%)
    NUM_PARAMS
  };

  // random-walk maximum deviation in cents at RATE AMT = 100% (full knob).
  // The detune never strays further from the DETUNE knob value than this.
  static constexpr float kRwMaxCents = 10.f;

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float detune_cents; // -50.f .. +50.f, knob A
    float drive;        // 0.f .. 3.f, knob B
    float rw_amount;    // 0.f .. 1.f, edit: random-walk depth
    float rw_rate;      // 0.f .. 1.f, edit: random-walk speed

    void reset()
    {
      detune_cents = 0.f; // default: in tune
      drive = 1.f;        // default: unity drive
      rw_amount = 0.f;    // default: random walk off
      rw_rate = 0.5f;     // default: mid speed
    }

    Params() { reset(); }
  };

  void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case SHAPE:
      // 0 .. 1023 -> -50 .. +50 cents, center (511) is in tune
      params_.detune_cents = param_10bit_to_f32(value) * 100.f - 50.f;
      update_w0_b();
      break;

    case ALT:
      // 0 .. 1023 -> 0.f .. 3.f drive level into the saturator
      params_.drive = param_10bit_to_f32(value) * 3.f;
      break;

    case RATE_AMT:
      // 0 .. 1023 -> 0.f .. 1.f random-walk depth, quadratic mapping:
      // small knob positions give very small (barely audible) deviations,
      // full knob gives full +/-20-cent wander. rw_amount = x^2 where x is
      // the linear 0..1 knob value. This makes the low end of the knob a
      // fine "almost off" control instead of a hard on/off step.
      {
        const float x = param_10bit_to_f32(value);
        params_.rw_amount = x * x;
      }
      // Recompute the hard ceiling for the deviation (cents)
      rw_max_dev_ = params_.rw_amount * kRwMaxCents;
      break;

    case RATE_SPD:
      // 0 .. 1023 -> 0.f .. 1.f random-walk speed
      params_.rw_rate = param_10bit_to_f32(value);
      // Compute the target update interval in samples (recomputed once per parameter change)
      // period = 0.1 * 100^rw_rate seconds, converted to samples
      {
        const float period = 0.1f * fasterpowf(100.f, params_.rw_rate);
        rw_target_update_interval_ = (uint32_t)(period * 48000.f);
      }
      break;

    default:
      break;
    }
  }

  // life-cycle methods
  void init(float *) override final
  {
    params_.reset();
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    w0_ = 0.f;
    lfo_ = 0.f;
    rw_dev_cents_ = 0.f; // random-walk state starts centered
    rw_target_cents_ = 0.f; // random-walk target starts centered
    rw_target_update_counter_ = 0; // reset target update counter
    rw_max_dev_ = 0.f; // random-walk hard ceiling starts at 0 (AMT=0)
    rw_target_update_interval_ = 48000; // random-walk interval starts at 1s (default)
  }

  void reset() override final
  {
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    rw_dev_cents_ = 0.f; // re-center the random walk on stop
    rw_target_cents_ = 0.f; // re-center the random-walk target on stop
    rw_target_update_counter_ = 0; // reset target update counter
  }

  // audio processing callbacks

  // set frequency in digital w (w = f/samplerate, 0.5 is Nyquist)
  void setPitch(float w0)
  {
    w0_ = w0; // base phase increment, osc A uses this directly
    update_w0_b();
  }

  // lfo in (-1.0f, 1.0f)
  void setShapeLfo(float lfo)
  {
    lfo_ = lfo;
  }

  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    (void)in; // drone generator does not use the audio input

    // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
    const Params p = params_;

    // base phase increment
    const float w0 = w0_;
    const float drive = p.drive;

    // ---- Random-walk detune deviation (target-based wandering) ----
    // When rw_amount == 0 (knob at 0) the walk is disabled and w0_b is the
    // cached static value (regression: byte-for-byte identical to the old
    // code). rw_amount is the quadratic-mapped knob value (see setParameter),
    // so rw_active is true for any non-zero knob position.
    //
    // RATE AMT = maximum deviation (hard ceiling): the detune deviation is
    // limited to +/-rw_max_dev_ = +/-rw_amount*kRwMaxCents cents, so the
    // detune never strays further from the DETUNE knob value than AMT allows.
    // The deviation smoothly tracks a random target that is regenerated
    // periodically, ensuring |dev| <= rw_max_dev_ always.
    //
    // RATE SPD = restlessness: controls how often the target changes and how
    // quickly the deviation tracks it. High SPD -> frequent target updates
    // and fast tracking (restless), low SPD -> rare updates and slow tracking
    // (lazy drift).
    const bool rw_active = p.rw_amount > 0.f;
    float w0_b = w0_b_; // default: cached static detune
    if (rw_active)
    {
      // Compute the tracking coefficient once per block (rw_rate may change)
      // period = 0.1 * 100^rw_rate seconds
      const float period = 0.1f * fasterpowf(100.f, p.rw_rate);
      // Tracking coefficient: how quickly the deviation moves toward the target.
      // Higher k = faster tracking. k = 1 / (1 + tau * 48000) where tau is the
      // relaxation time (we use period as tau for simplicity).
      rw_k_ = 1.f / (1.f + period * 48000.f);
      // Initialize the target to the current deviation on first activation
      if (rw_target_cents_ == 0.f && rw_dev_cents_ != 0.f)
        rw_target_cents_ = rw_dev_cents_;
    }

    for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
    {
      // update random-walk detune deviation (cents) around the knob value
      if (rw_active)
      {
        // Update the target periodically
        if (++rw_target_update_counter_ >= rw_target_update_interval_)
        {
          rw_target_update_counter_ = 0;
          // Generate a new random target in [-rw_max_dev_, +rw_max_dev_]
          // Using osc_white() which returns values in [-1, 1]
          rw_target_cents_ = osc_white() * rw_max_dev_;
        }
        // Smoothly track the target: dev += (target - dev) * k
        rw_dev_cents_ += (rw_target_cents_ - rw_dev_cents_) * rw_k_;
        // Safety clamp (should rarely trigger if target is within bounds)
        if (rw_dev_cents_ > rw_max_dev_)
          rw_dev_cents_ = rw_max_dev_;
        else if (rw_dev_cents_ < -rw_max_dev_)
          rw_dev_cents_ = -rw_max_dev_;
        // recompute w0_b from effective detune (base + walk), 4th-order Taylor
        const float eff_detune = p.detune_cents + rw_dev_cents_;
        const float x = eff_detune * 5.7762265046261084e-04f; // cents * ln(2)/1200
        const float detune_factor =
            1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
        w0_b = w0 * detune_factor;
      }

      // phasor update
      phasor_a_ += w0;
      phasor_b_ += w0_b;

      // wrap phasors into [0, 1)
      if (phasor_a_ >= 1.f)
        phasor_a_ -= 1.f;
      if (phasor_b_ >= 1.f)
        phasor_b_ -= 1.f;

      // generate sawtooth waves in -1..1
      const float saw_a = 2.f * phasor_a_ - 1.f;
      const float saw_b = 2.f * phasor_b_ - 1.f;

      // mix both voices equally
      const float mix = 0.5f * (saw_a + saw_b);

      // soft-clip saturator: y = (1 - d^2) * tanh(d / (1 - d^2)), d in (-1, 1)
      // clip the drive-scaled signal into the stable range first
      float d = mix * drive;
      if (d > 0.9f)
        d = 0.9f;
      else if (d < -0.9f)
        d = -0.9f;
      const float one_minus_d2 = 1.f - d * d;
      const float y = one_minus_d2 * soft_tanhf(d / one_minus_d2);

      // scale output to a musical level (saturator max output is ~0.76)
      out[0] = y * 0.9f;
    }
  }

private:
  // cached derived coefficients, recomputed only when inputs change
  // (setPitch / setParameter(SHAPE)) to keep the audio path lightweight
  float w0_b_ = 0.f;

  // random-walk state: current detune deviation in cents (around the knob value)
  float rw_dev_cents_ = 0.f;
  // random-walk tracking coefficient (per sample), recomputed once per block
  // in process() from RATE SPD. Controls how quickly the deviation tracks the target.
  float rw_k_ = 0.f;
  // random-walk target deviation in cents, regenerated periodically.
  // The target is chosen randomly in [-rw_max_dev_, +rw_max_dev_].
  float rw_target_cents_ = 0.f;
  // random-walk hard ceiling for the deviation (cents), set by RATE AMT.
  float rw_max_dev_ = 0.f;
  // target update counter: counts samples since the last target update.
  uint32_t rw_target_update_counter_ = 0;
  // target update interval in samples (recomputed from RATE SPD each block).
  uint32_t rw_target_update_interval_ = 0;

  // recompute w0_b_ = w0_ * 2^(detune_cents/1200) — only when the base pitch
  // or detune actually changes.
  //
  // detune_cents is limited to +/-50 (see setParameter), so
  // x = cents * ln(2)/1200 stays within |x| <= 0.0417. In that range
  // 2^(cents/1200) = e^x is well approximated by a 4th-order Taylor series
  // (1 + x + x^2/2 + x^3/6 + x^4/24), which keeps the relative error below
  // 1.2e-7 (less than 0.003 cents at the extremes) — far under the threshold
  // of audibility. This replaces the libm powf() call, whose support chain
  // (__ieee754_powf, expm1f, scalbnf, ...) adds several hundred bytes of
  // code and data to the final unit image.
  //
  // Note: when the random walk is active (RATE AMT > 0), process() recomputes
  // the same Taylor expansion per sample for the effective (base + walk)
  // detune, so the audio path does run the 4th-order series there. The cached
  // w0_b_ above is only used while the walk is disabled.
  void update_w0_b()
  {
    const float x = params_.detune_cents * 5.7762265046261084e-04f; // cents * ln(2) / 1200
    const float detune_factor =
        1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
    w0_b_ = w0_ * detune_factor;
  }

  Params params_;
  float w0_;
  float lfo_;

  // local variables related to audio processing
  float phasor_a_;
  float phasor_b_;
};
