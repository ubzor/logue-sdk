#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
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
 *  the tone stays consistent across the keyboard. The DETU edit knob
 *  (±10 cents, centered on 0) detunes A and B in OPPOSITE directions around
 *  the base note (A = +c/2, B = -c/2), producing a per-voice phase drift
 *  (inter-voice beat = c * f / 600) — ~4x stronger than an equivalent
 *  post-mix sum detune at the same knob value. The chord and sub voices
 *  stay at the base note (stable anchors). The SATU knob scales the drive
 *  level before a soft-clip tanh saturator.
 *
 *  A sub-octave sine (one octave below, fixed -4 cents and FM'd by the
 *  platform shape-LFO) is mixed in parallel with the saturator for low-end
 *  "body". Before the saturator a pre-drive wavefolder (FOLD) can add even
 *  harmonics, and the saturator itself can be made asymmetric (ASYM).
 *
 *  An optional random-walk "wandering" mechanism modulates the detunes of
 *  BOTH A and B oscillators (independent random targets on the same period,
 *  so the inter-voice beat frequency shifts over time — an asymmetric
 *  chorus). The walk works together with the static DET knob: DET sets the
 *  base chorus depth of the whole drone, the walk moves the A/B voices
 *  around the base note so the inter-voice beat drifts in time.
 *    RATE AMT  — maximum deviation (quadratic mapping: knob^2 -> 0..1
 *                normalized, scaled to +/-10 cents at the audio path, so
 *                neither voice strays further from its DETUNE knob value than
 *                AMT allows),
 *    RATE SPD  — how often the random targets are regenerated (0.1 s
 *                fast/restless .. 10 s slow/lazy, log mapping),
 *    RATE SMTH — how quickly the deviations track the targets (a time
 *                constant, 10 ms snappy .. 1.5 s glacial, log mapping).
 *  The two voices wander with independent random targets on the same period,
 *  so the inter-voice beat frequency shifts over time (asymmetric chorus).
 *  Keeping the time constant (SMTH) independent of the period (SPD) makes the
 *  audible depth stable regardless of SPEED. With RATE AMT at 0 the detunes
 *  are static, preserving the original behavior.
 *
 *  No dynamic allocation: all state lives in static members of this class.
 *
 */

#include "processor.h"
#include "unit_osc.h"
#include "utils/float_math.h" // fastertanh2f()
#include "osc_params.h"       // OscParams enum + constants + OscParamsData struct
#include "osc_waves.h"        // WaveTable: bank scanning + pitch-shifted morph
#include "osc_envelope.h"     // AhEnvelope: single-voice A-H-R envelope
#include "osc_saturator.h"    // Saturator: pre-drive wavefolder + asymmetric soft-clip
#include "osc_random_walk.h"  // RandomWalk: dual-channel detune wandering
#include "osc_sub2.h"         // Sub2Voice: f/4 saturated, LPF'd, gated sub voice
#include "osc_chord.h"        // ChordVoice: third oscillator at selectable interval
#include "osc_detune.h"       // Detune: cent-based pitch scaling (Taylor series)
#ifdef EMSCRIPTEN
#include "osc_lfo_sim.h"      // ShapeLfoSim: simulator-only shape-LFO synthesizer
#endif

class Osc : public Processor
{
  // The wavetable scanner (osc_waves.h) is a flat helper struct; let it read
  // the Osc's private w0_ if needed (it isn't today — update() takes w0 as an
  // argument — but keep the door open without exposing w0_ publicly).
  friend struct WaveTable;

public:
  uint32_t getBufferSize() const override final { return 0; } // NTS-1 osc do not support sdram allocation

  // ---- Audio parameters ----------------------------------------------------
  // PARAM_SEL / PARAM_VAL are the user-facing header slots (header.c slots 0/1
  // and 2/3). DETUNE..CHORD are the 12 internal DSP parameters that the
  // multiplexer edits.
  enum
  {
    PARAM_SEL = 0U, // knob A (header slot 0): parameter selector 0..15, shows name
    PARAM_VAL,      // knob B (header slot 1): raw value 0..1023 of PARAM_SEL, shows value
    DETUNE,         // static symmetric detune of A/B (−10..+10 cents, A=+c/2, B=−c/2, default 0)
    RATE_AMT,       // random-walk depth (0..100%, default 0% = off)
    RATE_SPD,       // random-walk period (0..100% -> 0.1 s..10 s, log, default 50%)
    RATE_SMTH,      // random-walk time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SUB1,           // sub-octave sine level (0..100%, default 50%)
    SUB2,           // second sub (f/4) very-soft-saturated level (0..100%, default 0% = off)
    SATU,           // saturator drive (0..3.0, default 2.0 = moderate)
    ASYM,           // asymmetric saturator (−100%..+100%, default 0% = symmetric)
    FOLD,           // pre-drive wavefolder depth (0..100%, default 0% = off)
    WAV1,           // oscillator A wavetable (0..90 across banks A-F)
    WAV2,           // oscillator B wavetable (0..90 across banks A-F)
    CHORD,          // third (chord) oscillator level (0..100%, default 0% = off)
    CHRD_INT,       // chord interval (0..1900 cents, continuous)
    RW_WAV,         // random-walk depth on wavetable morph (0..100%, default 0% = off)
    RW_DRV,         // random-walk depth on saturator drive (0..100%, default 0% = off)
    RW_FLD,         // random-walk depth on wavefolder depth (0..100%, default 0% = off)
    NUM_PARAMS
  };

