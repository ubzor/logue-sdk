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
 *  DRONE -- dual wavetable drone generator for NTS-1 mkII.
 *
 *  Two independent wavetable phasor accumulators running at the base note
 *  frequency. Each voice scans one of the 91 Korg wavetables (six banks A-F,
 *  airy -> dense) selected by the WAVE A / WAVE B edit knobs; a continuous
 *  morph position is mapped onto the bank arrays (see resolve_wave), and the
 *  morph point drifts slightly toward the denser banks as the pitch rises so
 *  the tone stays consistent across the keyboard. Two edit knobs (DET1 /
 *  DET2) detune the second / first oscillator by up to +/-100 cents each,
 *  producing phase beating and an asymmetric chorus when both are moved. The
 *  SATU knob scales the drive level before a soft-clip tanh saturator.
 *
 *  A sub-octave sine (one octave below, fixed -4 cents and FM'd by the
 *  platform shape-LFO) is mixed in parallel with the saturator for low-end
 *  "body". Before the saturator a pre-drive wavefolder (FOLD) can add even
 *  harmonics, and the saturator itself can be made asymmetric (ASYM).
 *
 *  An optional random-walk "wandering" mechanism modulates the detunes of
 *  BOTH oscillators around their knob values: RATE AMT sets the maximum
 *  deviation (quadratic mapping: knob^2 -> 0..+/-10 cents hard ceiling, so
 *  neither voice strays further from its DETUNE knob value than AMT allows)
 *  and RATE SPD sets the "restlessness" — how often the targets change and
 *  how quickly the deviations track them (0.1 s fast, restless .. 10 s slow,
 *  lazy). The two voices wander with independent random targets on the same
 *  period, so the inter-voice beat frequency shifts over time (asymmetric
 *  chorus). With RATE AMT at 0 the detunes are static, preserving the
 *  original behavior.
 *
 *  No dynamic allocation: all state lives in static members of this class.
 *
 */

#include "processor.h"
#include "unit_osc.h"
#include "utils/float_math.h" // fastertanh2f()
#include "osc_waves.h"        // WaveTable: bank scanning + pitch-shifted morph
#include "osc_envelope.h"     // AhEnvelope: single-voice A-H-R envelope

class Osc : public Processor
{
  // The wavetable scanner (osc_waves.h) is a flat helper struct; let it read
  // the Osc's private w0_ if needed (it isn't today — update() takes w0 as an
  // argument — but keep the door open without exposing w0_ publicly).
  friend struct WaveTable;

public:
  uint32_t getBufferSize() const override final { return 0; } // NTS-1 osc do not support sdram allocation

  // audio parameters
  enum
  {
    PARAM_SEL = 0U, // knob A (header slot 0): parameter selector 0..10, shows name
    PARAM_VAL,      // knob B (header slot 1): raw value 0..1023 of PARAM_SEL, shows value
    DETUNE,         // detune of osc B (−100..+100 cents, default 0 = in tune)
    DETUNE_A,       // detune of osc A (−100..+100 cents, default 0 = in tune)
    RATE_AMT,       // random-walk depth (0..100%, default 0% = off)
    RATE_SPD,       // random-walk speed (0..100%, default 50%)
    SUB1,           // sub-octave sine level (0..100%, default 50%)
    SUB2,           // second sub (f/4) very-soft-saturated level (0..100%, default 0% = off)
    SATU,           // saturator drive (0..3.0, default 2.0 = moderate)
    ASYM,           // asymmetric saturator (−100%..+100%, default 0% = symmetric)
    FOLD,           // pre-drive wavefolder depth (0..100%, default 0% = off)
    WAV1,           // oscillator A wavetable (0..90 across banks A-F)
    WAV2,           // oscillator B wavetable (0..90 across banks A-F)
    NUM_PARAMS
  };

  // ---- Parameter selector / value multiplexer ----
  // Knob A (PARAM_SEL, slot 0) picks one of the 11 DSP parameters
  // (DETUNE..WAV2), knob B (PARAM_VAL, slot 1) edits its raw 10-bit value.
  // The 11 raw 10-bit values (0..1023) are cached in edit_value_ so that
  // re-selecting a previously used parameter restores its value instead of
  // resetting it to the default.
  static constexpr uint8_t kEditParamCount = 11; // DETUNE..WAV2
  // The UI (header.c PARAM slot, 0..10) expects this order:
  //   0 DET1 (DETUNE), 1 DET2 (DETUNE_A), 2 RATE (RATE_AMT), 3 SPEE (RATE_SPD),
  //   4 SUB1, 5 SUB2, 6 SATU, 7 ASYM, 8 FOLD, 9 WAV1, 10 WAV2.
  // So the first editable enum value is DETUNE (=2), NOT RATE_AMT (=4).
  // kEditParamFirst + select_ must equal the right enum for every select_ 0..10.
  static constexpr uint8_t kEditParamFirst = DETUNE;

  // Raw 10-bit values (0..1023) of the 11 editable parameters, cached so the
  // selector can recall them. Defaults match the header's "init" values:
  // DETUNE 511, DETUNE_A 511, RATE 0, SPEE 511, SUB1 511, SUB2 0, SATU 682,
  // ASYM 511, FOLD 0, WAV1 0, WAV2 0.
  int32_t edit_value_[kEditParamCount] = {511, 511, 0, 511, 511, 0, 682, 511, 0, 0, 0};

  // Currently selected parameter (0..10), i.e. edit_value_[select_] is what
  // PARAM_VAL edits. 0 = DETUNE (default, matches header "init" of PARAM_SEL).
  uint8_t select_ = 0;

