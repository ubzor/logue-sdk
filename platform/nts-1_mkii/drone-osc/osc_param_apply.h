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
      update_w0_det(); // recompute the A/B detuned phase increments from the new detune
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
        params_.rw_amount_lin = x;        // linear: Breath stream only (voice-level wander)
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

    // ---- Wavetable selection (WAV1 / WAV2 / WAV3) ----
    // The raw value maps 0..1023 -> 0..(kWavesTotal-1) across the six banks
    // (91 tables). The morph position is resolved through the interpolating
    // resolve_wave() in the audio path, so a continuous raw value glides
    // between adjacent tables.
    case WAV1:
      params_.wave_a = param_10bit_to_f32(value) * (WaveTable::kWavesTotal - 1);
      recalc_pitch_derived(); // re-resolve wavetables + keep derived voices in sync
      break;
    case WAV2:
      params_.wave_b = param_10bit_to_f32(value) * (WaveTable::kWavesTotal - 1);
      recalc_pitch_derived();
      break;
    case WAV3:
      params_.wave_c = param_10bit_to_f32(value) * (WaveTable::kWavesTotal - 1);
      recalc_pitch_derived();
      break;

    // ---- Chord voice (CHORD level, CINT interval) ----
    case CHORD:
      // Linear chord gain (0..1); the chord's own folder+saturator pass and
      // kMixChordDirect scaling happen in the audio path. See osc_chord.h.
      params_.chord_level = param_10bit_to_f32(value);
      break;
    case CHRD_INT:
      // Chord interval in cents (0..ChordVoice::kMaxCents). Recompute the
      // chord phasor's frequency ratio at K-rate (never in the audio path).
      // params_.chord_cents is the single source of truth (OscParamsData);
      // chord_.update() sets the phasor's w0 from the base pitch + interval.
      //
      // Pitch crossfade: the "new" phasor (chord_new_) takes the FRESH offset
      // and the blend starts from 0 (all old). The OLD phasor (chord_) is NOT
      // re-fretted to the new frequency here -- it keeps running at its current
      // (old) w0, so the per-sample loop (osc_mix.h) genuinely blends the old
      // note into the new one over 2 s instead of jumping. chord_old_cents_ is
      // set to the PRE-CHANGE cents so that a base-pitch change MID-FADE
      // (recalc_pitch_derived / noteOn) keeps the old note at its old frequency
      // (the crossfade is not destroyed). When the crossfade completes,
      // chord_old_cents_ is synced back to params_.chord_cents (osc_mix.h).
      {
        const float new_cents = param_10bit_to_f32(value) * ChordVoice::kMaxCents;
        if (new_cents != prev_chord_cents_)  // only crossfade on an actual change
        {
          chord_new_.update(w0_, new_cents);  // fresh offset, phasor unchanged
          chord_old_cents_ = prev_chord_cents_; // old phasor tracks the pre-change cents
          chord1_cross_ = 0.f;                // start from "all old"
        }
        params_.chord_cents = new_cents;      // single source of truth
        prev_chord_cents_ = new_cents;
        // NOTE: chord_.update() intentionally omitted -- the old phasor keeps
        // its current w0 for the duration of the crossfade (see above).
      }
      break;

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

    // ---- Breath stream (RW_LVL / RW_LSP / RW_LSM): voice-level wandering ----
    // The 4th random-walk stream (rw_[3]) drives the four voice LEVELS
    // (A / B / chord / sub) so they slowly "breathe". RW_LVL is the depth;
    // RW_LSP / RW_LSM are the stream's own period / time constant (independent
    // of the other three streams' SPDx/SMTx). At RW_LVL = 0 (default) the
    // sound is unchanged (all level factors = 1.0).
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
    case CHSAT:
      params_.chord_drive = param_10bit_to_f32(value) * kSatuMax;
      break;
    case CHFLD:
      params_.chord_fold = param_10bit_to_f32(value);
      break;
    // ---- CINT2's OWN drive/fold (CHSAT2 / CHFLD2) ----
    // Independent of both the A+B SATU/FOLD and the CINT voice's CHSAT/CHFLD,
    // so the second interval (chord2) can be driven/filtered separately.
    // Defaults match CHSAT/CHFLD (see OscParamsData::reset) so the stock sound
    // is unchanged until the user splits the drive.
    case CHSAT2:
      params_.chord2_drive = param_10bit_to_f32(value) * kSatuMax;
      break;
    case CHFLD2:
      params_.chord2_fold = param_10bit_to_f32(value);
      break;

    // ---- Enrichment: second chord interval (CINT2) ----
    // A second chord-voice oscillator at a continuous interval above the base
    // (0..ChordVoice::kMaxCents = 1900 c). Shares the chord's level (CHRD),
    // wavetable (WAV3), folder/saturator (CHSAT/CHFLD), and the Breath stream's
    // lvl_c (both intervals breathe in sync — the chord is one object).
    case CINT2:
      // Same pitch crossfade as CINT (see the CHRD_INT case above): the "new"
      // phasor (chord2_new_) takes the fresh offset, the blend starts from 0,
      // and the OLD phasor (chord2_) is NOT touched -- it keeps its current
      // (old) w0 for the duration of the crossfade. Do NOT call
      // recalc_pitch_derived() here: it would re-fret chord2_ (the old phasor)
      // to the new frequency and break the crossfade. chord2_old_cents_ is set
      // to the PRE-CHANGE cents so a base-pitch change MID-FADE keeps the old
      // note at its old frequency (synced back on completion in osc_mix.h).
      {
        const float new_cents = param_10bit_to_f32(value) * ChordVoice::kMaxCents;
        if (new_cents != prev_chord2_cents_)
        {
          chord2_new_.update(w0_, new_cents);
          chord2_old_cents_ = prev_chord2_cents_; // old phasor tracks the pre-change cents
          chord2_cross_ = 0.f;
        }
        params_.chord2_cents = new_cents;      // single source of truth
        update_w0_sub();
        update_w0_sub2();
        prev_chord2_cents_ = new_cents;
      }
      break;

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

    // NOTE: FOLD2 (second cascaded wavefolder) and DCK (sidechain-duck amount)
    // were removed from the editable parameter set. FOLD2 was judged
    // unnecessary; DCK is now a CONSTANT (kDuckConstant, osc_constants.h) and
    // no longer has an apply path. The audio path no longer references
    // params_.fold2_depth / params_.duck_amt.

    default:
      break;
  }
}