  // ---- Multiplexer constants -----------------------------------------------
  // Number of editable DSP parameters (DETUNE..RW_FLD).
  static constexpr uint8_t kEditParamCount = 16; // DETUNE..RW_FLD
  // The first editable enum value. The UI (header.c PARAM slot, 0..15) expects
  // this order: 0 DET (DETUNE), 1 RATE (RATE_AMT), 2 SPEE (RATE_SPD),
  // 3 SMTH (RATE_SMTH), 4 SUB1, 5 SUB2, 6 SATU, 7 ASYM,
  // 8 FOLD, 9 WAV1, 10 WAV2, 11 CHRD (CHORD), 12 CINT (CHRD_INT),
  // 13 RW_WAV, 14 RW_DRV, 15 RW_FLD. So kEditParamFirst + select_
  // (select_ 0..15) maps to DETUNE..RW_FLD.
  static constexpr uint8_t kEditParamFirst = DETUNE;

  // ---- Runtime DSP parameter struct (see osc_params.h) ---------------------
  using Params = ::OscParamsData;

  // ---- Display scaling constants (raw 10-bit <-> DSP float) ----------------
  // Single source of truth for the ranges each editable parameter maps onto,
  // used by BOTH apply_edit_param() (raw -> DSP float) and
  // getParameterStrValue() (raw -> display string). Keep in sync with the
  // web simulator (websim/osc.html formatParamValue) and the default raw
  // values in the header's "init" fields (header.c).
  //
  // DET spans +/-kDetuneMaxCents around 0 cents, centered on the raw
  // midpoint (kRawMid = 511). SATU spans 0..kSatuMax. ASYM spans +/-100%.
  static constexpr float    kDetuneMaxCents = 10.f;  // sum-voice detune: +/-10 c
  static constexpr float    kSatuMax        = 3.f;   // 0..3.0 drive level
  static constexpr int32_t  kRawMid         = 511;   // raw 10-bit value == 0%
  static constexpr int32_t  kRawMax         = 1023;  // raw 10-bit value == 100%
  // Random-walk detune scale: the normalized walk deviations (rw_.dev_a/b,
  // in [-1, +1]) are multiplied by p.rw_amount (RATE AMT, 0..1) and then by
  // this (cents) in the audio path, so RATE AMT at 100% (rw_amount = 1)
  // wanders each voice by +/-kRwDetuneScaleCents around its DETUNE knob value. Kept here (not in RandomWalk) because the scale is
  // a per-target choice: each walk target gets its own scale at its point of
  // use (detune below; wavetable morph / drive / fold in the process() audio
  // path, gated by the RW_WAV / RW_DRV / RW_FLD depth knobs).
  static constexpr float    kRwDetuneScaleCents = 10.f; // +/-10 cents at RW depth 100%
  // Random-walk wavetable-morph scale: the normalized deviation is multiplied
  // by this (in tables) so RW_WAV at 100% wanders the morph position by
  // +/-kRwMorphScaleTables around the WAV1/WAV2 knob. Matches the platform
  // LFO morph depth (WaveTable::kLfoMorphDepth = 10 tables) so the walk can
  // sweep the same span the LFO does.
  static constexpr float    kRwMorphScaleTables = 10.f; // +/-10 tables at RW_WAV 100%
  // Random-walk drive scale: the normalized deviation is multiplied by this
  // (in drive units) so RW_DRV at 100% wanders the saturator drive by
  // +/-kRwDriveScale around the SATU knob. Drive is in 0..kSatuMax (0..3.0),
  // so +/-0.5 is a gentle-but-audible swing (e.g. 2.0 -> 1.5..2.5).
  static constexpr float    kRwDriveScale = 0.5f;       // +/-0.5 drive at RW_DRV 100%
  // Random-walk wavefolder scale: the normalized deviation is multiplied by
  // this (in fold-depth units) so RW_FLD at 100% wanders the folder depth by
  // +/-kRwFoldScale around the FOLD knob. Fold depth is 0..1; +/-0.3 is a
  // moderate metallic shimmer. Negative results are clamped to 0 in the path.

  // ---- Parameter selector / value multiplexer (state) ----
  // Knob A (PARAM_SEL, slot 0) picks one of the 12 DSP parameters
  // (DETUNE..CHORD), knob B (PARAM_VAL, slot 1) edits its raw 10-bit value.
  // The 12 raw 10-bit values (0..1023) are cached in edit_value_ so that
  // re-selecting a previously used parameter restores its value instead of
  // resetting it to the default.

  // Raw 10-bit values (0..1023) of the 16 editable parameters, cached so the
  // selector can recall them. Defaults match the DSP defaults in
  // OscParamsData::reset() (osc_params.h):
  // DETUNE 511, RATE 0, SPEE 511, SMTH 511, SUB1 511, SUB2 205, SATU 341,
  // ASYM 511, FOLD 0, WAV1 0, WAV2 128, CHORD 205, CINT 377, RW_WAV 0,
  // RW_DRV 0, RW_FLD 0.
  int32_t edit_value_[kEditParamCount] = {511, 0, 511, 511, 511, 205, 341, 511, 0, 0, 128, 205, 377, 0, 0, 0};