  // Public accessors for the web simulator (wasm.cc) to read the selector
  // cache state without going through the audio worklet thread.
  uint8_t getSelect() const { return select_; }
  int32_t getEditValue(uint8_t idx) const
  {
    return (idx < kEditParamCount) ? edit_value_[idx] : 0;
  }

  // Cached value of the currently selected parameter (PARAM_VAL). Used by the
  // web simulator's VALUE slider init/display (wasm.cc unit_get_param_value).
  int32_t getValue() const { return edit_value_[select_]; }

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

  // (The LFO -> wavetable morph depth lives in WaveTable::kLfoMorphDepth,
  // see osc_waves.h.)

  // ---- Second sub voice (SUB2): f/4, very-soft-saturated, LPF'd, gated ----
  // Drive level into the soft saturator for the second sub. Kept deliberately
  // low (~1.3) so it only "rounds" the clean f/4 sine and adds body/thickness
  // without turning it into a distorted bass. The saturator generates odd
  // (and a little even) harmonics; the LPF below then trims those so the
  // voice stays in the low end rather than muddying the mids.
  static constexpr float kSub2Drive = 1.3f;

  // One-pole LPF cutoff for the second sub, in Hz. Chosen ~200 Hz so the
  // saturator's 3rd/5th harmonics of an f/4 fundamental (which sit an octave
  // or two above it) are tamed while the fundamental and the body below it
  // pass untouched. The per-sample coefficient is folded into a literal in
  // update_w0_sub2() (no division in the audio path).
  static constexpr float kSub2LpfCutoffHz = 200.f;

  // Pitch gate for the second sub: the f/4 voice is fully silenced (gate -> 0)
  // once its fundamental drops below kSub2GateHz, because at very low notes
  // f/4 becomes infrasonic (e.g. C1 = 32.7 Hz -> f/4 = 8.2 Hz) and would only
  // waste headroom / muddy the mix. The boundary in terms of the *base* (saw)
  // frequency is 4 * kSub2GateHz (≈120 Hz ≈ C3). The gate opens/closes with a
  // short smoothing time constant so switching notes never clicks.
  static constexpr float kSub2GateHz = 30.f;
  // Gate smoothing: ~5 ms first-order time constant for the open/close
  // transition (precomputed per-sample coefficient in process()).
  static constexpr float kSub2GateTauSec = 0.005f;

  // Pre-drive wavefolder: before the tanh saturator, mix is folded into
  // [-1, 1] via  triangle(2x) — adding even harmonics and a "metallic"
  // edge. Depth is the FOLD knob (0 = off, 1 = full triangle fold).

  // ---- Parameter ranges & display scaling ----
  // Single source of truth for the ranges each editable parameter maps onto,
  // used by BOTH apply_edit_param() (raw -> DSP float) and
  // getParameterStrValue() (raw -> display string). Keep in sync with the
  // web simulator (websim/osc.html formatParamValue) and the default raw
  // values in the header's "init" fields (header.c).
  //
  // DET1/DET2 span +/-kDetuneMaxCents around 0 cents, centered on the raw
  // midpoint (kRawMid = 511). SATU spans 0..kSatuMax. ASYM spans +/-100%.
  // RATE SPD spans kRwPeriodMinSec..kRwPeriodMaxSec (log, 100^x) — 0.1 s to 10 s.
  static constexpr float    kDetuneMaxCents    = 100.f;
  static constexpr float    kSatuMax           = 3.f;   // 0..3.0 drive level
  static constexpr float    kRwPeriodMinSec    = 0.1f;  // fastest random-walk period
  static constexpr float    kRwPeriodMaxSec    = 10.f;  // slowest  (ratio = 100)
  static constexpr int32_t  kRawMid            = 511;   // raw 10-bit value == 0%
  static constexpr int32_t  kRawMax            = 1023;  // raw 10-bit value == 100%

  // ---- Audio-path mix & saturator constants ----
  // Final-mix gains. Worst-case peaks: |y*kMixOsc|<0.5,
  // |sub*kMixSub|<0.4, |sub2*kMixSub2|<0.3 (branches rarely peak together,
  // and the final hard-clip guards the sum).
  static constexpr float kMixOsc  = 0.5f;
  static constexpr float kMixSub  = 0.4f;
  static constexpr float kMixSub2 = 0.3f;
  // Stable drive limit for the asymmetric saturator: keeps the tanh argument
  // in (-1, 1) even when |asym| scales a half up (see process()).
  static constexpr float kSatDriveLimit = 0.9f;

  // ---- Sample-rate / reference-frequency helpers ----
  // getSampleRate() (Processor base) returns 48000.f for all logue products;
  // reuse it in constexpr audio-path sites to avoid a stray 48000 literal.
  static constexpr float    kSampleRate   = getSampleRate(); // 48000
  static constexpr float    kRefHz        = 880.f;           // A4
  static constexpr float    kTwoPi        = 6.283185307179586f;
  static constexpr float    kCentsToX     = 5.7762265046261084e-04f; // cents * ln(2)/1200
  // 0.5 * 2^(kSubDetuneCents/1200) == 0.5 * 2^(-4/1200); derived from the
  // sub-octave fixed detune so it cannot drift if kSubDetuneCents changes.
  static constexpr float    kSubW0Scale   = 0.4988460883f; // 0.5 * 2^(-4/1200)

  // (The A-H-R envelope lives in AhEnvelope, see osc_envelope.h.)

