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
#include "utils/float_math.h" // fastertanh2f()

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
    SUB,          // edit: sub-octave sine level (0..100%, default 50%)
    FOLD,         // edit: pre-drive wavefolder depth (0..100%, default 0% = off)
    ASYM,         // edit: asymmetric saturator (−100%..+100%, default 0% = symmetric)
    NUM_PARAMS
  };

  // random-walk maximum deviation in cents at RATE AMT = 100% (full knob).
  // The detune never strays further from the DETUNE knob value than this.
  static constexpr float kRwMaxCents = 10.f;

  // Fixed sub-octave detune: the sub sine runs slightly flat (−4 cents) so
  // it "breathes" against the saws instead of locking to them. A few cents
  // at one octave below gives a slow, organic beat without sounding sloppy.
  static constexpr float kSubDetuneCents = -4.f;

  // LFO → FM depth on the sub phasor. The platform shape-LFO arrives in
  // lfo_ in (-1, 1); we scale it to ±5 cents of frequency modulation on the
  // sub-octave sine, giving it a subtle "wobble" the external LFO module
  // can't reach (it only sees the whole unit, not the internal sub voice).
  static constexpr float kSubLfoFmCents = 5.f;

  // Pre-drive wavefolder: before the tanh saturator, mix is folded into
  // [-1, 1] via  triangle(2x) — adding even harmonics and a "metallic"
  // edge. Depth is the FOLD knob (0 = off, 1 = full triangle fold).

  // Key-triggered A-S-R envelope (no sustain/level knob, all times fixed):
  //   attack   2 s, exponential fade 0 -> 1
  //   sustain  2 s, full level
  //   release  2 s, exponential fade 1 -> 0, then the unit goes silent
  // Triggered by noteOn only; noteOff is intentionally ignored. A second key
  // press while the sequence is still running restarts it from the top.
  //
  // The attack/release curves are first-order relaxations:
  //   env += (target - env) * a  with  a = 1 - exp(-5 / N),
  // where N is the phase length in samples. a is precomputed once in noteOn()
  // (never in the audio path); five halvings over the phase mean the level
  // covers ~99.3% of the transition by the phase boundary.
  static constexpr uint32_t kEgAttackSamples   = 2U * 48000U; // 2 s
  static constexpr uint32_t kEgSustainSamples  = 2U * 48000U; // 2 s
  static constexpr uint32_t kEgReleaseSamples  = 2U * 48000U; // 2 s

  enum EgPhase : uint8_t
  {
    kEgPhaseIdle = 0,
    kEgPhaseAttack,
    kEgPhaseSustain,
    kEgPhaseRelease
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float detune_cents; // -50.f .. +50.f, knob A
    float drive;        // 0.f .. 3.f, knob B
    float rw_amount;    // 0.f .. 1.f, edit: random-walk depth
    float rw_rate;      // 0.f .. 1.f, edit: random-walk speed
    float sub_level;    // 0.f .. 1.f, edit: sub-octave sine level
    float fold_depth;   // 0.f .. 1.f, edit: pre-drive wavefolder depth
    float asym;         // -1.f .. +1.f, edit: asymmetric saturator (0 = symmetric)

    void reset()
    {
      detune_cents = 0.f; // default: in tune
      drive = 1.f;        // default: unity drive
      rw_amount = 0.f;    // default: random walk off
      rw_rate = 0.5f;     // default: mid speed
      sub_level = 0.5f;   // default: half-level sub
      fold_depth = 0.f;   // default: wavefolder off (regression-safe)
      asym = 0.f;         // default: symmetric saturator (regression-safe)
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

    case SUB:
      // 0 .. 1023 -> 0.f .. 1.f sub-octave sine level (linear)
      params_.sub_level = param_10bit_to_f32(value);
      break;

    case FOLD:
      // 0 .. 1023 -> 0.f .. 1.f pre-drive wavefolder depth (linear)
      params_.fold_depth = param_10bit_to_f32(value);
      break;

    case ASYM:
      // 0 .. 1023 -> -1.f .. +1.f (0 = symmetric saturator)
      params_.asym = param_10bit_to_f32(value) * 2.f - 1.f;
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
    phasor_sub_ = 0.f;
    w0_ = 0.f;
    lfo_ = 0.f;
    rw_dev_cents_ = 0.f; // random-walk state starts centered
    rw_target_cents_ = 0.f; // random-walk target starts centered
    rw_target_update_counter_ = 0; // reset target update counter
    rw_max_dev_ = 0.f; // random-walk hard ceiling starts at 0 (AMT=0)
    rw_target_update_interval_ = 48000; // random-walk interval starts at 1s (default)
    eg_phase_ = kEgPhaseIdle; // no note has ever been triggered
    eg_elapsed_ = 0;
    env_ = 0.f; // silent until the first noteOn
    eg_armed_ = false;
    eg_alpha_a_ = 1.f - expf(-5.f / static_cast<float>(kEgAttackSamples));
    eg_alpha_r_ = 1.f - expf(-5.f / static_cast<float>(kEgReleaseSamples));
  }

  void reset() override final
  {
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    phasor_sub_ = 0.f;
    rw_dev_cents_ = 0.f; // re-center the random walk on stop
    rw_target_cents_ = 0.f; // re-center the random-walk target on stop
    rw_target_update_counter_ = 0; // reset target update counter
    // stop the note and kill any in-flight envelope (allNoteOff semantics)
    eg_phase_ = kEgPhaseIdle;
    eg_elapsed_ = 0;
    env_ = 0.f;
    eg_armed_ = false;
  }

  // ---- note handling ---------------------------------------------------------

  // Key press: trigger (or re-trigger) the A-S-R sequence from the top.
  void noteOn(uint8_t note, uint8_t velo) override final
  {
    (void)note;
    (void)velo;
    eg_phase_ = kEgPhaseAttack;
    eg_elapsed_ = 0;
    env_ = 0.f;
    eg_armed_ = true;
  }

  // Deliberately empty: the envelope runs its full 2 s + 2 s + 2 s cycle
  // regardless of when (or whether) the key is released.
  void noteOff(uint8_t note) override final
  {
    (void)note;
  }

  // All-notes-off: kill any in-flight envelope immediately (instant silence).
  void allNoteOff() override final
  {
    eg_phase_ = kEgPhaseIdle;
    eg_elapsed_ = 0;
    env_ = 0.f;
    eg_armed_ = false;
  }

  // audio processing callbacks

  // set frequency in digital w (w = f/samplerate, 0.5 is Nyquist)
  void setPitch(float w0)
  {
    w0_ = w0; // base phase increment, osc A uses this directly
    update_w0_b();
    update_w0_sub(); // sub factor is a compile-time constant, no param dependency
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
    // ---- Key-triggered A-S-R envelope -------------------------------------
    // The state machine advances one *sample* at a time (see the per-sample
    // block below), so eg_elapsed counts samples and each 2 s phase really
    // lasts 2 s regardless of the block size. The relaxation coefficient is
    // cached once per block; the phase only changes inside the sample loop.
    const float alpha_a = eg_alpha_a_;
    const float alpha_r = eg_alpha_r_;
    EgPhase eg_phase = eg_phase_;
    uint32_t eg_elapsed = eg_elapsed_;
    float env = eg_armed_ ? env_ : 0.f;

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

      // sub-octave phasor: base rate is w0_sub_ (f/2 * 2^(sub_detune/1200)),
      // modulated by the platform shape-LFO (±kSubLfoFmCents around w0_sub_).
      // The LFO is applied as a per-sample detune via a cached Taylor factor,
      // so it costs one extra multiply per sample (no powf, no Taylor per
      // sample — the LFO value is small, ±5 cents, so the 4th-order series
      // is more than accurate enough).
      {
        const float lfo_fm_cents = lfo_ * kSubLfoFmCents;
        const float x = lfo_fm_cents * 5.7762265046261084e-04f; // cents * ln(2)/1200
        const float lfo_factor = 1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
        phasor_sub_ += w0_sub_ * lfo_factor;
      }

      // wrap phasors into [0, 1)
      if (phasor_a_ >= 1.f)
        phasor_a_ -= 1.f;
      if (phasor_b_ >= 1.f)
        phasor_b_ -= 1.f;
      if (phasor_sub_ >= 1.f)
        phasor_sub_ -= 1.f;

      // generate sawtooth waves in -1..1
      const float saw_a = 2.f * phasor_a_ - 1.f;
      const float saw_b = 2.f * phasor_b_ - 1.f;

      // sub-octave sine in -1..1 (LUT lookup, ~4 VFP ops)
      const float sub = osc_sinf(phasor_sub_);

      // mix both voices equally
      float mix = 0.5f * (saw_a + saw_b);

      // pre-drive wavefolder (FOLD knob): scale mix by (1 + fold_depth) and
      // reflect into [-1, 1] (one reflection handles |x| up to 2). Blends
      // linearly from the raw mix (FOLD=0) to the fully folded signal (FOLD=1).
      // Adds even harmonics and a "metallic" edge before the tanh saturator.
      if (p.fold_depth > 0.f)
      {
        const float x_fold = mix * (1.f + p.fold_depth);
        const float ax = x_fold < 0.f ? -x_fold : x_fold;
        const float folded = (ax > 1.f)
            ? ((x_fold > 0.f) ? (2.f - x_fold) : (-2.f - x_fold))
            : x_fold;
        mix += p.fold_depth * (folded - mix);
      }

      // soft-clip saturator: y = (1 - d^2) * tanh(d / (1 - d^2)), d in (-1, 1)
      // clip the drive-scaled signal into the stable range first
      float d = mix * drive;
      if (d > 0.9f)
        d = 0.9f;
      else if (d < -0.9f)
        d = -0.9f;

      // asymmetric gain: scale d differently for positive / negative halves
      // to generate even harmonics ("warm", "brassy" tone). asym=0 -> unity
      // gain on both halves (regression-safe: original symmetric tanh).
      // asym>0 -> positive half driven harder, negative half softer.
      // asym<0 -> vice versa.
      const float asym_gain = (d >= 0.f) ? (1.f + p.asym) : (1.f - p.asym);
      float d_a = d * asym_gain;
      // re-clamp after asymmetric scaling: |d_a| can exceed 0.9 when
      // |asym| > 0 and |d| is near the 0.9 limit (e.g. d=0.9, asym=+0.5
      // -> d_a=1.35). This keeps the tanh argument in the stable range.
      if (d_a > 0.9f)
        d_a = 0.9f;
      else if (d_a < -0.9f)
        d_a = -0.9f;
      const float one_minus_d2 = 1.f - d_a * d_a;
      // fastertanh2f (Padé [3/3], ~6 VFP ops) replaces the former Padé [5/5]
      // soft_tanhf (~14 VFP ops). Max |error| over the full operating range
      // x in [-4.68, 4.68] is ~2e-3 (-54 dBFS) — inaudible for a saturator
      // (the error appears only as a very slight rounding of the highest
      // harmonics at full drive). The output of fastertanh2f is bounded in
      // (-1, 1), and (1-d^2) < 1, so y stays in (-1, 1): the 0.9f output
      // gain below still has headroom (max |y*0.9| < 0.9).
      const float y = one_minus_d2 * fastertanh2f(d_a / one_minus_d2);

      // ---- advance the A-S-R envelope by one sample ----
      // (per-sample so the 2 s phases are sample-accurate at any block size)
      if (eg_armed_)
      {
        switch (eg_phase)
        {
        case kEgPhaseAttack:
          // exponential fade 0 -> 1: env += (1 - env) * alpha_a
          env += (1.f - env) * alpha_a;
          if (++eg_elapsed >= kEgAttackSamples)
          {
            eg_elapsed = 0;
            eg_phase = kEgPhaseSustain;
          }
          break;
        case kEgPhaseSustain:
          env = 1.f;
          if (++eg_elapsed >= kEgSustainSamples)
          {
            eg_elapsed = 0;
            eg_phase = kEgPhaseRelease;
          }
          break;
        case kEgPhaseRelease:
          // exponential fade 1 -> 0: env += (0 - env) * alpha_r
          env -= env * alpha_r;
          if (++eg_elapsed >= kEgReleaseSamples)
          {
            eg_phase = kEgPhaseIdle;
            env = 0.f;
            eg_armed_ = false; // sequence finished, silent until next noteOn
          }
          break;
        default: // kEgPhaseIdle
          env = 0.f;
          break;
        }
      }
      else
      {
        env = 0.f;
      }

      // scale output to a musical level (saturator max output is ~0.76)
      // and apply the key-triggered A-S-R envelope level.
      //
      // Sub-octave sine is mixed in parallel with the saturator (it bypasses
      // the soft-clip on purpose: low-frequency sine through tanh would be
      // heavily attenuated). Per-branch gains are chosen so the worst-case
      // peak stays below 0.9:
      //   |y * 0.5f|   < 0.5  (saturator output bounded in (-1, 1))
      //   |sub * 0.4f| < 0.4  (sine bounded in [-1, 1])
      //   |sum|       < 0.9
      // At the default SUB = 50%, the sub branch is at 0.2 amplitude, which
      // is a balanced "body" level under the saturated saw stack.
      out[0] = (y * 0.5f + sub * p.sub_level * 0.4f) * env;
    }

    // write the envelope state machine back for the next block
    eg_phase_ = eg_phase;
    eg_elapsed_ = eg_elapsed;
    env_ = env;
  }