  // Currently selected parameter (0..15), i.e. edit_value_[select_] is what
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

#ifdef __EMSCRIPTEN__
  // ---- Direct edit-parameter channel (web simulator only) -------------------
  // On hardware the PARAM/VALUE multiplexer is driven by the platform's
  // unit_set_param_value() callback (K-rate, atomic per knob turn). The web
  // simulator's per-parameter sliders used to write to the WebAudioParam
  // slots 0/1 (PARAM_SEL / PARAM_VAL), but K-rate WebAudioParam smoothing
  // causes select_ to sweep through intermediate values (0 -> 1 -> 2 -> ...
  // -> target), each of which re-applies a different edit parameter to the
  // DSP -- producing audible glitches that look like a phantom LFO.
  //
  // To fix this, the UI bypasses WebAudioParam entirely and writes a
  // packed (index, raw) request to this single 32-bit slot. The request is
  // consumed once per block by process() (audio worklet thread), so the
  // (select_, PARAM_VAL) pair is applied atomically -- the same write
  // semantics the hardware has.
  //
  // Layout: bit 31 = valid flag, bits 0..4 = index (0..15), bits 8..17 =
  // raw value (0..1023). A 32-bit aligned store on x86 is atomic; even if
  // the worklet observed a torn value it would just skip the request
  // (valid bit would not be consistent with the payload).
  //
  // The hardware build does not use this channel: under non-EMSCRIPTEN
  // the volatile slot is absent (zero size) and process() never touches it.
  volatile uint32_t sim_edit_request_;

  // Write an edit-parameter request from the main thread (UI). Index is
  // 0..16 (kEditParamCount - 1), raw is 0..1023. Safe to call from any
  // thread; the audio worklet thread picks it up on the next block.
  void simEditRequest(uint8_t index, int32_t raw)
  {
    if (index >= kEditParamCount)
      return;
    const uint32_t packed = (1U << 31) | (static_cast<uint32_t>(index) & 0x1FU) |
                            ((static_cast<uint32_t>(raw) & 0x3FFU) << 8);
    sim_edit_request_ = packed;
  }
#endif

  // random-walk maximum deviation in cents at RATE AMT = 100% (full knob).
  // The detune never strays further from the DETUNE knob value than this.

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

  // ---- Static symmetric detune (DETU knob, ±kDetuneMaxCents = ±10 c) ----
  // The DETU knob detunes A and B in OPPOSITE directions around the base
  // note: w0_A = w0 * 2^(+c/2 / 1200), w0_B = w0 * 2^(-c/2 / 1200). This
  // produces a per-voice phase drift (the inter-voice beat frequency is
  // c * f / 600, twice the sum-detune rate) — the audible chorus depth is
  // ~4x stronger than the post-mix sum detune at the same knob value,
  // matching the old two-knob DET1/DET2 feel at moderate settings. The
  // Taylor factors are cached in K-rate (update_w0_det) when the random
  // walk is off; when the walk is active, process() recomputes them per
  // sample for the combined (static + walk) per-voice detune. The chord
  // voice and the sub voices stay at the base note (they are stable
  // "anchors", not beating voices).
  float w0_a_det_ = 0.f; // w0 * 2^(+detune_cents/2 / 1200)
  float w0_b_det_ = 0.f; // w0 * 2^(-detune_cents/2 / 1200)


  // Pre-drive wavefolder: before the tanh saturator, mix is folded into
  // [-1, 1] via  triangle(2x) — adding even harmonics and a "metallic"
  // edge. Depth is the FOLD knob (0 = off, 1 = full triangle fold).

  // (Display scaling constants kDetuneMaxCents / kSatuMax / kRawMid / kRawMax
  //  live in OscParams, see osc_params.h.)

  // ---- Audio-path mix constants ----
  // Final-mix gains. Worst-case peaks: |y*kMixOsc|<0.5,
  // |sub*kMixSub|<0.4, |sub2*kMixSub2|<0.3 (branches rarely peak together,
  // and the final hard-clip guards the sum).
  static constexpr float kMixOsc  = 0.5f;
  static constexpr float kMixSub  = 0.4f;
  static constexpr float kMixSub2 = 0.3f;
  // (The saturator drive limit lives in Saturator::kDriveLimit, see osc_saturator.h.)

  // ---- Chord voice (CHORD / CHRD_INT edit params) --------------------------
  // The third oscillator runs at a continuous interval above the base
  // (CINT knob: 0..1900 cents, any harmonic reachable). The CHORD knob
  // (0..100%) sets the voice level. All interval/gain logic lives in
  // ChordVoice (osc_chord.h): the audio path is one multiply
  // (w0 * 2^(cents/1200) via Taylor) plus one wavetable lookup.
  //
  // The CHORD knob (0..100%) sets the voice level. The per-voice gain is
  // normalized: gain = ChordVoice::kMixChord * lvl / (1 + lvl). At lvl=0 the
  // voice is silent (regression-safe). At lvl=1 (full knob) it reaches
  // kMixChord with no increase in the main mix's peak.