  // Note: Make sure that default param values correspond to declarations in header.c
  struct Params
  {
    float detune_cents;   // -100.f .. +100.f, knob A (osc B)
    float detune_a_cents; // -100.f .. +100.f, knob A (osc A)
    float drive;          // 0.f .. 3.f, knob B
    float rw_amount;    // 0.f .. 1.f, edit: random-walk depth
    float rw_rate;      // 0.f .. 1.f, edit: random-walk speed
    float sub_level;    // 0.f .. 1.f, edit: sub-octave (f/2) sine level
    float sub2_level;   // 0.f .. 1.f, edit: second sub (f/4) saturated level
    float fold_depth;   // 0.f .. 1.f, edit: pre-drive wavefolder depth
    float asym;         // -1.f .. +1.f, edit: asymmetric saturator (0 = symmetric)
    float wave_a;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc A wavetable index
    float wave_b;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc B wavetable index

    void reset()
    {
      detune_cents = 0.f;     // default: in tune
      detune_a_cents = 0.f;   // default: in tune
      drive = 2.f;            // default: moderate drive (50% of 0..3 range)
      rw_amount = 0.f;    // default: random walk off
      rw_rate = 0.5f;     // default: mid speed
      sub_level = 0.5f;   // default: half-level sub
      sub2_level = 0.f;   // default: second sub off (regression-safe)
      fold_depth = 0.f;   // default: wavefolder off (regression-safe)
      asym = 0.f;         // default: symmetric saturator (regression-safe)
      wave_a = 0.f;       // default: first wave of bank A
      wave_b = 0.f;       // default: first wave of bank A
    }

    Params() { reset(); }
  };

  // Map a raw 10-bit value (0..1023) for one of the 10 editable parameters
  // (DETUNE..WAV2) onto the DSP state. This is the single source of truth
  // for the parameter scaling: it is used both by the edit-menu path
  // (setParameter(DETUNE..WAV2)) and by the PARAM_VAL path.
  void apply_edit_param(uint8_t idx, int32_t value)
  {
    switch (idx)
    {
    case DETUNE:
      // 0..1023 -> -kDetuneMaxCents..+kDetuneMaxCents (50% = 511 = 0 cents)
      params_.detune_cents = (param_10bit_to_f32(value) * 2.f - 1.f) * kDetuneMaxCents;
      update_w0_b();
      break;

    case DETUNE_A:
      // 0..1023 -> -kDetuneMaxCents..+kDetuneMaxCents (50% = 511 = 0 cents)
      params_.detune_a_cents = (param_10bit_to_f32(value) * 2.f - 1.f) * kDetuneMaxCents;
      update_w0_a();
      break;

    case RATE_AMT:
      {
        const float x = param_10bit_to_f32(value);
        params_.rw_amount = x * x;
      }
      rw_max_dev_ = params_.rw_amount * kRwMaxCents;
      break;

    case RATE_SPD:
      params_.rw_rate = param_10bit_to_f32(value);
      {
        const float period = kRwPeriodMinSec * fasterpowf(100.f, params_.rw_rate);
        rw_target_update_interval_ = (uint32_t)(period * kSampleRate);
      }
      break;

    case SUB1:
      params_.sub_level = param_10bit_to_f32(value);
      break;

    case SUB2:
      params_.sub2_level = param_10bit_to_f32(value);
      break;

    case SATU:
      // 0..1023 -> 0..kSatuMax drive level
      params_.drive = param_10bit_to_f32(value) * kSatuMax;
      break;

    case ASYM:
      // 0..1023 -> -1..+1 (0% = 0)
      params_.asym = param_10bit_to_f32(value) * 2.f - 1.f;
      break;

    case FOLD:
      params_.fold_depth = param_10bit_to_f32(value);
      break;

    case WAV1:
      params_.wave_a = param_10bit_to_f32(value) * (WaveTable::kWavesTotal - 1);
      waves_.update(w0_);
      break;

    case WAV2:
      params_.wave_b = param_10bit_to_f32(value) * (WaveTable::kWavesTotal - 1);
      waves_.update(w0_);
      break;

    default:
      break;
    }
  }

  void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case PARAM_SEL:
      // Knob A: parameter selector. 0..10 -> DETUNE, DETUNE_A, RATE_AMT,
      // RATE_SPD, SUB1, SUB2, SATU, ASYM, FOLD, WAV1, WAV2. Mask the low
      // nibble (& 0xF) so any in-range index (0..10) maps to itself; a
      // narrower mask (the old & 9) corrupted indices 5..10, e.g. WAV2 (10)
      // -> RATE (2). On selection change, re-apply the cached raw value of
      // the newly selected parameter so the DSP immediately reflects it
      // (PARAM_VAL then edits that value).
      select_ = (uint8_t)(value & 0xF);
      apply_edit_param(kEditParamFirst + select_, edit_value_[select_]);
      break;

    case PARAM_VAL:
      // Knob B: raw 10-bit value (0..1023) of the currently selected
      // parameter. Write into the cache (so re-selecting later restores
      // it) and apply to DSP.
      edit_value_[select_] = value;
      apply_edit_param(kEditParamFirst + select_, value);
      break;