private:
  // cached derived coefficients, recomputed only when inputs change
  // (setPitch / setParameter(SHAPE)) to keep the audio path lightweight
  float w0_b_ = 0.f;
  float w0_sub_ = 0.f;

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

  // recompute w0_sub_ = w0_ * 2^((kSubDetuneCents)/1200) — the sub-octave
  // phasor's phase increment, cached at setPitch() (called every block from
  // unit_render). The sub runs half the base rate (f/2 = one octave down)
  // plus a fixed kSubDetuneCents detune. Because the detune is a compile-
  // time constant, the factor 2^(kSubDetuneCents/1200) is folded into a
  // single literal: 0.5 * 2^(-4/1200) ≈ 0.49885 (precomputed, so the audio
  // path is one multiply, no Taylor series needed per sample).
  void update_w0_sub()
  {
    w0_sub_ = w0_ * 0.4988460883f; // 0.5 * 2^(-4/1200)
  }

  Params params_;
  float w0_;
  float lfo_;

  // local variables related to audio processing
  float phasor_a_;
  float phasor_b_;
  float phasor_sub_; // sub-octave sine phasor (advances at w0/2)

  // key-triggered A-S-R envelope state (see the class-top comment for timing)
  EgPhase eg_phase_ = kEgPhaseIdle;
  uint32_t eg_elapsed_ = 0;
  float env_ = 0.f;
  bool eg_armed_ = false;
  // first-order relaxation coefficients for attack/release, precomputed in init()
  float eg_alpha_a_ = 0.f;
  float eg_alpha_r_ = 0.f;
};
