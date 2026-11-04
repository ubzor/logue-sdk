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
  float rw_amount_lin;// 0.f .. 1.f, edit: LINEAR RATE-knob depth, used ONLY by the Breath
                     //            stream (voice-level wandering). Linear so that at
                     //            RATE=50% the breath can still reach full depth
                     //            (dev=-1 -> lvl=0) and voices can fully disappear;
                     //            the quadratic rw_amount would cap breath at 25%
                     //            depth (lvl >= 0.75) at RATE=50%.
  // Three independent wandering streams (Tone / Timbre / Density), each with
  // its own period (SPDx) and time constant (SMTx). They wander with
  // independent random targets on independent (randomly-drawn) periods, so the
  // streams are NOT phase-locked to each other — each "lives its own life".
  // See osc.h for which DSP target each stream drives:
  //   [0] Tone    -> detune A/B + FM (FM_A / FM_B + RFM_A / RFM_B)
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
  // ---- Chord wavetable (WAV3): the third oscillator's own timbre -----------
  // Before the WAV3 edit-param, the chord voice (osc C) was forced to use the
  // same wavetable as osc A (wave_a) in the audio path, so the chord could
  // never sound different from the main voice. WAV3 gives the chord voice its
  // own table index, so the three-voice cluster can span three independent
  // timbres (A / B / C). Default = 0 (the first airy table of bank A), which
  // is a musically-usable, non-aliasing starting point; users who want the
  // old "chord copies A" behavior can set WAV3 to the same value as WAV1.
  float wave_c;       // 0.f .. WaveTable::kWavesTotal-1, edit: osc C (chord) wavetable index
  float chord_level;  // 0.f .. 1.f, edit: chord-voice level
  // ---- MAIN: level of the "main" voice (the A+B mixed output) --------------
  // The "main" voice is the result of mixing oscillators A and B (WAV1 + WAV2)
  // and running them through the folder/saturator/SVF chain — i.e. the
  // `y_filt` branch of the final mix (NOT the sub-octave, sub-2, or chord
  // voices, which are mixed in parallel and bypass the soft-clip). At the
  // default (A+B) this voice usually dominates the drone, so MAIN gives it its
  // own fader: it scales the `y_filt * kMixOsc` term, so 0 = main voice fully
  // ducked (only SUB1/SUB2/chord remain) and 1 = full level. Default = 1.0,
  // which keeps the stock sound bit-identical (regression-safe).
  float main_level;   // 0.f .. 1.f, edit: main (A+B mixed) voice level (MAIN)
  float chord_cents;  // 0.f .. ChordVoice::kMaxCents (1900), edit: chord interval (CINT)
  float rw_wav;       // 0.f .. 1.f, edit: random-walk depth on wavetable morph (RW_WAV)
  float rw_drv;       // 0.f .. 1.f, edit: random-walk depth on saturator drive (RW_DRV)
  float rw_fld;       // 0.f .. 1.f, edit: random-walk depth on wavefolder depth (RW_FLD)
  // ---- Internal SVF (FCUT / RES): 12 dB/oct low-pass after the saturator --
  float svf_cut;      // 0.f .. 1.f, edit: FCUT knob (log-mapped 80 Hz .. 12 kHz)
  float svf_res;      // 0.f .. 1.f, edit: RES knob (linear 0.5 .. 2.5 Q)
  float svf_wet;      // 0.f .. 1.f, edit: FCUT dry/wet (0 = raw, 1 = full LPF)
  float rw_fc;        // 0.f .. 1.f, edit: random-walk depth on SVF cutoff (RWFC)

  // ---- Breath stream (voice-level wandering): RW_LVL / RW_LSP / RW_LSM ----
  // A fourth random-walk stream (rw_[3] in the Osc) drives the four voice
  // LEVELS (A / B / chord / sub) so they slowly "breathe" in and out of
  // prominence — the heart of a drone, which is a cluster of voices trading
  // the floor. The three knobs below are its three degrees of freedom:
  //   rw_lvl  — depth: per-voice level swing (RW_LVL, 0..100% -> 0..1). The
  //             Osc scales each voice by (1 + rw_lvl * dev) so 0 = no
  //             breathing (static levels, regression-safe) and 1 = a full
  //             +/-100% swing.
  //   rw_lsp  — period: how often new level targets are drawn (RW_LSP,
  //             0.1 s .. 10 s, log). Independent of the other three streams.
  //   rw_lsm  — time constant: how quickly levels track their targets (RW_LSM,
  //             10 ms .. 1.5 s, log).
  // The shared RATE (rw_amount) is still the depth CEILING for this stream too,
  // so RATE = 0 disables breathing just as it disables the other walks.
  float rw_lvl;       // 0.f .. 1.f, edit: breath depth on voice levels (RW_LVL)
  float rw_lsp;       // 0.f .. 1.f, edit: breath stream period (RW_LSP, log)
  float rw_lsm;       // 0.f .. 1.f, edit: breath stream time constant (RW_LSM, log)
  // ---- Chord voice's OWN drive/fold (CHSAT / CHFLD) -------------------------
  // Independent of the A+B voice's SATU/FOLD so the chord can be driven (or
  // kept clean) separately. ASYM is shared. Defaults match the A+B SATU/FOLD
  // defaults (drive=1.0, fold=0) so the stock sound is bit-identical.
  float chord_drive;  // 0.f .. 3.f, edit CHSAT: chord's own saturator drive
  float chord_fold;   // 0.f .. 1.f, edit CHFLD: chord's own wavefolder depth
  // ---- Enrichment: second chord interval, jitter, ring-mod, phase-FM ----
  // CINT2: second chord-voice interval (0..ChordVoice::kMaxCents). Shares the
  // chord's level (chord_level/CHRD), wavetable (wave_c/WAV3), folder/saturator
  // (chord_drive/chord_fold via CHSAT/CHFLD), and the Breath stream's lvl_c, so
  // both intervals breathe in sync (the chord is one object). See the CINT2
  // branch in the audio path (osc_mix.h).
  float chord2_cents; // 0.f .. ChordVoice::kMaxCents (1900), edit CINT2
  // JIT: random phase jitter on A/B. The knob maps 0..1023 -> 0..kJitMaxCycles
  // (peak phase deviation, in cycles). The audio path adds (white()-0.5)*jit
  // to each voice's phasor increment, so the carrier "smears" into a dense
  // partial. At 0 (default) the jitter is off and the stock sound is unchanged.
  float jit_cycles;   // 0.f .. kJitMaxCycles, edit JIT (peak phase deviation)
  // RMOD: A*B ring-mod parallel voice. The knob maps 0..1023 -> 0..1 (mix
  // depth). The audio path computes (wav_a * wav_b) * ring_mix and adds it to
  // the final mix (parallel branch, like the chord). At 0 (default) the
  // ring-mod is off and the stock sound is unchanged.
  float ring_mix;     // 0.f .. 1.f, edit RMOD (A*B ring-mod level)
  // FM1/FM2 (FM_A/FM_B): phase modulation of A by B and vice versa. The knob
  // stores the RAW normalized depth (0..1023 -> 0..1); the audio path maps it
  // through the kFmCurveExp (quadratic) curve -> 0..kFmMaxCents (peak phase
  // deviation, in cycles, before the 0.5 normalization). So the low end is a
  // near-silent "breathing" and the sidebands open up smoothly near the top.
  // At 0 (default) the FM is off and the stock sound is unchanged.
  float fm_a;         // 0.f .. 1.f raw depth (curve in the audio path), edit FM1 (STATIC phase-FM on A by B)
  float fm_b;         // 0.f .. 1.f raw depth (curve in the audio path), edit FM2 (STATIC phase-FM on B by A)
  // RFM1/RFM2 (RFM_A/RFM_B): random-walk DEPTH on the FM index (the
  // "откуда гулять" knob, like RDRV on SATU or RWAV on WAV1). The audio path
  // adds (rw_[0].dev * rw_amt * rfm * kFmMaxCents) to the static fm_a/fm_b
  // index, so the FM "breathes" with the random walk on top of the static
  // sidebands. Gated by the shared RATE ceiling (rw_amt). At rfm=0 (default)
  // the FM is purely static (no wandering); at fm=0 & rfm>0 & RATE>0 it is
  // purely "breathing".
  float rfm_a;        // 0.f .. 1.f, edit RFMA (random-walk depth on FM A)
  float rfm_b;        // 0.f .. 1.f, edit RFMB (random-walk depth on FM B)

  void reset()
  {
    detune_cents = 0.f;     // default: in tune (sum voice)
    drive = 1.0f;           // default: gentle drive (transparent, ~33% of 0..3)
    rw_amount = 0.f;    // default: random walk off (all three streams)
    rw_amount_lin = 0.f;// default: breath off (linear RATE depth, same off-state as rw_amount)
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
    wave_c = 0.f;       // default: chord voice starts on the first wave of bank A
    chord_level = 0.2f; // default: soft chord voice
    main_level = 1.0f;  // default: main (A+B) voice at full level (stock sound)
    chord_cents = 700.f;// default: perfect fifth (~700 cents)
    rw_wav = 0.f;       // default: walk off for wavetable morph
    rw_drv = 0.f;       // default: walk off for drive
    rw_fld = 0.f;       // default: walk off for fold depth (was 0.5: dead knob at stock RATE=0,
                        // and the multiplexer bug made it sound like a phantom LFO before the
                        // direct-channel fix. 0 keeps the stock sound bit-identical.)
     svf_cut = 0.9f;     // default: SVF near full-open (~9 kHz); with svf_wet = 0 the
                        // LPF is transparent, so the stock sound is preserved.
     svf_res = 0.f;      // default: Q = 0.5 (over-damped, no resonance peak)
     svf_wet = 0.f;      // default: 0% wet = the SVF is transparent (raw signal).
     rw_fc = 0.f;        // default: walk off for the SVF cutoff
     // ---- Breath stream (voice-level wandering) ----
     rw_lvl = 0.f;       // default: breathing OFF (static voice levels, regression-safe)
     rw_lsp = 0.5f;      // default: mid breath period (~3.2 s, log) — same mid as the others
     rw_lsm = 0.5f;      // default: mid breath time constant (~0.12 s, log)
      // ---- Chord's own drive/fold (CHSAT / CHFLD) ----
      // Defaults match the A+B SATU/FOLD defaults (drive=1.0, fold=0) so the
      // stock sound is unchanged; the chord starts with the same gentle drive
      // and no fold as the main voice.
      chord_drive = 1.0f; // default: gentle drive (same as SATU)
      chord_fold = 0.f;   // default: chord wavefolder off (same as FOLD)
      // ---- Enrichment defaults (CINT2 / JIT / RMOD / FM1 / FM2) ----
      // CINT2 defaults to a major sixth (950 c): with the default CINT=700 c
      // (fifth) and CINT2=950 c (sixth) the stock drone gains a gentle major-
      // sixth "sparkle" on top of the fifth. The user can turn it off by
      // setting CINT2 to 0 (unison) or CHRD to 0 (silences the whole chord).
      // JIT / RMOD / FM1 / FM2 all default to 0 (off), so the enrichment
      // voices are silent at the factory preset.
      chord2_cents = 950.f; // default: major sixth (~950 c)
      jit_cycles   = 0.f;   // default: jitter off
      ring_mix     = 0.f;   // default: ring-mod off
      fm_a         = 0.f;   // default: static phase-FM off (A)
      fm_b         = 0.f;   // default: static phase-FM off (B)
      rfm_a        = 0.f;   // default: FM-A walk off (pure static FM)
      rfm_b        = 0.f;   // default: FM-B walk off (pure static FM)
  }

  OscParamsData() { reset(); }
};
