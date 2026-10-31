#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_params.h
 *
 *  Runtime DSP parameter struct for the DRONE unit.
 *
 *  Holds the OscParamsData struct — the live DSP state, populated by
 *  apply_edit_param (in osc.h) from the raw 10-bit values of the currently
 *  selected parameter. Reset to defaults on init().
 *
 *  The audio-parameter enum (PARAM_SEL, PARAM_VAL, DETUNE..WAV2), the
 *  multiplexer constants (kEditParamCount, kEditParamFirst), and the display
 *  scaling constants (kDetuneMaxCents, kSatuMax, kRawMid, kRawMax) remain in
 *  the Osc class (osc.h) because they are tightly coupled to the multiplexer
 *  state (select_, edit_value_) and the apply_edit_param / setParameter /
 *  getParameterStrValue methods.
 *
 */

// ---- Runtime DSP parameters -------------------------------------------------
// The live DSP state, populated by apply_edit_param from the raw 10-bit values
// of the currently selected parameter. Reset to defaults on init().
struct OscParamsData
{
  // Static symmetric detune of A and B (DETU knob, ±10 cents): A gets +c/2,
  // B gets -c/2. Applied per-voice (not post-mix) so the inter-voice beat
  // frequency is c * f / 600 — ~4x stronger than an equivalent post-mix sum
  // detune at the same knob value. The chord voice and the sub voices stay
  // at the base note (they are stable "anchors").
  float detune_cents;   // -10.f .. +10.f, DETU knob (A/B symmetric)
  float drive;          // 0.f .. 3.f, SATU knob
  float rw_amount;    // 0.f .. 1.f, edit: SHARED random-walk depth ceiling (quadratic-mapped)
                     //            for all three wandering streams (RATE knob).
  // Three independent wandering streams (Tone / Timbre / Density), each with
  // its own period (SPDx) and time constant (SMTx). They wander with
  // independent random targets on independent (randomly-drawn) periods, so the
  // streams are NOT phase-locked to each other — each "lives its own life".
  // See osc.h for which DSP target each stream drives:
  //   [0] Tone    -> detune A/B + cross-FM (XFM_A / XFM_B)
  //   [1] Timbre  -> wavetable morph (RW_WAV) + SVF cutoff (RWFC)
  //   [2] Density -> saturator drive (RW_DRV) + wavefolder depth (RW_FLD)
  float rw_rate[3];   // 0.f .. 1.f, edit SPD1..3: stream period (0.1 s .. 10 s, log)
  float rw_smooth[3]; // 0.f .. 1.f, edit SMT1..3: stream time constant (10 ms .. 1.5 s, log)
  float sub_level;    // 0.f .. 1.f, edit: sub-octave (f/2) sine level
  float sub2_level;   // 0.f .. 1.f, edit: second sub (f/4) saturated level
  float fold_depth;   // 0.f .. 1.f, edit: pre-drive wavefolder depth
  float asym;         // -1.f .. +1.f, edit: asymmetric saturator (0 = symmetric)
  float wave_a;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc A wavetable index
  float wave_b;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc B wavetable index
  float chord_level;  // 0.f .. 1.f, edit: chord-voice level
  float chord_cents;  // 0.f .. ChordVoice::kMaxCents (1900), edit: chord interval (CINT)
  float rw_wav;       // 0.f .. 1.f, edit: random-walk depth on wavetable morph (RW_WAV)
  float rw_drv;       // 0.f .. 1.f, edit: random-walk depth on saturator drive (RW_DRV)
  float rw_fld;       // 0.f .. 1.f, edit: random-walk depth on wavefolder depth (RW_FLD)
  // ---- Cross-FM (XFM): phase modulation of A by B and vice versa --------
  float xfm_a;        // 0.f .. 1.f, edit: XFM depth on osc A (A is modulated by B)
  float xfm_b;        // 0.f .. 1.f, edit: XFM depth on osc B (B is modulated by A)
  // ---- Internal SVF (FCUT / RES): 12 dB/oct low-pass after the saturator --
  float svf_cut;      // 0.f .. 1.f, edit: FCUT knob (log-mapped 80 Hz .. 12 kHz)
  float svf_res;      // 0.f .. 1.f, edit: RES knob (linear 0.5 .. 2.5 Q)
  float svf_wet;      // 0.f .. 1.f, edit: FCUT dry/wet (0 = raw, 1 = full LPF)
  float rw_fc;        // 0.f .. 1.f, edit: random-walk depth on SVF cutoff (RWFC)

  void reset()
  {
    detune_cents = 0.f;     // default: in tune (sum voice)
    drive = 1.0f;           // default: gentle drive (transparent, ~33% of 0..3)
    rw_amount = 0.f;    // default: random walk off (all three streams)
    // All three streams default to mid period (~3.2 s) / mid time constant
    // (~0.12 s) — same defaults the old single SPEE/SMTH knobs had.
    rw_rate[0] = 0.5f;   // SPD1 (Tone)
    rw_rate[1] = 0.5f;   // SPD2 (Timbre)
    rw_rate[2] = 0.5f;   // SPD3 (Density)
    rw_smooth[0] = 0.5f; // SMT1 (Tone)
    rw_smooth[1] = 0.5f; // SMT2 (Timbre)
    rw_smooth[2] = 0.5f; // SMT3 (Density)
    sub_level = 0.5f;   // default: half-level sub
    sub2_level = 0.2f;  // default: light f/4 sub for low-end body
    fold_depth = 0.f;   // default: wavefolder off
    asym = 0.f;         // default: symmetric saturator
    wave_a = 0.f;       // default: first wave of bank A
    wave_b = 11.f;      // default: a different (still airy) wave from bank A
    chord_level = 0.2f; // default: soft chord voice
    chord_cents = 700.f;// default: perfect fifth (~700 cents)
    rw_wav = 0.f;       // default: walk off for wavetable morph
    rw_drv = 0.f;       // default: walk off for drive
    rw_fld = 0.f;       // default: walk off for fold depth (was 0.5: dead knob at stock RATE=0,
                        // and the multiplexer bug made it sound like a phantom LFO before the
                        // direct-channel fix. 0 keeps the stock sound bit-identical.)
     xfm_a = 0.f;        // default: cross-FM off (A not modulated by B)
     xfm_b = 0.f;        // default: cross-FM off (B not modulated by A)
     svf_cut = 0.9f;     // default: SVF near full-open (~9 kHz); with svf_wet = 0 the
                        // LPF is transparent, so the stock sound is preserved.
     svf_res = 0.f;      // default: Q = 0.5 (over-damped, no resonance peak)
     svf_wet = 0.f;      // default: 0% wet = the SVF is transparent (raw signal).
     rw_fc = 0.f;        // default: walk off for the SVF cutoff
  }

  OscParamsData() { reset(); }
};
