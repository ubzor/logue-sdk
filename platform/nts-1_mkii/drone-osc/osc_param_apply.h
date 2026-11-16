#pragma once
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: osc_param_apply.h
 *
 *  Out-of-line definition of Osc::apply_edit_param() -- the single
 *  raw 10-bit -> DSP-float converter for every editable parameter.
 *
 *  This was the bulk of osc.h (a ~230-line switch). It now lives here so the
 *  class body stays small. It is #included at the BOTTOM of osc.h, after the
 *  Osc class is complete, so it can reference members (params_, rw_, chord_,
 *  waves_, select_, edit_value_) and the osc_consts::* tuning constants.
 *
 *  It is called from two paths:
 *    1. The PARAM_VAL multiplexer (setParameter(PARAM_VAL) / setEditParam)
 *       applies the selected parameter (kEditParamFirst + select_) using the
 *       current edit_value_[select_] value.
 *    2. The setParameter(DETUNE..CHFLD) path (edit menu / loguecli / web sim)
 *       applies the parameter directly by its enum value.
 *  Both paths share this exact code, so the DSP and the display can never
 *  disagree about a parameter's range.
 *
 */

#include "osc.h"  // Osc class (complete definition) + osc_consts::* + helpers

inline void Osc::apply_edit_param(uint8_t idx, int32_t value)
{
  switch (idx)
  {
    // ---- Static symmetric A/B detune (DETUNE) ----
    // A = +c/2, B = -c/2 (see the DETUNE header comment). The raw value maps
    // 0..1023 -> -kDetuneMaxCents..+kDetuneMaxCents (511 = 0 cents).
    case DETUNE:
      params_.detune_cents = (param_10bit_to_f32(value) * 2.f - 1.f) * kDetuneMaxCents;
      update_det_factors(); // recompute the shared A/B detune factors (per-voice w0 * factor in the audio path)
      break;

    // ---- Shared random-walk depth ceiling (RATE) ----
    // The single RATE knob sets the MAXIMUM depth for ALL three wandering
    // streams (Tone / Timbre / Density). The walk's own max_dev is the
    // normalized value; each stream scales its own deviation onto a DSP
    // parameter with its own knob (RW_WAV / RW_DRV / RW_FLD). Using a SHARED
    // ceiling means turning RATE up opens every wandering stream at once,
    // and RATE = 0 disables all wandering regardless of the per-target knobs.
    // The mapping is quadratic (knob^2) so the low end of the knob is gentle
    // and the depth ramps up slowly, matching a musical perception of "how
    // much does it move".
    case RATE_AMT:
      // Quadratic mapping: raw^2 -> 0..1 normalized depth.
      {
        const float x = param_10bit_to_f32(value);
        params_.rw_amount = x * x;       // quadratic: Tone / Timbre / Density (unchanged)
        params_.rw_amount_lin = x;        // linear: (historically Breath; now unused — Breath is RATE-independent)
      }
      break;

    // ---- Tone stream (SPD1 / SMT1): detune A/B + cross-FM ----
    case SPD1:
      params_.rw_rate[0] = param_10bit_to_f32(value);
      break;
    case SMT1:
      params_.rw_smooth[0] = param_10bit_to_f32(value);
      break;
    // ---- Timbre stream (SPD2 / SMT2): wavetable morph + SVF cutoff ----
    case SPD2:
      params_.rw_rate[1] = param_10bit_to_f32(value);
      break;
    case SMT2:
      params_.rw_smooth[1] = param_10bit_to_f32(value);
      break;
    // ---- Density stream (SPD3 / SMT3): drive + wavefolder ----
    case SPD3:
      params_.rw_rate[2] = param_10bit_to_f32(value);
      break;
    case SMT3:
      params_.rw_smooth[2] = param_10bit_to_f32(value);
      break;

    // ---- Sub voices (SUB1 = f/2, SUB2 = f/4) ----
    case SUB1:
      params_.sub_level = param_10bit_to_f32(value);
      break;
    case SUB2:
      params_.sub2_level = param_10bit_to_f32(value);
      break;
    // ---- Saturator drive / asymmetry / wavefolder (SATU / ASYM / FOLD) ----
    case SATU:
      params_.drive = param_10bit_to_f32(value) * kSatuMax;
      break;
    case ASYM:
      params_.asym = (param_10bit_to_f32(value) * 2.f - 1.f);
      break;
    case FOLD:
      params_.fold_depth = param_10bit_to_f32(value);
      break;

    // ---- Wavetable selection (WAVE / WAV2-deprecated) ----
    // WAVE (WAV1) selects the BANK (0..5, A..F). The LFO drives the
    // intra-bank morph position centered on the bank (see
    // WaveTable::resolve_wave / bank_center). The bank boundary is hard: the
    // morph never crosses into the next bank.
    // WAV2 is deprecated (both A/B now read the same wave) — no-op.
    case WAV1:
    {
      float raw = param_10bit_to_f32(value);
      int32_t bank = (int32_t)(raw * 5.999f);
      if (bank < 0) bank = 0;
      if (bank > 5) bank = 5;
      params_.wave_pos = (float)bank;
      break;
    }
    case WAV2:
      // Second wave stream's bank (A..F, 0..5). Same quantization as WAV1:
      // raw 0..1023 -> bank 0..5. Was deprecated (A/B shared one wave);
      // revived as the 2D-morph stream-2 bank (the audio path crossfades
      // stream 1 and stream 2 by WAVE_MIX, see the "2D wave crossfade"
      // block in osc_mix.h).
    {
      float raw = param_10bit_to_f32(value);
      int32_t bank = (int32_t)(raw * 5.999f);
      if (bank < 0) bank = 0;
      if (bank > 5) bank = 5;
      params_.wave_pos2 = (float)bank;
      break;
    }

    // ---- Chord voice (CHORD level, CINT interval) ----

    // ---- MAIN (A+B mixed) voice level ----
    // (y_filt * kMixOsc * main_level) is applied in the audio path; here we
    // only store the 0..1 gain. At 100% (default) the gain is 1.0, so the
    // stock sound is preserved.
    case MAIN:
      params_.main_level = param_10bit_to_f32(value);
      break;

    // ---- Random-walk per-target depths (RW_WAV / RW_DRV / RW_FLD) ----
    // These scale the deviation of each wandering stream; the shared RATE
    // knob (rw_amount) is the overall ceiling applied in the audio path.
    case RW_WAV:
      params_.rw_wav = param_10bit_to_f32(value);
      break;
    case RW_DRV:
      params_.rw_drv = param_10bit_to_f32(value);
      break;
    case RW_FLD:
      params_.rw_fld = param_10bit_to_f32(value);
      break;

    // ---- Internal SVF (FCUT cutoff + dry/wet, RES resonance) ----
    // The cutoff knob maps 0..1023 -> a LOG frequency 80 Hz..12 kHz AND a
    // linear dry/wet 0..1 (0% = raw signal, 100% = full LPF). At the default
    // (svf_wet = 0) the SVF is transparent, so the stock sound is unchanged.
    case FCUT:
      {
        const float x = param_10bit_to_f32(value);
        params_.svf_cut = x;
        params_.svf_wet = x;
        // Recompute the SVF coefficients at K-rate (the audio path only adds
        // the per-sample cc from the random walk). See osc_svf.h.
        // Log sweep 80 Hz..12 kHz. fasterpowf (not libm powf) to avoid an
        // unresolved libm dynamic symbol on the device.
        const float freq = 80.f * fasterpowf(12000.f / 80.f, x);
        svf_.updateCutoff(freq, 0.5f, kSampleRate);
      }
      break;
    case RES:
      {
        // Q = 0.5 + 2.0 * x  (x in 0..1) -> 0.5..2.5
        const float x = param_10bit_to_f32(value);
        params_.svf_res = x;
        const float freq = 80.f * fasterpowf(12000.f / 80.f, params_.svf_cut);
        svf_.updateCutoff(freq, 0.5f + 2.0f * x, kSampleRate);
      }
      break;
    case RW_FC:
      // Random-walk depth on the SVF cutoff (Timbre stream).
      params_.rw_fc = param_10bit_to_f32(value);
      break;

    // ---- Breath stream (RW_LVL / RW_LSP / RW_LSM): PER-VOICE level wandering ----
    // The 4th random-walk stream (rw_[3]) drives the MAIN voice level of EACH
    // cluster note independently, so the notes "breathe" in/out of prominence
    // relative to each other. INDEPENDENT of RATE: RW_LVL > 0 enables breathing
    // even at RATE = 0. RW_LVL is the depth; RW_LSP / RW_LSM are the stream's
    // own period / time constant (independent of the other three streams'
    // SPDx/SMTx). Only the MAIN (A+B) voice is breathed; SUB1/SUB2 stay static.
    // At RW_LVL = 0 (default) the sound is unchanged (all level factors = 1.0).
    case RW_LVL:
      params_.rw_lvl = param_10bit_to_f32(value);
      break;
    case RW_LSP:
      params_.rw_lsp = param_10bit_to_f32(value);
      rw_[3].setup(params_.rw_lsp, params_.rw_amount, params_.rw_lsm);
      break;
    case RW_LSM:
      params_.rw_lsm = param_10bit_to_f32(value);
      rw_[3].setup(params_.rw_lsp, params_.rw_amount, params_.rw_lsm);
      break;

    // ---- Chord voice's OWN drive/fold (CHSAT / CHFLD) ----
    // Independent of the A+B SATU/FOLD so the chord can be driven (or kept
    // clean) separately. ASYM is shared. Defaults match the A+B defaults so
    // the stock sound is bit-identical.
    // ---- CINT2's OWN drive/fold (CHSAT2 / CHFLD2) ----
    // Independent of both the A+B SATU/FOLD and the CINT voice's CHSAT/CHFLD,
    // so the second interval (chord2) can be driven/filtered separately.
    // Defaults match CHSAT/CHFLD (see OscParamsData::reset) so the stock sound
    // is unchanged until the user splits the drive.

    // (CINT2 / CHSAT2 / CHFLD2 were removed — the chord/interval concept is gone.)

    // ---- Enrichment: random phase jitter on A/B (JIT) ----
    // The knob maps 0..1023 -> 0..kJitMaxCycles (peak phase deviation, cycles).
    // The audio path adds (osc_white() - 0.5) * jit_cycles to each voice's
    // phasor increment, "smearing" the carrier into a dense partial. At 0 the
    // jitter is off (regression-safe). The per-sample white() draws happen in
    // process() (never here, K-rate only).
    case JIT:
      params_.jit_cycles = param_10bit_to_f32(value) * kJitMaxCycles;
      break;

    // ---- Enrichment: A*B ring-mod derived voice (RMOD, MAIN-ducked) ----
    // The knob maps 0..1023 -> 0..1 (mix depth). The audio path computes
    // (wav_a * wav_b) * ring_mix * main_level and adds it to the final mix as
    // a parallel branch (not in the A+B saturator chain, but ducked by MAIN:
    // MAIN=0 silences the whole main-voice cluster at once). At 0 the
    // ring-mod is off (regression-safe).
    case RMOD:
      params_.ring_mix = param_10bit_to_f32(value);
      break;

    // ---- Enrichment: phase-FM (FM_A / FM_B = STATIC index, RFM_A/B = walk) ----
    // Classic phase modulation of A by B (FM_A) and B by A (FM_B).
    // FM_A/FM_B are the STATIC index. The knob stores the RAW normalized
    // depth (0..1023 -> 0..1); the audio path (osc_mix.h) maps it through the
    // kFmCurveExp (quadratic) depth curve -> 0..kFmMaxCents and the 0.5
    // normalization to the peak index in cycles (see the fm_a_curve/fm_b_curve
    // lines). Unlike the old RW-gated version, the static index is NOT gated
    // by RATE — turning FM on gives constant sidebands immediately (a classic
    // FM index), no walk required. RFM_A/RFM_B (0..1) are the random-walk
    // DEPTH on that index: the audio path adds (rw_[0].dev * rw_amt * rfm *
    // kFmMaxCents) so the FM "breathes" with the walk on top of the static
    // sidebands (gated by RATE, like every other walk). At fm=0 & rfm=0 the FM
    // is fully off.
    case FM_A:
      params_.fm_a = param_10bit_to_f32(value); // raw 0..1 depth (curve in the audio path)
      break;
    case FM_B:
      params_.fm_b = param_10bit_to_f32(value); // raw 0..1 depth (curve in the audio path)
      break;
    case RFM_A:
      params_.rfm_a = param_10bit_to_f32(value);
      break;
    case RFM_B:
      params_.rfm_b = param_10bit_to_f32(value);
      break;

    // ---- Enrichment-2: formant vowel position (FORM) + dry/wet (FVOL) ----
    // FORM maps 0..1023 -> 0..1 (vowel morph position across A-E-I-O-U). The
    // audio path crossfades the per-vowel biquad coefficients between the two
    // nearest stops, so the vowel "slides". FVOL is the formant stage's
    // dry/wet (0..1); at 0 the formant contributes nothing (regression-safe),
    // so the two knobs are independent: FORM can be dialed in silently, then
    // FVOL fades the formant in. Changing either re-computes the (K-rate)
    // formant biquad coefficients + morph weights.
    case FORM:
      params_.formant_pos = param_10bit_to_f32(value);
      recompute_formant_comb(); // K-rate: 10 biquad coefs + morph weights
      break;
    case FVOL:
      params_.formant_wet = param_10bit_to_f32(value);
      recompute_formant_comb(); // K-rate (the weights only depend on FORM, but
                                   // recompute keeps the cached coefs consistent)
      break;

    // ---- Enrichment-2: comb resonator feedback (COMB) ----
    // The knob maps 0..1023 -> 0..kCombFbMax (the feedback coefficient of the
    // one-pole IIR comb, clamped < 1 for stability). The comb delay is a fixed
    // constant (kCombDelaySmp, ~190 Hz peak); only the feedback is settable.
    // At 0 the comb is a pure wire (regression-safe). No K-rate recompute
    // needed (the delay length is constant).
    case COMB:
      params_.comb_fb = param_10bit_to_f32(value) * kCombFbMax;
      break;

    // ---- SUBB: sub-bass "breath" depth (shared sub follows the focus voice) ----
    // The knob maps 0..1023 -> 0..1 (linear). The audio path (osc_mix.h)
    // converts it to a swing depth d = subb_depth * kSubBreathMax and a
    // one-pole follower (kSubBreathTauSec) drives sub_breath_gain toward
    // (1 + (sub_env_focus - 0.5) * 2 * d) so the low end swells with the
    // loudest (breath-focus) voice. At 0 the gain is constant 1.0 (static
    // low end, the old behaviour). No K-rate recompute needed (the follower
    // state is audio-path state, relaxed per sample).
    case SUBB:
      params_.subb_depth = param_10bit_to_f32(value);
      break;

    // ---- Internal wave-LFO frequency (WAVE_LFO_RATE) -------------------------
    // LOG scale (a LINEAR knob mapped to an EXPONENTIAL frequency, 3 decades):
    // raw 0..1023 -> x = raw/1023 (0..1); freq = 10^(3x - 3) Hz, so each
    // equal knob step is a constant MULTIPLIER of the previous frequency
    // (0.001 -> 0.01 -> 0.1 -> 1 Hz). This is the same log convention as the
    // SPDx/SMTx knobs. raw 0 is SPECIAL-CASED to 0 Hz (LFO OFF, the phase
    // does not advance):
    //   raw = 0    -> 0 Hz (LFO OFF, static wave -- the factory default)
    //   raw = 1    -> 10^(-2.997) ~ 0.001 Hz (start of the log scale)
    //   raw = 341  -> 10^(3*0.333-3) = 10^-2 = 0.01 Hz
    //   raw = 683  -> 10^(3*0.667-3) = 10^-1 = 0.1 Hz
    //   raw = 1023 -> 10^(3*1-3)     = 10^0  = 1 Hz (the max)
    // Written to lfo_sim_.freq_hz (the internal LFO, see osc_lfo_sim.h).
    // fasterpowf(10.f, x) is the platform's fast powf (no libm).
    case WAVE_LFO_RATE:
      {
        if (value == 0)
          lfo_sim_.freq_hz = 0.f; // raw 0 = LFO OFF (0 Hz)
        else
        {
          const float x = param_10bit_to_f32(value); // 0..1
          lfo_sim_.freq_hz = fasterpowf(10.f, 3.f * x - 3.f); // 0.001..1 (log)
        }
      }
      break;

    // ---- Internal wave-LFO morph depth (WAVE_LFO_DEPTH) ----------------------
    // LINEAR scale, 0..100% (raw 0..1023 -> 0..1). At 0 the LFO is off (the
    // wave stays at the bank center, static); at 100% the morph sweeps the
    // full bank (first to last table). Written to lfo_sim_.morph_amt.
    case WAVE_LFO_DEPTH:
      lfo_sim_.morph_amt = param_10bit_to_f32(value); // 0..1
      break;

    // ---- Second wave stream: internal wave-LFO 2 frequency (WAVE2_LFO_RATE) --
    // Identical LOG scale to WAVE_LFO_RATE (stream 1), but written to the
    // SECOND LFO (lfo_sim2_, see osc_lfo_sim.h): raw 0 = 0 Hz (LFO OFF, the
    // stream-2 wave stays at its bank center, static); raw 1..1023 ->
    // 10^(3*raw/1023 - 3) = 0.001..1 Hz (3 decades). The two LFOs run with
    // INDEPENDENT phases (each ShapeLfoSim owns its own phase), so with two
    // incommensurate frequencies (e.g. 13 s and 19 s) the (stream 1, stream 2)
    // morph trajectory never repeats.
    case WAVE2_LFO_RATE:
      {
        if (value == 0)
          lfo_sim2_.freq_hz = 0.f; // raw 0 = LFO 2 OFF (0 Hz)
        else
        {
          const float x = param_10bit_to_f32(value); // 0..1
          lfo_sim2_.freq_hz = fasterpowf(10.f, 3.f * x - 3.f); // 0.001..1 (log)
        }
      }
      break;

    // ---- Second wave stream: internal wave-LFO 2 morph depth (WAVE2_LFO_DEPTH)
    // LINEAR scale, 0..100% (raw 0..1023 -> 0..1), same as WAVE_LFO_DEPTH but
    // for lfo_sim2_. At 0 the stream-2 wave stays at its bank center (static);
    // at 100% the LFO sweeps the full stream-2 bank. Written to
    // lfo_sim2_.morph_amt.
    case WAVE2_LFO_DEPTH:
      lfo_sim2_.morph_amt = param_10bit_to_f32(value); // 0..1
      break;

    // ---- Wave 1 <-> Wave 2 crossfade (WAVE_MIX) ------------------------------
    // SYMMETRIC -100%..+100% (raw 0..1023 -> -1..+1, 511 = 0 = 50/50). The
    // audio path (osc_mix.h) computes the crossfade weight x = 0.5*(1+mix):
    //   raw 0    -> mix = -1 -> x = 0   (stream 1 only, stream 2 silent)
    //   raw 511  -> mix =  0 -> x = 0.5 (equal 50/50 blend, the default)
    //   raw 1023 -> mix = +1 -> x = 1   (stream 2 only, stream 1 silent)
    // Written to params_.wave_mix (-1..+1). At the factory preset (stream 2 =
    // bank A, LFO 2 off) stream 2 == stream 1, so any mix value is bit-identical
    // to the old single-stream sound.
    case WAVE_MIX:
      params_.wave_mix = param_10bit_to_f32(value) * 2.f - 1.f; // -1..+1
      break;

    // NOTE: FOLD2 (second cascaded wavefolder) and DCK (sidechain-duck amount)
    // were removed from the editable parameter set. FOLD2 was judged
    // unnecessary; DCK is now a CONSTANT (kDuckConstant, osc_constants.h) and
    // no longer has an apply path. The audio path no longer references
    // params_.fold2_depth / params_.duck_amt.

    default:
      break;
  }
}

