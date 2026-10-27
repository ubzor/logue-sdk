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
 *  No dynamic allocation: all state lives in static members of this class.
 *
 */
#include <math.h>

#include "processor.h"
#include "unit_osc.h"

class Osc : public Processor
{
public:
  uint32_t getBufferSize() const override final { return 0; } // NTS-1 osc do not support sdram allocation

  // audio parameters
  enum
  {
    SHAPE = 0U, // knob A: detune of oscillator B (-50 .. +50 cents)
    ALT,        // knob B: drive level into the soft-clip saturator (0 .. 3)
    NUM_PARAMS
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float detune_cents; // -50.f .. +50.f, knob A
    float drive;        // 0.f .. 3.f, knob B

    void reset()
    {
      detune_cents = 0.f; // default: in tune
      drive = 1.f;        // default: unity drive
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
      break;

    case ALT:
      // 0 .. 1023 -> 0.f .. 3.f drive level into the saturator
      params_.drive = param_10bit_to_f32(value) * 3.f;
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
  }

  void reset() override final
  {
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
  }

  // audio processing callbacks

  // set frequency in digital w (w = f/samplerate, 0.5 is Nyquist)
  void setPitch(float w0)
  {
    w0_ = w0; // base phase increment, osc A uses this directly
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

    // base phase increment and detuned increment for the second oscillator
    const float w0 = w0_;
    // detune factor from cents: 2^(cents/1200)
    const float detune_factor = powf(2.f, p.detune_cents * (1.f / 1200.f));
    const float w0_b = w0 * detune_factor;

    const float drive = p.drive;

    for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
    {
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
      const float y = one_minus_d2 * tanhf(d / one_minus_d2);

      // scale output to a musical level (saturator max output is ~0.76)
      out[0] = y * 0.9f;
    }
  }

private:
  Params params_;
  float w0_;
  float lfo_;

  // local variables related to audio processing
  float phasor_a_;
  float phasor_b_;
};
