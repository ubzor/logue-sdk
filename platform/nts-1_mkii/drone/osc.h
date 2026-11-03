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
 *  This header is the THIN SKELETON of the Osc class. The heavy method bodies
 *  were factored out (see the #includes at the bottom of this file) so the
 *  class definition stays small and each concern lives in its own file:
 *
 *    osc_constants.h       -- the static tuning constants (global scope)
 *    osc_param_apply.h     -- Osc::apply_edit_param()  (raw 10-bit -> DSP)
 *    osc_param_display.h   -- Osc::getParameterStrValue() (raw -> display)
 *    osc_mix.h             -- Osc::process()           (per-sample audio path)
 *
 *  The remaining building blocks are helper structs in their own headers
 *  (osc_waves.h, osc_saturator.h, osc_svf.h, osc_random_walk.h,
 *  osc_envelope.h, osc_sub2.h, osc_chord.h, osc_detune.h, osc_lfo_sim.h),
 *  and the live DSP state lives in OscParamsData (osc_params.h).
 *
 *  Sound overview: two wavetable phasors (A/B) at the base note with a static
 *  symmetric detune (DETU), a pre-drive wavefolder (FOLD) and an asymmetric
 *  soft-clip saturator (SATU/ASYM), a third "chord" voice (CHRD/CINT) with its
 *  own drive/fold (CHSAT/CHFLD), sub-octave (SUB1) and sub-2 (SUB2) voices, an
 *  internal 12 dB/oct SVF (FCUT/RESO), and four independent random-walk streams
 *  (Tone/Timbre/Density/Breath) plus classic cross-FM (XFM1/XFM2). No dynamic
 *  allocation: all state lives in members of this class.
 *
 */

#include "processor.h"
#include "unit_osc.h"
#include "utils/float_math.h" // fastertanh2f()
#include "osc_params.h"       // OscParams enum + constants + OscParamsData struct
#include "osc_waves.h"        // WaveTable: bank scanning + pitch-shifted morph
#include "osc_envelope.h"     // AhEnvelope: single-voice A-H-R envelope
#include "osc_saturator.h"    // Saturator: pre-drive wavefolder + asymmetric soft-clip
#include "osc_svf.h"          // Svf: 12 dB/oct state-variable filter (post-saturator)
#include "osc_random_walk.h"  // RandomWalk: multi-channel wandering
#include "osc_sub2.h"         // Sub2Voice: f/4 saturated, LPF'd, gated sub voice
#include "osc_chord.h"        // ChordVoice: third oscillator at selectable interval
#include "osc_detune.h"       // Detune: w0 detuning / factor (4th-order Taylor)
#include "osc_lfo_sim.h"      // LfoSim: web-sim-only shape-LFO synthesizer
#include "osc_constants.h"    // global-scope tuning constants (kSatuMax, kMix*, ...)

class Osc : public Processor
{
  // The wavetable scanner (osc_waves.h) is a flat helper struct; let it read
  // our private state (params_, the phasor w0) without exposing it.
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
    RATE_AMT,       // SHARED random-walk depth ceiling for all three streams (0..100%, default 0% = off)
    // Three independent wandering streams (Tone / Timbre / Density). Each has
    // its own period (SPDx) and time constant (SMTx), so they are not
    // phase-locked. See osc_params.h for the per-stream targets each drives.
    SPD1,           // Tone stream period   (0..100% -> 0.1 s..10 s, log, default 50%)
    SMT1,           // Tone stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SPD2,           // Timbre stream period  (0..100% -> 0.1 s..10 s, log, default 50%)
    SMT2,           // Timbre stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SPD3,           // Density stream period (0..100% -> 0.1 s..10 s, log, default 50%)
    SMT3,           // Density stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    SUB1,           // sub-octave sine level (0..100%, default 50%)
    SUB2,           // second sub (f/4) very-soft-saturated level (0..100%, default 0% = off)
    SATU,           // saturator drive (0..3.0, default 2.0 = moderate)
    ASYM,           // asymmetric saturator (−100%..+100%, default 0% = symmetric)
    FOLD,           // pre-drive wavefolder depth (0..100%, default 0% = off)
    WAV1,           // oscillator A wavetable (0..90 across banks A-F)
    WAV2,           // oscillator B wavetable (0..90 across banks A-F)
    WAV3,           // oscillator C (chord voice) wavetable (0..90 across banks A-F)
    CHORD,          // third (chord) oscillator level (0..100%, default 0% = off)
    CHRD_INT,       // chord interval (0..1900 cents, continuous)
    MAIN,           // main (A+B mixed) voice level (0..100%, default 100% = full)
    RW_WAV,         // random-walk depth on wavetable morph (0..100%, default 0% = off)
    RW_DRV,         // random-walk depth on saturator drive (0..100%, default 0% = off)
    RW_FLD,         // random-walk depth on wavefolder depth (0..100%, default 0% = off)
    XFM_A,          // cross-FM depth on osc A (B modulates A's phase, 0..30 c, default 0 = off)
    XFM_B,          // cross-FM depth on osc B (A modulates B's phase, 0..30 c, default 0 = off)
    FCUT,           // internal SVF cutoff + dry/wet (log 80 Hz..12 kHz, 0% = raw, 100% = LPF)
    RES,            // internal SVF resonance Q (0..100% -> 0.5..2.5, default 0% = 0.5)
    RW_FC,          // random-walk depth on SVF cutoff (0..100%, default 0% = off)
    RW_LVL,         // breath: random-walk depth on the four voice LEVELS (0..100%, default 0% = off)
    RW_LSP,         // breath stream period (0..100% -> 0.1 s..10 s, log, default 50%)
    RW_LSM,         // breath stream time constant (0..100% -> 10 ms..1.5 s, log, default 50%)
    // ---- Chord voice's OWN drive/fold (independent of the A+B SATU/FOLD) ----
    // The chord has its own wavefolder + saturator pass (see the CHORD branch in
    // process()). CHSAT is its drive (0..3.0, like SATU) and CHFLD is its
    // fold depth (0..1, like FOLD). ASYM stays SHARED with the A+B voice.
    // Appended at the END (after RW_LSM) so the existing edit indices
    // (DETUNE..RW_LSM = 0..29) are not renumbered.
    CHSAT,          // chord saturator drive (0..3.0, default = SATU default)
    CHFLD,          // chord wavefolder depth (0..1, default = FOLD default = off)
    NUM_PARAMS
  };

  // ---- Multiplexer constants -----------------------------------------------
  // Number of editable DSP parameters (DETUNE..RW_LSM).
  static constexpr uint8_t kEditParamCount = 32; // DETUNE..CHFLD
  // The first editable enum value. The UI (header.c PARAM slot, 0..31) expects
  // this order (select_ index -> parameter), 32 parameters total:
  //   0 DET (DETUNE)    1 RATE (RATE_AMT, shared depth ceiling)
  //   2 SPD1            3 SMT1            (Tone stream period / smooth)
  //   4 SPD2            5 SMT2            (Timbre stream period / smooth)
  //   6 SPD3            7 SMT3            (Density stream period / smooth)
  //   8 SUB1            9 SUB2            10 SATU           11 ASYM
  //   12 FOLD           13 WAV1           14 WAV2           15 WAV3
  //   16 CHRD (CHORD)   17 CINT (CHRD_INT) 18 MAIN (main A+B voice level)
  //   19 RW_WAV         20 RW_DRV         21 RW_FLD
  //   22 XFM_A (cross-FM on A)  23 XFM_B (cross-FM on B)
  //   24 FCUT (SVF cutoff + dry/wet)  25 RES (SVF resonance Q)
  //   26 RWFC (RW_FC: random-walk depth on SVF cutoff)
  //   27 RW_LVL (RW_LVL: breath depth on voice levels)
  //   28 RW_LSP (RW_LSP: breath stream period)  29 RW_LSM (RW_LSM: breath smooth)
  //   30 CHSAT (chord's own saturator drive)  31 CHFLD (chord's own fold depth)
  // PARAM_SEL / PARAM_VAL are the two multiplexer knobs (not editable params).
  // kEditParamCount = 32 = (CHFLD - DETUNE + 1); select_ 0..31 -> DETUNE..CHFLD.
  static constexpr uint8_t kEditParamFirst = DETUNE;
  // ---- Runtime DSP parameter struct (see osc_params.h) ---------------------
  using Params = ::OscParamsData;

  // ---- Multiplexer state (select_ / edit_value_) --------------------
  // edit_value_[i] is the raw 10-bit cache of editable parameter i, so
  // re-selecting a parameter restores its last value. Defaults match the
  // DSP defaults in OscParamsData::reset() (osc_params.h).
  // WAV3 0, CHORD 205, CINT 377, MAIN 1023 (full), RW_WAV 0, RW_DRV 0,
  // RW_FLD 0, XFM_A 0, XFM_B 0, FCUT 921 (~90%), RES 0, RWFC 0, RW_LVL 0
  // (breath off), RW_LSP 511, RW_LSM 511.
  int32_t edit_value_[kEditParamCount] = {511, 0, 511, 511, 511, 511, 511, 511, 511, 205, 341, 511, 0, 0, 128, 0, 205, 377, 1023, 0, 0, 0, 0, 0, 921, 0, 0, 0, 511, 511, 341, 0};

  // Currently selected parameter (0..19), i.e. edit_value_[select_] is what
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

  // Map a raw 10-bit value (0..1023) for one of the 32 editable
  // parameters (DETUNE..CHFLD) onto the DSP state. This is the single
  // source of truth for the parameter scaling: it is used both by the
  // edit-menu path (setParameter(DETUNE..CHFLD)) and by the PARAM_VAL
  // multiplexer path. The body lives in osc_param_apply.h.
  void apply_edit_param(uint8_t idx, int32_t value);
  // walk is off; when the walk is active, process() recomputes them per
  // sample for the combined (static + walk) per-voice detune. The chord
  // voice and the sub voices stay at the base note (they are stable
  // "anchors", not beating voices).
  float w0_a_det_ = 0.f; // w0 * 2^(+detune_cents/2 / 1200)
  float w0_b_det_ = 0.f; // w0 * 2^(-detune_cents/2 / 1200)
  // (The A-H-R envelope lives in AhEnvelope, see osc_envelope.h.)
  // (The runtime DSP parameter struct Params is defined in osc_params.h as
  //  OscParamsData; see the `using Params = ::OscParamsData;` alias above.)

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

  // Format the display string for a header slot (0..3): PARAM_SEL shows
  // the name of the selected parameter, PARAM_VAL shows its formatted
  // value. The body lives in osc_param_display.h.
  const char *getParameterStrValue(uint8_t id, int32_t value) const override final;
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
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // each stream starts centered, AMT=0 (off)
    svf_.reset(); // zero the SVF integrator states (coefficients set by apply_edit_param above)
    prev_wav_a_ = 0.f; // cross-FM lookback cache: start at 0 (first sample is a no-op)
    prev_wav_b_ = 0.f;
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
    for (uint8_t i = 0; i < kRwStreamCount; i++)
      rw_[i].reset(); // re-center every stream on stop
    svf_.reset(); // zero the SVF integrator states (keeps the knob-derived coefficients)
    prev_wav_a_ = 0.f; // cross-FM lookback cache
    prev_wav_b_ = 0.f;
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

  // Tempo callback (K-rate). The platform (unit.cc) and the web simulator
  // (Module.fx_set_bpm -> setTempo) deliver the current BPM here. The value
  // is stored in tempo_bpm_ so it can be read from the audio path later
  // (e.g. for BPM-synced random-walk periods, see the planned SYNC mode).
  // A 32-bit aligned float write is effectively atomic on Cortex-M7; the
  // worst case is the audio thread seeing either the old or the new BPM for
  // one block, which is inaudible (it only affects a slow modulation period,
  // never the audio rate).
  void setTempo(float bpm) override
  {
    tempo_bpm_ = bpm;
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
  // Per-sample audio path (phasor advance, wavetable resolve, A+B chain,
  // chord, sub/sub-2, final mix, envelope). The body lives in osc_mix.h.
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final;
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

  // Four independent random-walk streams (dual channel each). See
  // osc_random_walk.h. Index 0 = Tone (detune + cross-FM), 1 = Timbre
  // (wavetable morph + SVF cutoff), 2 = Density (drive + wavefolder), 3 =
  // Breath (the four voice LEVELS: A / B / chord / sub wander in prominence).
  // Each stream wanders with independent random targets on its own
  // (randomly-drawn) period, so the streams are not phase-locked to each
  // other. (The Breath stream uses all four of its channels — A/B/C/D — to
  // give the four voices independent wandering; the other three streams use
  // only the A/B pair, which keep their shared-counter behavior.)
  static constexpr uint8_t kRwStreamCount = 4;
  RandomWalk rw_[kRwStreamCount];

  // Internal 12 dB/oct state-variable filter (post-saturator, pre-sub-mix).
  // See osc_svf.h. The cutoff (FCUT knob) and resonance (RES knob) are set at
  // K-rate; the cutoff is modulated per-sample by the random walk (RWFC is
  // folded into the FCUT knob's dry/wet — see apply_edit_param). The dry/wet
  // crossfade (svf_wet) blends the raw saturator output with the LPF output.
  Svf::State svf_;

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
  // Current tempo in BPM, delivered via setTempo() (K-rate). Default 120
  // (the web-simulator slider default). Read from the audio path for
  // BPM-synced modulation (planned SYNC mode); today it is stored but not
  // yet consumed, so the stock sound is unaffected.
  float tempo_bpm_ = 120.f;

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
  // Previous-sample wavetable outputs, cached for the next sample's cross-FM
  // modulation (one-sample lookback, see the XFM branch in the phasor update).
  // Zeroed on init / reset so the first sample's cross-FM is a no-op.
  float prev_wav_a_ = 0.f;
  float prev_wav_b_ = 0.f;
  // ---- Wavetable scanning (see osc_waves.h) ----------------------------------
  // The six-bank Korg wavetable scanner + the pitch-shifted morph state live in
  // WaveTable (osc_waves.h). The Osc owns a WaveTable member and reads its
  // cached morph position (waves_.wave_shift_) in the audio path; resolve_wave
  // is called as WaveTable::resolve_wave(...). The LFO->morph depth constant is
  // WaveTable::kLfoMorphDepth.
  WaveTable waves_;
};

// The Osc method bodies are defined here, AFTER the class is complete, so
// they can reference every data member and the global-scope tuning constants
// from osc_constants.h.
#include "osc_param_apply.h"   // Osc::apply_edit_param()
#include "osc_param_display.h" // Osc::getParameterStrValue()
#include "osc_mix.h"           // Osc::process()