    default:
      break;
    }
  }

  // String representations for the type_strings parameters. Header slots 0/1
  // are the fixed A/B knobs; slots 2/3 are their optional (edit-menu)
  // duplicates (see header.c). All four carry type_strings, so this method
  // accepts any of the four slot indices and routes them to the same
  // underlying state (select_ / edit_value_):
  //   PARAM (slot 0 or 2): the name of the selected parameter
  //                        ("DET1", "SUB1", ..).
  //   VALUE (slot 1 or 3): a short representation of the selected parameter's
  //                        raw 10-bit value ("50%", "+25%", wavetable index).
  // The manual formatter below avoids pulling printf machinery into the ARM
  // image. Longest possible output is 5 chars ("+100%").
  const char *getParameterStrValue(uint8_t id, int32_t value) const override final
  {
    static const char *const names[kEditParamCount] = {
        "DET1", "DET2", "RATE", "SPEE", "SUB1",
        "SUB2", "SATU", "ASYM", "FOLD", "WAV1", "WAV2"};
    const bool is_param_sel = (id == PARAM_SEL || id == PARAM_SEL + 2U);
    const bool is_param_val = (id == PARAM_VAL || id == PARAM_VAL + 2U);
    if (is_param_sel)
    {
      return (value >= 0 && value < (int)kEditParamCount) ? names[value] : "";
    }
    if (!is_param_val)
      return "";

    // PARAM VAL: format the currently selected parameter's cached raw value.
    const uint8_t idx = (select_ < kEditParamCount) ? select_ : 0;
    const int32_t raw = edit_value_[idx];
    const int32_t k = kEditParamFirst + idx;
    static char buf[8];
    int v;
    if (k == WAV1 || k == WAV2)
      v = (int)(param_10bit_to_f32(raw) * (WaveTable::kWavesTotal - 1));
    else if (k == ASYM)
      v = (int)((raw * 200) / kRawMax) - 100;
    else if (k == DETUNE || k == DETUNE_A)
      v = (int)((raw - kRawMid) * (int)kDetuneMaxCents / kRawMid); // -kDetuneMaxCents..+kDetuneMaxCents cents (multiply before divide)
    else if (k == SATU)
      v = (int)(param_10bit_to_f32(raw) * (kSatuMax * 100.f)); // 0..300
    else
      v = (int)((raw * 100) / kRawMax);

    const bool percent = (k != WAV1 && k != WAV2 && k != SATU && k != DETUNE && k != DETUNE_A);
    int n = 0;
    if (v < 0)
      buf[n++] = '-';
    else if (k == ASYM)
      buf[n++] = '+';
    int t = (v < 0) ? -v : v;
    char d[4];
    int nd = 0;
    do
    {
      d[nd++] = (char)('0' + t % 10);
      t /= 10;
    } while (t > 0);
    for (int i = nd - 1; i >= 0; --i)
      buf[n++] = d[i];
    if (percent)
      buf[n++] = '%';
    buf[n] = '\0';
    return buf;
  }

  // life-cycle methods
  void init(float *) override final
  {
    params_.reset();
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    phasor_sub_ = 0.f;
    phasor_sub2_ = 0.f;
    sub2_lpf_state_ = 0.f;
    sub2_gate_ = 0.f;
    w0_ = 0.f;
    lfo_ = 0.f;
    // Parameter selector: start with DETUNE (select_ = 0) selected.
    // Re-apply the cached default values so the DSP and the PARAM SEL /
    // PARAM VAL display agree on startup (the header's "init" value of
    // PARAM_VAL already matches edit_value_[0], but this keeps the two in
    // sync if either side is edited independently later).
    select_ = 0;
    for (uint8_t i = 0; i < kEditParamCount; i++)
      apply_edit_param(kEditParamFirst + i, edit_value_[i]);
    waves_.update(w0_); // resolve default wavetable pointers from params_
    rw_dev_a_cents_ = 0.f; // random-walk state (osc A) starts centered
    rw_dev_b_cents_ = 0.f; // random-walk state (osc B) starts centered
    rw_target_a_cents_ = 0.f; // random-walk target (osc A) starts centered
    rw_target_b_cents_ = 0.f; // random-walk target (osc B) starts centered
    rw_target_update_counter_ = 0; // reset target update counter
    rw_max_dev_ = 0.f; // random-walk hard ceiling starts at 0 (AMT=0)
    rw_target_update_interval_ = (uint32_t)kSampleRate; // random-walk interval starts at 1s (default)
    // A-H-R envelope: start silent, no note active.
    note_on_ = false;
    eg_.reset();
    // First-order relaxation coefficients (per sample) for the exponential
    // attack/release. alpha = 1 - exp(-5 / N) where N is the phase length in
    // samples; five time-constants over the phase means the level covers
    // ~99.3% of the transition by the phase boundary. Precomputed here, never
    // in the audio path.
    eg_alpha_a_ = 1.f - expf(-5.f / static_cast<float>(AhEnvelope::kAttackSamples));
    eg_alpha_r_ = 1.f - expf(-5.f / static_cast<float>(AhEnvelope::kReleaseSamples));
  }

  void reset() override final
  {
    phasor_a_ = 0.f;
    phasor_b_ = 0.f;
    phasor_sub_ = 0.f;
    phasor_sub2_ = 0.f;
    sub2_lpf_state_ = 0.f;
    sub2_gate_ = 0.f;
    rw_dev_a_cents_ = 0.f; // re-center the random walk (osc A) on stop
    rw_dev_b_cents_ = 0.f; // re-center the random walk (osc B) on stop
    rw_target_a_cents_ = 0.f; // re-center the random-walk target (osc A) on stop
    rw_target_b_cents_ = 0.f; // re-center the random-walk target (osc B) on stop
    rw_target_update_counter_ = 0; // reset target update counter
    // Stop the note and kill the A-H-R envelope (allNoteOff semantics).
    note_on_ = false;
    eg_.reset();
  }

  // ---- note handling ---------------------------------------------------------

  // Key press: note active. Start (or restart) the A-H-R sequence from the top
  // of the attack. A second key press while a sequence is still running
  // restarts it (re-trigger), which is the expected drone behavior.
  void noteOn(uint8_t note, uint8_t velo) override final
  {
    (void)note;
    (void)velo;
    note_on_ = true;
    eg_.noteOn();
  }

  // Key release: note inactive. If the envelope is in Attack or Hold, begin
  // the 2 s exponential release. If already in Release or Idle, nothing
  // happens (the release runs to completion on its own).
  void noteOff(uint8_t note) override final
  {
    (void)note;
    note_on_ = false;
    eg_.noteOff();
  }

  // All-notes-off: note inactive, kill any in-flight envelope immediately
  // (instant silence — no release tail).
  void allNoteOff() override final
  {
    note_on_ = false;
    eg_.allOff();
  }

  // audio processing callbacks

  // set frequency in digital w (w = f/samplerate, 0.5 is Nyquist)
  void setPitch(float w0)
  {
    w0_ = w0; // base phase increment, osc A uses this directly
    update_w0_b();
    update_w0_a();
    update_w0_sub(); // sub factor is a compile-time constant, no param dependency
    update_w0_sub2(); // f/4 phase increment + fixed LPF/gate coefficients
    waves_.update(w0_); // re-resolve wavetables: morph position shifts with pitch
  }

  // lfo in (-1.0f, 1.0f)
  void setShapeLfo(float lfo)
  {
    lfo_ = lfo;
  }