  // ---- Sample-rate / reference-frequency helpers ----
  // getSampleRate() (Processor base) returns 48000.f for all logue products;
  // reuse it in constexpr audio-path sites to avoid a stray 48000 literal.
  // (kCentsToX lives in Detune, see osc_detune.h.)
  static constexpr float    kSampleRate   = getSampleRate(); // 48000
  static constexpr float    kRefHz        = 880.f;           // A4
  static constexpr float    kTwoPi        = 6.283185307179586f;
  // 0.5 * 2^(kSubDetuneCents/1200) == 0.5 * 2^(-4/1200); derived from the
  // sub-octave fixed detune so it cannot drift if kSubDetuneCents changes.
  static constexpr float    kSubW0Scale   = 0.4988460883f; // 0.5 * 2^(-4/1200)

  // (The A-H-R envelope lives in AhEnvelope, see osc_envelope.h.)
  // (The runtime DSP parameter struct Params is defined in osc_params.h as
  //  OscParamsData; see the `using Params = ::OscParamsData;` alias above.)

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
      // The DETU knob detunes A and B in OPPOSITE directions (±c/2 each),
      // producing a per-voice phase drift (inter-voice beat = c*f/600).
      // The Taylor factors are cached in w0_a_det_/w0_b_det_ (K-rate) and
      // used directly in process() when the walk is off. See update_w0_det.
      params_.detune_cents = (param_10bit_to_f32(value) * 2.f - 1.f) * kDetuneMaxCents;
      update_w0_det();
      break;

    case RATE_AMT:
      {
        const float x = param_10bit_to_f32(value);
        params_.rw_amount = x * x; // quadratic: knob^2 -> 0..1 normalized depth
      }
      break;

    case RATE_SPD:
      // Target-update period only (the tracking time constant is RATE_SMTH).
      // The full setup (ceiling + interval + tracking) is recomputed from all
      // three RATE parameters so a change to any of them stays consistent.
      params_.rw_rate = param_10bit_to_f32(value);
      rw_.setup(params_.rw_rate, params_.rw_amount, params_.rw_smooth);
      break;

    case RATE_SMTH:
      // Tracking time constant only (10 ms .. 1.5 s, log). Recompute the full
      // setup so the tracking coefficient k picks up the new value.
      params_.rw_smooth = param_10bit_to_f32(value);
      rw_.setup(params_.rw_rate, params_.rw_amount, params_.rw_smooth);
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

    case CHORD:
      // 0..1023 -> 0..1 chord-voice level. The per-voice gain is applied in
      // process() as ChordVoice::kMixChord * lvl / (1 + lvl) — see osc_chord.h.
      params_.chord_level = param_10bit_to_f32(value);
      break;

    case CHRD_INT:
      // 0..1023 -> 0..1900 cents (linear). Continuous interval: any harmonic
      // is reachable, no quantization. The interval is stored in params_
      // (OscParamsData::chord_cents — the single source of truth for the
      // CINT default), then the chord phasor's phase increment is recomputed
      // (K-rate, Taylor via Detune::factor).
      {
        params_.chord_cents = param_10bit_to_f32(value) * ChordVoice::kMaxCents;
        chord_.update(w0_, params_.chord_cents);
      }
      break;

    case RW_WAV:
      // 0..1023 -> 0..1 random-walk depth on the wavetable morph. The actual
      // table scale (kRwMorphScaleTables) is applied in the audio path; here we
      // only store the 0..1 depth gate.
      params_.rw_wav = param_10bit_to_f32(value);
      break;

    case RW_DRV:
      // 0..1023 -> 0..1 random-walk depth on the saturator drive. The drive
      // scale (kRwDriveScale) is applied in the audio path.
      params_.rw_drv = param_10bit_to_f32(value);
      break;

