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
  float rw_amount_lin;// 0.f .. 1.f, edit: LINEAR RATE-knob depth. (Historically
                     //            used by the Breath stream; since the Breath
                     //            stream became RATE-independent (gated only by
                     //            RW_LVL), this field is no longer read by the
                     //            audio path. Kept for struct compatibility.)
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
  float wave_pos;     // 0.f .. 5.f, edit WAVE: bank index (A=0..F=5)
                      // The LFO drives the intra-bank morph position centered
                      // on the bank (see WaveTable::resolve_wave / bank_center).
  float wave_pos2;    // 0.f .. 5.f, edit WAV2: second wave stream's bank index
                      // (A=0..F=5). Wave-LFO 2 (lfo_sim2_) drives the intra-bank
                      // morph position centered on this bank, INDEPENDENTLY of
                      // stream 1. The audio path crossfades stream 1 and stream
                      // 2 by wave_mix (see WaveTable::resolve_wave / the
                      // "2D wave crossfade" block in osc_mix.h).
  float wave_mix;     // -1.f .. +1.f, edit WAVE_MIX: the wave 1 <-> wave 2
                      // crossfade weight. The audio path computes
                      // x = 0.5 * (1 + wave_mix): x = 0 (mix = -1) -> stream 1
                      // only, x = 0.5 (mix = 0) -> equal 50/50 blend, x = 1
                      // (mix = +1) -> stream 2 only. default 0 = 50/50.
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
  float rw_wav;       // 0.f .. 1.f, edit: random-walk depth on wavetable morph (RW_WAV)
  float rw_drv;       // 0.f .. 1.f, edit: random-walk depth on saturator drive (RW_DRV)
  float rw_fld;       // 0.f .. 1.f, edit: random-walk depth on wavefolder depth (RW_FLD)
  // ---- Internal SVF (FCUT / RES): 12 dB/oct low-pass after the saturator --
  float svf_cut;      // 0.f .. 1.f, edit: FCUT knob (log-mapped 80 Hz .. 12 kHz)
  float svf_res;      // 0.f .. 1.f, edit: RES knob (linear 0.5 .. 2.5 Q)
  float svf_wet;      // 0.f .. 1.f, edit: FCUT dry/wet (0 = raw, 1 = full LPF) -- stage 1 (12 dB/oct)
  float svf_wet2;     // 0.f .. 1.f, edit: FCUT2 dry/wet (0 = raw, 1 = full LPF) -- stage 2 (12->24 dB/oct)
  float rw_fc;        // 0.f .. 1.f, edit: random-walk depth on SVF cutoff (RWFC)

  // ---- Breath stream (PER-VOICE level wandering): RW_LVL / RW_LSP / RW_LSM ----
  // The fourth random-walk stream (rw_[3] in the Osc) drives the MAIN voice
  // level of EACH cluster note INDEPENDENTLY, so the notes slowly "breathe"
  // in and out of prominence relative to each other — the heart of a drone.
  // INDEPENDENT of RATE: breathing is gated ONLY by rw_lvl (Breath Depth);
  // RATE = 0 no longer disables breathing (the other three wandering streams
  // are still RATE-gated). Only the MAIN (A+B) voice is breathed; the sub-1
  // (f/2) and sub-2 (f/4) voices stay static (the low-end anchor never
  // "flutters"). The three knobs below are its three degrees of freedom:
  //   rw_lvl  — depth: per-voice level swing (RW_LVL, 0..100% -> 0..1). The
  //             Osc scales each voice by (1 + rw_lvl * 2 * breath_dev) so 0 =
  //             no breathing (static levels, regression-safe) and 1 = a full
  //             +/-100% swing (dev=-1 -> lvl=0, voice disappears).
  //   rw_lsp  — period: how often new per-voice targets are drawn (RW_LSP,
  //             0.1 s .. 10 s, log). Independent of the other three streams.
  //   rw_lsm  — time constant: how quickly each voice's level tracks its
  //             target (RW_LSM, 10 ms .. 1.5 s, log).
  float rw_lvl;       // 0.f .. 1.f, edit: per-voice breath depth on MAIN voice (RW_LVL)
  float rw_lsp;       // 0.f .. 1.f, edit: breath stream period (RW_LSP, log)
  float rw_lsm;       // 0.f .. 1.f, edit: breath stream time constant (RW_LSM, log)
  // (chord_drive / chord_fold / chord2_cents / chord2_drive / chord2_fold were
  //  removed — the chord/interval concept is gone.)
  // JIT: random phase jitter on A/B. The knob maps 0..1023 -> 0..kJitMaxCycles
  // (peak phase deviation, in cycles). The audio path adds (white()-0.5)*jit
  // to each voice's phasor increment, so the carrier "smears" into a dense
  // partial. At 0 (default) the jitter is off and the stock sound is unchanged.
  float jit_cycles;   // 0.f .. kJitMaxCycles, edit JIT (peak phase deviation)
  // RMOD: A*B ring-mod derived voice (parallel, MAIN-ducked). The knob maps
  // 0..1023 -> 0..1 (mix depth). The audio path computes (wav_a * wav_b) *
  // ring_mix * main_level and adds it to the final mix (parallel branch: not
  // in the A+B saturator chain, but ducked by MAIN like A+B itself). At 0
  // (default) the ring-mod is off and the stock sound is unchanged.
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
  // ---- FM modulator C: an INDEPENDENT oscillator that phase-FMs the (A+B)
  //  SUM (the drone carrier). Classic FM: C is the modulator, A+B the carrier.
  //  C is NOT heard by itself -- it only bends the sum's phase for vibrato /
  //  motion colour on top of the (untouched) cross-FM A+B timbre. The audio
  //  path (osc_mix.h) advances a C phasor at fm_c_rate and adds
  //  fm_c_depth * kFmCMaxCycles * C_wave to the sum's phase. At fm_c_rate = 0
  //  (default) C is off and the stock sound is bit-identical. (This is the
  //  "dedicated, always-running modulator" the former FM bus was removed for.)
  float fm_c_rate;    // 0.f .. 1.f, edit FMCR (C frequency, LOG 0..1 Hz)
  uint8_t fm_c_wave;  // 0=SIN, 1=SAW, 2=SQR, 3=NOISE, edit FMCW
  float fm_c_depth;   // 0.f .. 1.f, edit FMCD (C depth, LINEAR)
  // ---- FM modulator S (static teeth): a carrier-locked FM oscillator --------
  // S's rate is LOCKED to the (A+B) carrier: it runs at `fm_static` teeth per
  // carrier cycle (FMST, log kFmStaticMinTeeth..kFmStaticMaxTeeth; <1 =
  // sub-octave, fm/fc < 1), phase-FMing the (A+B) sum with N teeth of
  // deviation per cycle (a static harmonic comb). The audio
  // path (osc_mix.h) advances an S phasor by `fm_static` x the A phasor advance
  // per sample and adds `fm_static_amp * kFmStaticMaxCycles * S_wave` to both
  // A and B phasors. At fm_static = 0 (default) or fm_static_amp = 0 S is off
  // (bit-identical). Combined with C: S is the static (note-locked) comb, C is
  // the free-running low-frequency vibrato.
  float fm_static;    // 0.f .. kFmStaticMaxTeeth, edit FMST (teeth per carrier cycle, LOG 0.05..16)
  float fm_static_amp; // 0.f .. 1.f, edit FMSA (tooth amplitude / FM depth, LINEAR)
  // NOTE: the tooth SHAPE is shared with the C modulator (fm_c_wave, edit FM_CW
  // / FM Mod Wave) -- S re-uses the same SIN/SAW/SQR/NOISE selector, so there
  // is no separate fm_static_wave field.
  // ---- FM AMT: the GLOBAL FM MOD amount (master multiplier on C + S) --------
  // A single knob (LINEAR 0..100%) that multiplies BOTH the C (vibrato) and S
  // (static teeth) phase offsets in the audio path (osc_mix.h) before they are
  // added to the A/B phasors. At 1.0f (default, 100%) the offsets are
  // unchanged (bit-identical regression-safe); at 0.f the whole FM MOD block
  // is OFF regardless of FMCD/FMSA (both offsets are exactly 0). It scales the
  // DEVIATION only, never the C/S rates (FMCR/FMST) or shapes (FM_CW).
  float fm_amt;       // 0.f .. 1.f, edit FMAM (global FM MOD amount, LINEAR)

  // ---- Enrichment-2 (formant / comb) -----------------------------------------
  // All default to OFF (transparent) so the stock sound is bit-identical.
  // (FOLD2 and DCK were removed — FOLD2 was judged unnecessary, DCK is now a
  //  constant (kDuckConstant, osc_constants.h); no DSP state for them here.)
  float formant_pos;  // 0.f .. 1.f, edit FORM: vowel morph position (A=0 .. U=1)
  float formant_wet;  // 0.f .. 1.f, edit FVOL: formant stage dry/wet (0 = dry)
  float comb_fb;      // 0.f .. kCombFbMax, edit COMB (feedback, < 1 = stable)
  // ---- SUBB: sub-bass "breath" depth (the shared sub bass follows the
  // breath-focus voice) ----
  // The SHARED sub bass (SUB1/SUB2, one pair for the whole polyphonic cluster)
  // can BREATHE: its level tracks the loudest (breath-focus) voice's envelope
  // with a one-pole follower (kSubBreathTauSec), swinging in [1-d, 1+d] where
  // d = subb_depth * kSubBreathMax. At 0 (would be regression-safe) the gain
  // is constant 1.0 (the old static low end); the DEFAULT is 0.3 (~30% ->
  // d ~= 0.18, a gentle "the low end follows the breathing note"). Gated
  // INDEPENDENTLY of the BRT (RW_LVL) knob: SUBB > 0 makes the sub bass follow
  // the loudest voice even with per-voice breathing off (the envelope max is
  // used, so a single note = constant gain, no flutter).
  float subb_depth;   // 0.f .. 1.f, edit SUBB (sub-bass breath depth)

  // (GLIDE removed: no glide_raw field)

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
    sub2_level = 0.4f;  // default: ~40% f/4 sub (a clearly audible low-end anchor)
    fold_depth = 0.f;   // default: wavefolder off
    asym = 0.f;         // default: symmetric saturator
    wave_pos = 0.f;     // default: bank A (LFO sweeps within bank A)
    wave_pos2 = 0.f;    // default: stream-2 bank A (SAME as stream 1 at factory)
    wave_mix = 0.f;     // default: 50/50 crossfade; with stream 2 = bank A and
                        // its LFO off, the blend of two identical waves is
                        // bit-identical to the old single-stream sound
    main_level = 1.0f;  // default: main (A+B) voice at full level (stock sound)
    rw_wav = 0.f;       // default: walk off for wavetable morph
    rw_drv = 0.f;       // default: walk off for drive
    rw_fld = 0.f;       // default: walk off for fold depth (was 0.5: dead knob at stock RATE=0,
                        // and the multiplexer bug made it sound like a phantom LFO before the
                        // direct-channel fix. 0 keeps the stock sound bit-identical.)
     svf_cut = 0.9f;     // default: SVF near full-open (~9 kHz); with svf_wet = 0 the
                        // LPF is transparent, so the stock sound is preserved.
     svf_res = 0.f;      // default: Q = 0.5 (over-damped, no resonance peak)
     svf_wet = 0.f;      // default: 0% wet = the SVF is transparent (raw signal).
     svf_wet2 = 0.f;     // default: 0% wet = the 2nd SVF stage is transparent (12 dB/oct).
     rw_fc = 0.f;        // default: walk off for the SVF cutoff
     // ---- Breath stream (voice-level wandering) ----
     rw_lvl = 0.f;       // default: breathing OFF (static voice levels, regression-safe)
     rw_lsp = 0.5f;      // default: mid breath period (~3.2 s, log) — same mid as the others
     rw_lsm = 0.5f;      // default: mid breath time constant (~0.12 s, log)
      // ---- Chord's own drive/fold (CHSAT / CHFLD) ----
      // Defaults match the A+B SATU/FOLD defaults (drive=1.0, fold=0) so the
      // stock sound is unchanged; the chord starts with the same gentle drive
      // and no fold as the main voice.
      // (CINT2 / CHSAT2 / CHFLD2 were removed — the chord/interval concept is gone.)
      // ---- Enrichment defaults (JIT / RMOD / FM1 / FM2) ----
      // JIT / RMOD / FM1 / FM2 all default to 0 (off), so the enrichment
      // voices are silent at the factory preset.
      jit_cycles   = 0.f;   // default: jitter off
      ring_mix     = 0.f;   // default: ring-mod off
      fm_a         = 0.f;   // default: static phase-FM off (A)
      fm_b         = 0.f;   // default: static phase-FM off (B)
      rfm_a        = 0.f;   // default: FM-A walk off (pure static FM)
      rfm_b        = 0.f;   // default: FM-B walk off (pure static FM)
      // ---- FM modulator C defaults (OFF, regression-safe) ----
      fm_c_rate    = 0.f;   // default: C OFF (the (A+B) sum is unmodulated)
      fm_c_wave    = 0;     // default: SIN
      fm_c_depth   = 0.f;   // default: C OFF (no phase deviation)
      fm_static    = 0.f;   // default: S OFF (0 teeth, the (A+B) sum is unmodulated)
      fm_static_amp= 1.f;   // default: full amplitude (100%); with fm_static=0 the offset is 0
      fm_amt       = 1.f;   // default: 100% -- the global FM MOD master is fully open
                             // (C/S offsets pass through unchanged, bit-identical)
      // ---- Enrichment-2 defaults (all OFF, regression-safe) ----
      // FORM sits at the middle vowel, but the formant stage is fully DRY
      // (formant_wet = 0) so it contributes nothing until the user raises
      // FVOL. Every other enrichment-2 knob defaults to 0 (off / transparent).
      formant_pos  = 0.5f;  // default: middle vowel (A-E-I-O-U morph midpoint)
      formant_wet  = 0.f;   // default: formant OFF (dry = transparent)
      comb_fb      = 0.f;   // default: comb resonator off
      subb_depth   = 0.3f;  // default: ~30% (d ~= 0.18) — the shared sub bass gently
                            // follows the breath-focus note (see the SUBB comment)
  }

  OscParamsData() { reset(); }
};