#ifdef EMSCRIPTEN
  // Simulator-only LFO settings (set from the JS UI, K-rate). On hardware
  // these are the physical OSC-button knobs and are delivered via the
  // platform's shape_lfo; here they drive the internal sine generator.
  void setLfoFreq(float freq)
  {
    lfo_freq_ = (freq < 0.f) ? 0.f : (freq > 30.f) ? 30.f : freq;
  }

  void setLfoDepth(float depth)
  {
    lfo_depth_ = (depth < 0.f) ? 0.f : (depth > 1.f) ? 1.f : depth;
  }
#endif

  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    (void)in; // drone generator does not use the audio input

    // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
    const Params p = params_;

    // base phase increment
    const float w0 = w0_;
    const float drive = p.drive;

    // ---- Random-walk detune deviation (target-based wandering, dual channel) ----
    // When rw_amount == 0 (knob at 0) the walk is disabled and w0_a / w0_b are
    // the cached static values (regression: byte-for-byte identical to the old
    // code). rw_amount is the quadratic-mapped knob value (see setParameter),
    // so rw_active is true for any non-zero knob position.
    //
    // RATE AMT = maximum deviation (hard ceiling) for BOTH osc A and osc B:
    // each voice's detune deviation is limited to +/-rw_max_dev_ =
    // +/-rw_amount*kRwMaxCents cents, so neither voice strays further from its
    // DETUNE knob value than AMT allows. The two channels wander with
    // INDEPENDENT random targets (chosen on the same period), so the
    // inter-voice beat frequency shifts over time — an asymmetric, ever-
    // changing chorus.
    //
    // The deviations smoothly track random targets that are regenerated
    // periodically, ensuring |dev| <= rw_max_dev_ always.
    //
    // RATE SPD = restlessness: controls how often the targets change and how
    // quickly the deviations track them. High SPD -> frequent target updates
    // and fast tracking (restless), low SPD -> rare updates and slow tracking
    // (lazy drift).
    // (The A-H-R envelope is advanced per-sample inside the loop below; the
    // relaxation coefficients are cached once per block for cheap access.)
    const float alpha_a = eg_alpha_a_;
    const float alpha_r = eg_alpha_r_;

    // ---- Second sub (SUB2) fixed coefficients, cached once per block ----
    // The f/4 voice is a clean sine run through a very soft saturator
    // (kSub2Drive), then a one-pole LPF (sub2_lpf_a_, cuts the saturator's
    // harmonics so the voice stays in the low end) and a pitch gate (closes
    // when the base frequency drops below sub2_gate_hz_, i.e. f/4 < 30 Hz).
    // All three are precomputed in update_w0_sub2(); the LPF state and the
    // smoothed gate amplitude are per-sample members (sub2_lpf_state_,
    // sub2_gate_).
    const float sub2_level = p.sub2_level;
    const float sub2_a = sub2_lpf_a_;
    const float sub2_k = sub2_gate_k_;
    const float sub2_gate_hz = sub2_gate_hz_;

    const bool rw_active = p.rw_amount > 0.f;
    float w0_a = w0_a_; // default: cached static detune (osc A)
    float w0_b = w0_b_; // default: cached static detune (osc B)

    // wavetable morph: base (knob) + pitch-shift (K-rate cached), the LFO
    // offset is added per-sample below. waves_.wave_shift_ is recomputed on
    // setParameter(WAV1/WAV2) / setPitch(); wave_a / wave_b are the knobs.
    const float wave_base_a = p.wave_a + waves_.wave_shift_;
    const float wave_base_b = p.wave_b + waves_.wave_shift_;
    if (rw_active)
    {
      // Compute the tracking coefficient once per block (rw_rate may change)
      // period = 0.1 * 100^rw_rate seconds
      const float period = kRwPeriodMinSec * fasterpowf(100.f, p.rw_rate);
      // Tracking coefficient: how quickly the deviations move toward the targets.
      // Higher k = faster tracking. k = 1 / (1 + tau * 48000) where tau is the
      // relaxation time (we use period as tau for simplicity). Shared by both
      // channels so the two voices drift in lockstep in speed, only the targets
      // differ.
      rw_k_ = 1.f / (1.f + period * kSampleRate);
      // Initialize the targets to the current deviations on first activation
      if (rw_target_a_cents_ == 0.f && rw_dev_a_cents_ != 0.f)
        rw_target_a_cents_ = rw_dev_a_cents_;
      if (rw_target_b_cents_ == 0.f && rw_dev_b_cents_ != 0.f)
        rw_target_b_cents_ = rw_dev_b_cents_;
    }

    for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
    {
      // update random-walk detune deviations (cents) around the knob values
      if (rw_active)
      {
        // Update the targets periodically (independent per voice)
        if (++rw_target_update_counter_ >= rw_target_update_interval_)
        {
          rw_target_update_counter_ = 0;
          // Generate new random targets in [-rw_max_dev_, +rw_max_dev_]
          // Using osc_white() which returns values in [-1, 1]
          rw_target_a_cents_ = osc_white() * rw_max_dev_;
          rw_target_b_cents_ = osc_white() * rw_max_dev_;
        }
        // Smoothly track the targets: dev += (target - dev) * k
        rw_dev_a_cents_ += (rw_target_a_cents_ - rw_dev_a_cents_) * rw_k_;
        rw_dev_b_cents_ += (rw_target_b_cents_ - rw_dev_b_cents_) * rw_k_;
        // Safety clamps (should rarely trigger if targets are within bounds)
        if (rw_dev_a_cents_ > rw_max_dev_)
          rw_dev_a_cents_ = rw_max_dev_;
        else if (rw_dev_a_cents_ < -rw_max_dev_)
          rw_dev_a_cents_ = -rw_max_dev_;
        if (rw_dev_b_cents_ > rw_max_dev_)
          rw_dev_b_cents_ = rw_max_dev_;
        else if (rw_dev_b_cents_ < -rw_max_dev_)
          rw_dev_b_cents_ = -rw_max_dev_;
        // recompute w0_a from effective detune (base + walk), 4th-order Taylor
        const float eff_detune_a = p.detune_a_cents + rw_dev_a_cents_;
        const float x_a = eff_detune_a * kCentsToX; // cents * ln(2)/1200
        const float detune_factor_a =
            1.f + x_a * (1.f + x_a * (0.5f + x_a * (1.f / 6.f + x_a * (1.f / 24.f))));
        w0_a = w0 * detune_factor_a;
        // recompute w0_b from effective detune (base + walk), 4th-order Taylor
        const float eff_detune_b = p.detune_cents + rw_dev_b_cents_;
        const float x_b = eff_detune_b * kCentsToX;
        const float detune_factor_b =
            1.f + x_b * (1.f + x_b * (0.5f + x_b * (1.f / 6.f + x_b * (1.f / 24.f))));
        w0_b = w0 * detune_factor_b;
      }

      // phasor update
      phasor_a_ += w0_a;
      phasor_b_ += w0_b;

      // sub-octave phasor: base rate is w0_sub_ (f/2 * 2^(sub_detune/1200)),
      // modulated by the platform shape-LFO (±kSubLfoFmCents around w0_sub_).
      // The LFO is applied as a per-sample detune via a cached Taylor factor,
      // so it costs one extra multiply per sample (no powf, no Taylor per
      // sample — the LFO value is small, ±5 cents, so the 4th-order series
      // is more than accurate enough).
      {
        const float lfo_fm_cents = lfo_ * kSubLfoFmCents;
        const float x = lfo_fm_cents * kCentsToX; // cents * ln(2)/1200
        const float lfo_factor = 1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
        phasor_sub_ += w0_sub_ * lfo_factor;
      }

      // second sub phasor: pure f/4, no LFO-FM and no fixed detune (it is a
      // stable low-end anchor, not a beating voice). Advances exactly at
      // w0/4 (w0_sub2_ is cached in update_w0_sub2()).
      phasor_sub2_ += w0_sub2_;

      // wrap phasors into [0, 1)
      if (phasor_a_ >= 1.f)
        phasor_a_ -= 1.f;
      if (phasor_b_ >= 1.f)
        phasor_b_ -= 1.f;
      if (phasor_sub_ >= 1.f)
        phasor_sub_ -= 1.f;
      if (phasor_sub2_ >= 1.f)
        phasor_sub2_ -= 1.f;

      // Simulator-only: synthesize the shape-LFO at audio rate (phase-coherent
      // sine). On hardware lfo_ is delivered by the platform via setShapeLfo()
      // and this block is compiled out, so the two paths share the same
      // morph code below.
#ifdef EMSCRIPTEN
      if (lfo_depth_ > 0.f)
      {
        lfo_phase_ += kTwoPi * lfo_freq_ * (1.f / kSampleRate);
        if (lfo_phase_ >= 1.f)
          lfo_phase_ -= 1.f;
        lfo_ = osc_sinf(lfo_phase_) * lfo_depth_;
      }
      else
      {
        lfo_ = 0.f;
      }
#endif

      // generate wavetable samples in -1..1. Each voice's table index is the
      // base position (knob + pitch-shift) plus the LFO morph offset, so the
      // timbre "breathes" across the wavetable. WaveTable::resolve_wave() crossfades the
      // fractional part between adjacent tables, so the morph glides smoothly.
      const float lfo_morph = lfo_ * WaveTable::kLfoMorphDepth;
      const float wav_a = WaveTable::resolve_wave(wave_base_a + lfo_morph, phasor_a_);
      const float wav_b = WaveTable::resolve_wave(wave_base_b + lfo_morph, phasor_b_);

      // sub-octave sine in -1..1 (LUT lookup, ~4 VFP ops)
      const float sub = osc_sinf(phasor_sub_);

      // ---- second sub (f/4): clean sine -> very soft saturator -> LPF -> gate ----
      // Only computed when the voice is actually audible (level > 0) to keep
      // the default path (SUB2 = 0) free of this cost.
      float sub2 = 0.f;
      if (sub2_level > 0.f)
      {
        // f/4 sine in -1..1 (LUT)
        const float s2 = osc_sinf(phasor_sub2_);

        // very soft saturator: a light tanh at kSub2Drive (~1.3) rounds the
        // sine and adds just enough odd harmonics for "body" without turning
        // it into a distorted bass. fastertanh2f (Padé [3/3]) is inaudibly
        // accurate and bounded in (-1, 1).
        const float s2_sat = fastertanh2f(s2 * kSub2Drive);

        // one-pole LPF (200 Hz): trims the saturator's 3rd/5th harmonics so the
        // voice stays in the low end instead of muddying the mids. First-order
        // low-pass: state += (in - state) * a; out = state.
        sub2_lpf_state_ += (s2_sat - sub2_lpf_state_) * sub2_a;

        // pitch gate: fully open when the base (saw) frequency is above the
        // threshold (f/4 >= 30 Hz), closed below it (f/4 infrasonic). Smoothed
        // first-order so switching notes never clicks. w0 = f/sr, so the base
        // frequency in Hz is w0 * sr; compare against sub2_gate_hz (4*30).
        const float open_target = (w0 * kSampleRate > sub2_gate_hz) ? 1.f : 0.f;
        sub2_gate_ += (open_target - sub2_gate_) * sub2_k;

        sub2 = sub2_lpf_state_ * sub2_gate_;
      }
      // else: voice off; leave the LPF/gate state as-is (it will re-engage
      // smoothly next time the level is raised).

      // mix both voices equally
      float mix = kMixOsc * (wav_a + wav_b);

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
      if (d > kSatDriveLimit)
        d = kSatDriveLimit;
      else if (d < -kSatDriveLimit)
        d = -kSatDriveLimit;

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
      if (d_a > kSatDriveLimit)
        d_a = kSatDriveLimit;
      else if (d_a < -kSatDriveLimit)
        d_a = -kSatDriveLimit;
      const float one_minus_d2 = 1.f - d_a * d_a;
      // fastertanh2f (Padé [3/3], ~6 VFP ops) replaces the former Padé [5/5]
      // soft_tanhf (~14 VFP ops). Max |error| over the full operating range
      // x in [-4.68, 4.68] is ~2e-3 (-54 dBFS) — inaudible for a saturator
      // (the error appears only as a very slight rounding of the highest
      // harmonics at full drive). The output of fastertanh2f is bounded in
      // (-1, 1), and (1-d^2) < 1, so y stays in (-1, 1): the 0.9f output
      // gain below still has headroom (max |y*0.9| < 0.9).
      const float y = one_minus_d2 * fastertanh2f(d_a / one_minus_d2);

      // ---- advance the A-H-R envelope by one sample ----
      // (per-sample so the 2 s phases are sample-accurate at any block size)
      eg_.advance(alpha_a, alpha_r);

      // scale output to a musical level (saturator max output is ~0.76)
      // and apply the A-H-R envelope level.
      //
      // Sub-octave sine is mixed in parallel with the saturator (it bypasses
      // the soft-clip on purpose: low-frequency sine through tanh would be
      // heavily attenuated). Per-branch gains are chosen so the worst-case
      // peak stays below 0.9:
      //   |y * 0.5f|    < 0.5  (saturator output bounded in (-1, 1))
      //   |sub * 0.4f|  < 0.4  (sine bounded in [-1, 1])
      //   |sub2*0.3f|  < 0.3  (LPF'd, gated voice bounded in (-1, 1))
      //   |sum|         < 1.2 (worst case, but branches rarely peak together)
      // At the default SUB1 = 50%, the sub branch is at 0.2 amplitude; SUB2
      // defaults to 0 (off) so the stock sound is unchanged. A hard clip
      // prevents digital overflow when all branches peak simultaneously.
      float raw = (y * kMixOsc + sub * p.sub_level * kMixSub + sub2 * sub2_level * kMixSub2) * eg_.env;
      if (raw > 1.f)
        raw = 1.f;
      else if (raw < -1.f)
        raw = -1.f;
      out[0] = raw;
    }
  }