    case RW_FLD:
      // 0..1023 -> 0..1 random-walk depth on the wavefolder depth. The fold
      // scale (kRwFoldScale) is applied in the audio path.
      params_.rw_fld = param_10bit_to_f32(value);
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
      // Knob A: parameter selector. 0..15 -> DETUNE, RATE_AMT, RATE_SPD,
      // RATE_SMTH, SUB1, SUB2, SATU, ASYM, FOLD, WAV1, WAV2, CHORD, CHRD_INT,
      // RW_WAV, RW_DRV, RW_FLD. Mask the low nibble (& 0xF) so any in-range
      // index (0..15) maps to itself. On selection change, re-apply the
      // cached raw value of the newly selected parameter so the DSP
      // immediately reflects it (PARAM_VAL then edits that value).
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
  //                        ("DETU", "SUB1", .., "RWAV", "RDRV", "RFLD").
  //   VALUE (slot 1 or 3): a short representation of the selected parameter's
  //                        raw 10-bit value ("50%", "+25%", wavetable index).
  // The RW_WAV / RW_DRV / RW_FLD depths format as 0..100% (the default branch).
  // The manual formatter below avoids pulling printf machinery into the ARM
  // image. Longest possible output is 5 chars ("+100%").
  const char *getParameterStrValue(uint8_t id, int32_t value) const override final
  {
    static const char *const names[kEditParamCount] = {
        "DETU", "RATE", "SPEE", "SMTH",
        "SUB1", "SUB2", "SATU", "ASYM", "FOLD", "WAV1", "WAV2", "CHRD", "CINT",
        "RWAV", "RDRV", "RFLD"};
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

    // SMTH (random-walk time constant): display as 0..100% (same format as
    // RATE / SPEE). The actual ms value (10..1500) is a log mapping of the
    // raw 0..1023 value; showing % keeps the display consistent and avoids
    // pulling a fasterpowf into the K-rate display path.

    // CINT (chord interval): display cents (0..1900), not a name.
    if (k == CHRD_INT)
    {
      int ci = (int)(param_10bit_to_f32(raw) * ChordVoice::kMaxCents + 0.5f);
      // format as "NNNc" (e.g. "700c", "1900c")
      int n2 = 0;
      int t2 = ci;
      char d2[5];
      int nd2 = 0;
      do
      {
        d2[nd2++] = (char)('0' + t2 % 10);
        t2 /= 10;
      } while (t2 > 0);
      for (int i = nd2 - 1; i >= 0; --i)
        buf[n2++] = d2[i];
      buf[n2++] = 'c';
      buf[n2] = '\0';
      return buf;
    }

    int v;
    if (k == WAV1 || k == WAV2)
      v = (int)(param_10bit_to_f32(raw) * (WaveTable::kWavesTotal - 1));
    else if (k == ASYM)
      v = (int)((raw * 200) / kRawMax) - 100;
    else if (k == DETUNE)
      v = (int)((raw - kRawMid) * (int)kDetuneMaxCents / kRawMid); // -kDetuneMaxCents..+kDetuneMaxCents cents (multiply before divide)
    else if (k == SATU)
      v = (int)(param_10bit_to_f32(raw) * (kSatuMax * 100.f)); // 0..300
    else
      v = (int)((raw * 100) / kRawMax);

    const bool percent = (k != WAV1 && k != WAV2 && k != SATU && k != DETUNE && k != CHRD_INT);
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
    chord_.reset(); // chord-voice phasor (the CINT interval default lives in params_.chord_cents)
    phasor_sub_ = 0.f;
    phasor_sub2_ = 0.f;
    sub2_.lpf_state = 0.f;
    sub2_.gate = 0.f;
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
    rw_.reset(); // random-walk state starts centered, AMT=0
    // A-H-R envelope: start silent, no note active.
    note_on_ = false;
    eg_.reset();
#ifdef __EMSCRIPTEN__
    // Clear any pending direct edit-parameter request (the UI may have
    // already sent one before init ran on the worklet thread).
    sim_edit_request_ = 0;
#endif
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
    chord_.reset();
    phasor_sub_ = 0.f;
    phasor_sub2_ = 0.f;
    sub2_.lpf_state = 0.f;
    sub2_.gate = 0.f;
    rw_.reset(); // re-center the random walk on stop
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
    w0_ = w0; // base phase increment
    update_w0_det(); // recompute A/B detuned w0 (symmetric ±c/2, K-rate)
    chord_.update(w0_, params_.chord_cents); // chord phasor: f * 2^(cents/1200)
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
  // platform's shape_lfo; here they drive the internal sine generator
  // (ShapeLfoSim, see osc_lfo_sim.h).
  void setLfoFreq(float freq)
  {
    lfo_sim_.freq = (freq < 0.f) ? 0.f : (freq > 30.f) ? 30.f : freq;
  }

  void setLfoDepth(float depth)
  {
    lfo_sim_.depth = (depth < 0.f) ? 0.f : (depth > 1.f) ? 1.f : depth;
  }
#endif

  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    (void)in; // drone generator does not use the audio input

#ifdef __EMSCRIPTEN__
    // Consume any pending direct edit-parameter request from the UI thread
    // (see simEditRequest). The (select_, PARAM_VAL) pair is applied
    // atomically here so the DSP never sees an intermediate select_ value --
    // the hardware's unit_set_param_value() path does the same thing (K-rate,
    // one knob turn = one apply).
    {
      const uint32_t req = sim_edit_request_;
      if (req & (1U << 31))
      {
        const uint8_t idx = (uint8_t)(req & 0x1FU);
        const int32_t raw = (int32_t)((req >> 8) & 0x3FFU);
        if (idx < kEditParamCount)
        {
          edit_value_[idx] = raw;
          select_ = idx;
          apply_edit_param(kEditParamFirst + idx, raw);
        }
        sim_edit_request_ = 0;
      }
    }
#endif

    // Caching current parameter values. Consider smoothing sensitive parameters in audio loop
    const Params p = params_;

    // base phase increment
    const float w0 = w0_;

    // ---- Random-walk detune deviation (target-based wandering, dual channel) ----
    // When rw_amount == 0 (knob at 0) the walk is disabled and w0_a / w0_b are
    // the cached static values (regression: byte-for-byte identical to the old
    // code). rw_amount is the quadratic-mapped knob value (see setParameter),
    // so rw_active is true for any non-zero knob position.
    //
    // The walk state (rw_.dev_a/b) is NORMALIZED to [-1, +1]; RATE AMT (the
    // quadratic-mapped rw_amount) is the depth ceiling, and the audio path
    // scales it to cents (kRwDetuneScaleCents = +/-10 cents) so neither voice
    // strays further from its DETUNE knob value than AMT allows. The two
    // channels wander with INDEPENDENT random targets (chosen on the same
    // period), so the inter-voice beat frequency shifts over time — an
    // asymmetric, ever-changing chorus.
    //
    // The deviations smoothly track random targets (see RandomWalk::advance)
    // that are regenerated every `interval` samples. RATE SPD sets that period
    // (0.1 s .. 10 s), RATE SMTH sets the tracking time constant (10 ms ..
    // 1.5 s); keeping them independent makes the audible depth stable
    // regardless of SPEED.
    // (The A-H-R envelope is advanced per-sample inside the loop below; the
    // relaxation coefficients are cached once per block for cheap access.)
    const float alpha_a = eg_alpha_a_;
    const float alpha_r = eg_alpha_r_;

    // ---- Second sub (SUB2) level (see osc_sub2.h for the voice DSP) ----
    const float sub2_level = p.sub2_level;

    // ---- Chord voice gain (precomputed once per block) ----
    // Normalized gain: ChordVoice::kMixChord * lvl / (1 + lvl). At lvl=0 the
    // voice is silent (regression-safe: identical to the 2-osc sound). At
    // lvl=1 (full knob) it reaches kMixChord. See osc_chord.h for details.
    const float chord_gain = (p.chord_level > 0.f)
        ? (ChordVoice::kMixChord * p.chord_level / (1.f + p.chord_level))
        : 0.f;

    const bool rw_active = p.rw_amount > 0.f;
    // A and B run at the base note ± the static DETU knob (symmetric ±c/2,
    // cached in w0_a_det_/w0_b_det_ at K-rate), plus the random-walk detune
    // (asymmetric, per-sample) when rw_active. When the walk is off, the
    // cached values are used directly (0 Taylor cost per sample).
    float w0_a = w0_a_det_; // static symmetric detune (osc A, +c/2)
    float w0_b = w0_b_det_; // static symmetric detune (osc B, -c/2)

    // wavetable morph: base (knob) + pitch-shift (K-rate cached), the LFO
    // offset is added per-sample below. waves_.wave_shift_ is recomputed on
    // setParameter(WAV1/WAV2) / setPitch(); wave_a / wave_b are the knobs.
    const float wave_base_a = p.wave_a + waves_.wave_shift_;
    const float wave_base_b = p.wave_b + waves_.wave_shift_;
    if (rw_active)
    {
      // Recompute the ceiling / interval / tracking from the rate parameters
      // (any of rw_rate / rw_amount / rw_smooth may have changed).
      rw_.setup(p.rw_rate, p.rw_amount, p.rw_smooth);
      // Initialize the targets to the current deviations on first activation
      if (rw_.target_a == 0.f && rw_.dev_a != 0.f)
        rw_.target_a = rw_.dev_a;
      if (rw_.target_b == 0.f && rw_.dev_b != 0.f)
        rw_.target_b = rw_.dev_b;
    }

    // Random-walk scaled values (computed per-sample inside the loop below).
    // rw_.dev_a/b are normalized to [-1, +1] and advance per-sample via
    // rw_.advance(). Each walk target scales them to its own units at the
    // point of use: detune (cents), morph (tables), drive, fold. The scaled
    // values must be recomputed every sample (not cached per-block) because
    // rw_.advance() moves dev_a/b on every sample.
    const bool rw_wav_active = rw_active && p.rw_wav > 0.f;
    const bool rw_drv_active = rw_active && p.rw_drv > 0.f;
    const bool rw_fld_active = rw_active && p.rw_fld > 0.f;

    for (const float *out_end = out + frames; out != out_end; in += 2, out += 1)
    {
      // update random-walk deviations (advance once per sample when active).
      // The RATE AMT knob is the DEPTH CEILING of the whole walk: it scales
      // every random-walk modulation (detune below; wavetable morph / drive /
      // fold further down). The normalized deviations (rw_.dev_a/b, in [-1,+1])
      // are multiplied by p.rw_amount (0..1, quadratic-mapped from the knob)
      // at each point of use, so RATE = 0 means no wandering at all and
      // RATE = 100% gives full scale. Without this factor the knobs would be
      // binary (any non-zero RATE gave full-depth walk) — see the bug this
      // fixes (rw_amount was only used as an on/off gate, never as a scale).
      const float rw_amt = p.rw_amount;
      float rw_dev_a_cents = 0.f;
      float rw_dev_b_cents = 0.f;
      if (rw_active)
      {
        rw_.advance(p.rw_amount, osc_white);
        // Per-sample scaled walk values (dev_a/b change every sample).
        rw_dev_a_cents = rw_.dev_a * rw_amt * kRwDetuneScaleCents;
        rw_dev_b_cents = rw_.dev_b * rw_amt * kRwDetuneScaleCents;
        // recompute w0_a / w0_b from the combined (static + walk) per-voice
        // detune: A gets +c/2 (static) + rw_dev_a (walk), B gets -c/2 (static)
        // + rw_dev_b (walk). 4th-order Taylor via Detune::w0_detuned
        // (see osc_detune.h).
        const float half_det = 0.5f * p.detune_cents;
        w0_a = Detune::w0_detuned(w0, +half_det + rw_dev_a_cents);
        w0_b = Detune::w0_detuned(w0, -half_det + rw_dev_b_cents);
      }
      const float rw_dev_a_tables = rw_wav_active ? rw_.dev_a * rw_amt * kRwMorphScaleTables : 0.f;
      const float rw_dev_b_tables = rw_wav_active ? rw_.dev_b * rw_amt * kRwMorphScaleTables : 0.f;
      const float rw_dev_drive    = rw_drv_active ? rw_.dev_a * rw_amt * kRwDriveScale : 0.f;
      // Multiplicative: eff_fold = p.fold_depth * (1 + dev_b * p.rw_fld * rw_amt).
      // p.rw_fld is 0..1 (RFLD knob). At RFLD=0 -> eff_fold = p.fold_depth (no mod).
      // At RFLD=100% & RATE=100% -> eff_fold wanders from 0 to 2*p.fold_depth
      // (clamped to 1). The rw_amt factor keeps the folder walk gated by the
      // global RATE AMT depth, consistent with the detune/morph/drive walks.
      // When p.fold_depth = 0, eff_fold = 0 regardless (no fold).
      const float rw_fold_mod = rw_fld_active ? rw_.dev_b * p.rw_fld * rw_amt : 0.f;

      // phasor update
      phasor_a_ += w0_a;
      phasor_b_ += w0_b;
      // Chord phasor: advances at w0 * kScale[interval] (see osc_chord.h).
      // No random-walk modulation — the chord voice is a stable "anchor"
      // like SUB2, not a beating voice. advance() returns false if the
      // interval is OFF (no oscillator).
      const bool chord_active = chord_.advance();

      // sub-octave phasor: base rate is w0_sub_ (f/2 * 2^(sub_detune/1200)),
      // modulated by the platform shape-LFO (±kSubLfoFmCents around w0_sub_).
      // The LFO is applied as a per-sample detune via Detune::factor (4th-order
      // Taylor, see osc_detune.h) — the LFO value is small (±5 cents), so the
      // series is more than accurate enough.
      {
        const float lfo_fm_cents = lfo_ * kSubLfoFmCents;
        phasor_sub_ += w0_sub_ * Detune::factor(lfo_fm_cents);
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
      // and this line is compiled out, so the two paths share the same
      // morph code below. See osc_lfo_sim.h for the synthesizer.
#ifdef EMSCRIPTEN
      lfo_ = lfo_sim_.process();
#endif

      // generate wavetable samples in -1..1. Each voice's table index is the
      // base position (knob + pitch-shift) plus the LFO morph offset, so the
      // timbre "breathes" across the wavetable. WaveTable::resolve_wave() crossfades the
      // fractional part between adjacent tables, so the morph glides smoothly.
      const float lfo_morph = lfo_ * WaveTable::kLfoMorphDepth;
      // Wavetable scan positions: base (WAV1/WAV2 knob) + LFO morph (platform
      // shape-LFO) + random-walk morph (RW_WAV knob, via rw_dev_a/b_tables).
      // The walk morph is added so each voice wanders through the 91 Korg
      // wavetables around its knob position — an unpredictable, organic
      // timbre drift that the static LFO sweep alone cannot produce.
      const float wav_a_pos = wave_base_a + lfo_morph + rw_dev_a_tables;
      const float wav_b_pos = wave_base_b + lfo_morph + rw_dev_b_tables;
      const float wav_a = WaveTable::resolve_wave(wav_a_pos, phasor_a_);
      const float wav_b = WaveTable::resolve_wave(wav_b_pos, phasor_b_);

      // Chord voice: third oscillator at a selectable interval above the base
      // (CINT knob: OFF/5TH/OCT/O+5/M3). Uses the WAV1 (osc A) wavetable so
      // it blends with the main voice. Computed only when both the interval
      // is active (chord_active) and the level is non-zero (chord_gain > 0)
      // to keep the default path (CHORD = 0 or CINT = OFF) free of the extra
      // wavetable lookup. See osc_chord.h for the interval table.
      const float wav_c = (chord_active && chord_gain > 0.f)
          ? WaveTable::resolve_wave(wave_base_a + lfo_morph, chord_.phasor)
          : 0.f;

      // sub-octave sine in -1..1 (LUT lookup, ~4 VFP ops)
      const float sub = osc_sinf(phasor_sub_);

      // ---- second sub (f/4): clean sine -> very soft saturator -> LPF -> gate ----
      // Only computed when the voice is actually audible (level > 0) to keep
      // the default path (SUB2 = 0) free of this cost.
      // See osc_sub2.h for the DSP details (sine -> soft saturator -> LPF -> gate).
      const float sub2 = sub2_.process(phasor_sub2_, w0, sub2_level);

      // mix both voices equally, then run through the pre-drive wavefolder
      // (FOLD knob) and the asymmetric soft-clip saturator (SATU/ASYM knobs).
      // See osc_saturator.h for the DSP details.
      // The chord voice (wav_c * chord_gain) is added to the mix before the
      // folder/saturator — it shares the same timbre chain so it stays
      // cohesive with the main voice. The normalized gain (kMixChord * lvl
      // / (1 + lvl)) keeps the saturator's peak budget unchanged from the
      // 2-osc case.
      const float mix = kMixOsc * (wav_a + wav_b + wav_c * chord_gain);
      // (The static DETU knob is applied per-voice above, via w0_a / w0_b —
      //  not post-mix: A and B are detuned in opposite directions around the
      //  base note, producing a per-voice phase drift that sounds ~4x
      //  stronger than an equivalent post-mix sum detune.)
      // Wavefolder depth: FOLD knob modulated by random-walk fold (RW_FLD knob).
      // Multiplicative: eff_fold = p.fold_depth * (1 + rw_fold_mod), where
      // rw_fold_mod = dev_b * p.rw_fld (normalized -1..1 * 0..1 = -1..1).
      // At RFLD=0: eff_fold = p.fold_depth (no modulation).
      // At RFLD=100%: eff_fold wanders from 0 to 2*p.fold_depth (clamped to 1).
      // When p.fold_depth = 0: eff_fold = 0 (no fold, RFLD has no effect).
      float eff_fold = p.fold_depth * (1.f + rw_fold_mod);
      if (eff_fold < 0.f) eff_fold = 0.f;
      else if (eff_fold > 1.f) eff_fold = 1.f;
      // Saturator drive: SATU knob + random-walk drive (RW_DRV knob, via
      // rw_dev_drive, per-sample). Clamp to 0..kSatuMax so the walk never
      // pushes the drive beyond the saturator's stable range.
      float eff_drive_clamped = p.drive + rw_dev_drive;
      if (eff_drive_clamped < 0.f) eff_drive_clamped = 0.f;
      else if (eff_drive_clamped > kSatuMax) eff_drive_clamped = kSatuMax;
      const float folded = Saturator::fold(mix, eff_fold);
      const float y = Saturator::saturate(folded, eff_drive_clamped, p.asym);

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
  // (setPitch / setParameter(DETUNE)) to keep the audio path lightweight.
  // (The static symmetric-detune factors w0_a_det_ / w0_b_det_ are declared
  //  earlier in the class, next to their documentation block. When the
  //  random walk is active, process() recomputes the per-voice factors per
  //  sample for the combined (static + walk) detune.)
  float w0_sub_ = 0.f;
  // second-sub (f/4) phase increment, precomputed in update_w0_sub2()
  float w0_sub2_ = 0.f;
  // Second sub voice (f/4, saturated, LPF'd, gated). See osc_sub2.h.
  Sub2Voice sub2_;
  // Chord voice (third oscillator at selectable interval). See osc_chord.h.
  ChordVoice chord_;

  // Random-walk detune wandering (dual channel). See osc_random_walk.h.
  RandomWalk rw_;

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

  // recompute the static symmetric-detune factors (w0_a_det_ / w0_b_det_)
  // — only when the DETU knob or the base pitch changes. The DETU knob
  // detunes A and B in OPPOSITE directions: A gets +c/2 cents, B gets -c/2
  // cents (c = params_.detune_cents, ±10 cents). The Taylor expansion
  // (see Detune::factor, osc_detune.h) at |x| <= 5 * ln(2)/1200 ≈ 0.0042
  // (per voice, since each is detuned by c/2) keeps the relative error
  // far below audibility.
  //
  // Note: when the random walk is active (RATE AMT > 0), process() recomputes
  // the per-voice factors per sample for the combined (static + walk) detune,
  // so the cached w0_a_det_ / w0_b_det_ are only used while the walk is
  // disabled (0 Taylor cost in the audio path in that case — just a read).
  void update_w0_det()
  {
    const float half = 0.5f * params_.detune_cents;
    w0_a_det_ = w0_ * Detune::factor(+half);
    w0_b_det_ = w0_ * Detune::factor(-half);
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
  //   sub2_.lpf_a  — one-pole LPF coefficient for Sub2Voice::kLpfCutoffHz (cuts the
  //                  saturator's harmonics so the voice stays in the low end)
  //   sub2_.gate_k — first-order smoothing coefficient for the pitch gate
  //                  (open/close transition, Sub2Voice::kGateTauSec time constant)
  //   sub2_.gate_hz_— base (saw) frequency in Hz at which f/4 == Sub2Voice::kGateHz;
  //                  below this the gate closes. Cached as 4 * Sub2Voice::kGateHz.
  void update_w0_sub2()
  {
    w0_sub2_ = w0_ * 0.25f; // f/4, no detune
    sub2_.update(w0_); // recompute LPF/gate coefficients (see osc_sub2.h)
  }

  Params params_;
  float w0_;
  float lfo_;

#ifdef EMSCRIPTEN
  // Host-side (web simulator) shape-LFO synthesizer. On hardware the platform
  // generates the LFO from the OSC-button A/B knobs and delivers it through
  // setShapeLfo(); in the simulator there is no platform, so we synthesize a
  // phase-coherent sine at audio rate here (see osc_lfo_sim.h). The UI sets
  // frequency (F knob, 0..30 Hz) and depth (S knob, 0..1) via
  // setLfoFreq()/setLfoDepth().
  ShapeLfoSim lfo_sim_;
#endif

  // local variables related to audio processing
  float phasor_a_;
  float phasor_b_;
  float phasor_sub_; // sub-octave sine phasor (advances at w0/2)
  // second sub voice (f/4, two octaves below the saws): a clean sine run
  // through a very soft saturator (adds "body"/thickening) then a one-pole
  // LPF (cuts the saturator's harmonics so it stays in the low end) and a
  // pitch gate (silences it when f/4 drops below Sub2Voice::kGateHz = infrasonic).
  // Unlike sub-1 it carries no fixed detune: it is a stable "anchor" in the
  // low end, not a beating voice.
  float phasor_sub2_; // second sub phasor (advances at w0/4, no detune)
  // ---- Wavetable scanning (see osc_waves.h) ----------------------------------
  // The six-bank Korg wavetable scanner + the pitch-shifted morph state live in
  // WaveTable (osc_waves.h). The Osc owns a WaveTable member and reads its
  // cached morph position (waves_.wave_shift_) in the audio path; resolve_wave
  // is called as WaveTable::resolve_wave(...). The LFO->morph depth constant is
  // WaveTable::kLfoMorphDepth.
  WaveTable waves_;
};