private:
  // cached derived coefficients, recomputed only when inputs change
  // (setPitch / setParameter(SHAPE)) to keep the audio path lightweight
  float w0_b_ = 0.f;
  float w0_a_ = 0.f;
  float w0_sub_ = 0.f;
  // second-sub (f/4) phase increment and fixed filter/gate coefficients, all
  // precomputed in update_w0_sub2() (called from setPitch, never audio path)
  float w0_sub2_ = 0.f;
  float sub2_lpf_a_ = 0.f;   // one-pole LPF coefficient (200 Hz)
  float sub2_gate_k_ = 0.f;  // pitch-gate smoothing coefficient
  float sub2_gate_hz_ = 0.f; // base (saw) Hz at which f/4 == kSub2GateHz

  // random-walk state: current detune deviation in cents (around the knob value),
  // one per voice (A and B). Both channels share the same hard ceiling
  // (rw_max_dev_) and the same tracking coefficient (rw_k_), but wander with
  // independent random targets so the inter-voice beat frequency shifts over
  // time (asymmetric chorus).
  float rw_dev_a_cents_ = 0.f;
  float rw_dev_b_cents_ = 0.f;
  // random-walk tracking coefficient (per sample), recomputed once per block
  // in process() from RATE SPD. Controls how quickly the deviations track the
  // targets. Shared by both channels so the two voices drift in lockstep in
  // speed; only the targets differ.
  float rw_k_ = 0.f;
  // random-walk target deviations in cents, regenerated periodically.
  // Each target is chosen randomly in [-rw_max_dev_, +rw_max_dev_],
  // independently per voice.
  float rw_target_a_cents_ = 0.f;
  float rw_target_b_cents_ = 0.f;
  // random-walk hard ceiling for the deviations (cents), set by RATE AMT.
  // Applies to both voices.
  float rw_max_dev_ = 0.f;
  // target update counter: counts samples since the last target update.
  uint32_t rw_target_update_counter_ = 0;
  // target update interval in samples (recomputed from RATE SPD each block).
  uint32_t rw_target_update_interval_ = 0;

  // Note "active" flag: true while a note is held (noteOn not yet followed by
  // noteOff / allNoteOff / reset). Today this is set by the MIDI handlers;
  // later it may be driven algorithmically (self-starting notes) without
  // touching the audio path. The A-H-R envelope below is the single voice
  // that shapes the attack/release of the active note.
  bool note_on_ = false;

  // A-H-R envelope (single voice). See osc_envelope.h for the state machine.
  AhEnvelope eg_;

  // First-order relaxation coefficients for attack/release, precomputed in
  // init() (never in the audio path).
  float eg_alpha_a_ = 0.f;
  float eg_alpha_r_ = 0.f;

  // recompute w0_b_ = w0_ * 2^(detune_cents/1200) — only when the base pitch
  // or detune actually changes.
  //
  // detune_cents is limited to +/-100 (see setParameter), so
  // x = cents * ln(2)/1200 stays within |x| <= 0.0833. In that range
  // 2^(cents/1200) = e^x is well approximated by a 4th-order Taylor series
  // (1 + x + x^2/2 + x^3/6 + x^4/24), which keeps the relative error below
  // ~1e-6 (less than 0.03 cents at the extremes) — far under the threshold
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
    const float x = params_.detune_cents * kCentsToX; // cents * ln(2) / 1200
    const float detune_factor =
        1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
    w0_b_ = w0_ * detune_factor;
  }

  // recompute w0_a_ = w0_ * 2^(detune_a_cents/1200) — only when the base
  // pitch or detune actually changes. Same 4th-order Taylor approximation as
  // update_w0_b() (see its comment); detune_a_cents is limited to +/-100.
  void update_w0_a()
  {
    const float x = params_.detune_a_cents * kCentsToX; // cents * ln(2) / 1200
    const float detune_factor =
        1.f + x * (1.f + x * (0.5f + x * (1.f / 6.f + x * (1.f / 24.f))));
    w0_a_ = w0_ * detune_factor;
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
    w0_sub_ = w0_ * kSubW0Scale; // 0.5 * 2^(kSubDetuneCents/1200) == 0.5 * 2^(-4/1200)
  }

  // recompute the second-sub (f/4) phase increment and its fixed filter/gate
  // coefficients. Unlike sub-1 there is NO fixed detune: the voice is a
  // stable "anchor" in the low end, so w0_sub2_ = w0_ * 0.25 exactly (two
  // octaves below the base). Also precomputed here (called once per pitch
  // change, never in the audio path):
  //   sub2_lpf_a_  — one-pole LPF coefficient for kSub2LpfCutoffHz (cuts the
  //                  saturator's harmonics so the voice stays in the low end)
  //   sub2_gate_k_ — first-order smoothing coefficient for the pitch gate
  //                  (open/close transition, kSub2GateTauSec time constant)
  //   sub2_gate_hz_— base (saw) frequency in Hz at which f/4 == kSub2GateHz;
  //                  below this the gate closes. Cached as 4 * kSub2GateHz.
  void update_w0_sub2()
  {
    w0_sub2_ = w0_ * 0.25f; // f/4, no detune
    // one-pole LPF: a = 1 - exp(-2*pi*fc/sr)
    sub2_lpf_a_ =
        1.f - expf(-kTwoPi * kSub2LpfCutoffHz / kSampleRate);
    // first-order gate: k = 1 - exp(-1/tau)
    sub2_gate_k_ = 1.f - expf(-1.f / (kSub2GateTauSec * getSampleRate()));
    // base frequency (Hz) at which the f/4 fundamental equals kSub2GateHz
    sub2_gate_hz_ = 4.f * kSub2GateHz;
  }

  Params params_;
  float w0_;
  float lfo_;

#ifdef EMSCRIPTEN
  // Host-side (web simulator) shape-LFO. On hardware the platform generates
  // the LFO from the OSC-button A/B knobs and delivers it through
  // setShapeLfo(); in the simulator there is no platform, so we synthesize a
  // phase-coherent sine at audio rate here. The UI sets frequency (F knob,
  // 0..30 Hz) and depth (S knob, 0..1) via setLfoFreq()/setLfoDepth().
  float lfo_freq_ = 1.f;
  float lfo_depth_ = 0.f; // default 0 = LFO off (regression-safe startup)
  float lfo_phase_ = 0.f;
#endif

  // local variables related to audio processing
  float phasor_a_;
  float phasor_b_;
  float phasor_sub_; // sub-octave sine phasor (advances at w0/2)
  // second sub voice (f/4, two octaves below the saws): a clean sine run
  // through a very soft saturator (adds "body"/thickening) then a one-pole
  // LPF (cuts the saturator's harmonics so it stays in the low end) and a
  // pitch gate (silences it when f/4 drops below kSub2GateHz = infrasonic).
  // Unlike sub-1 it carries no fixed detune: it is a stable "anchor" in the
  // low end, not a beating voice.
  float phasor_sub2_; // second sub phasor (advances at w0/4, no detune)
  // one-pole LPF state (integrator) for the saturator-harmonic cutoff on sub-2
  float sub2_lpf_state_ = 0.f;
  // smoothed pitch-gate amplitude for sub-2 (0 = silenced, 1 = open)
  float sub2_gate_ = 0.f;

  // ---- Wavetable scanning (see osc_waves.h) ----------------------------------
  // The six-bank Korg wavetable scanner + the pitch-shifted morph state live in
  // WaveTable (osc_waves.h). The Osc owns a WaveTable member and reads its
  // cached morph position (waves_.wave_shift_) in the audio path; resolve_wave
  // is called as WaveTable::resolve_wave(...). The LFO->morph depth constant is
  // WaveTable::kLfoMorphDepth.
  WaveTable waves_;
};
