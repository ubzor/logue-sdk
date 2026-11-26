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
    // NO-OP: the SUB1/SUB2 sub-octave voices are REMOVED -- the low end now
    // lives INSIDE the WTE wavetable as subharmonic partials (see
    // wte_default_wave in osc_wte_doc.h). The edit params remain in the
    // multiplexer (stable raw indices) but no longer drive any DSP.
    case SUB1:
    case SUB2:
      break; // removed (low end is in the wavetable)
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

    // ---- Wavetable selection (WAVE / WAV2) — REMOVED (no-op) -----------------
    // The stock banks (A..F) are gone: the A+B voices read the EDITOR
    // wavetable (the 16x256 WTE table from the /editor document, see
    // osc_wte.h / the "Wavetable samples (WTE)" block in osc_mix.h) as their
    // sole wavetable source. WAV1 (WAVE) and WAV2 (WAVE 2, the old stream-2
    // bank) are no-ops kept for the enum/preset layout. The table morph is
    // driven by the Wave LFO 1 params below (WAVE_LFO_RATE/DEPTH/SHAPE/GAP).
    case WAV1:
    case WAV2:
      break; // superseded by the WTE wavetable

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
        // Q = 0.5 + 2.0 * svf_res (same as the RES knob). The cascade
        // (24 dB/oct, see svf2_) mirrors the same coefficient: 2nd-order -> 24 dB/oct.
        const float q = 0.5f + 2.0f * params_.svf_res;
        svf_.updateCutoff(freq, q, kSampleRate);
        svf2_.updateCutoff(freq, q, kSampleRate);
      }
      break;
    case RES:
      {
        // Q = 0.5 + 2.0 * x  (x in 0..1) -> 0.5..2.5
        const float x = param_10bit_to_f32(value);
        params_.svf_res = x;
        const float freq = 80.f * fasterpowf(12000.f / 80.f, params_.svf_cut);
        const float q = 0.5f + 2.0f * x;
        svf_.updateCutoff(freq, q, kSampleRate);
        svf2_.updateCutoff(freq, q, kSampleRate);
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
      params_.fm_a = param_10bit_to_f32(value); // raw 0..1 LOG depth (audio path: 10^(x*5-5)*0.2 cycles)
      break;
    case FM_B:
      params_.fm_b = param_10bit_to_f32(value); // raw 0..1 LOG depth (audio path: 10^(x*5-5)*0.2 cycles)
      break;
    case RFM_A:
      params_.rfm_a = param_10bit_to_f32(value); // raw 0..1 LINEAR depth (audio path: *0.2 cycles)
      break;
    case RFM_B:
      params_.rfm_b = param_10bit_to_f32(value); // raw 0..1 LINEAR depth (audio path: *0.2 cycles)
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

    // ---- SUBB: sub-bass "breath" depth ----
    // NO-OP: the shared sub bass (SUB1/SUB2) is REMOVED -- the low end now
    // lives inside the WTE wavetable as subharmonic partials, so there is no
    // "sub-bass breath" to drive. The edit param remains in the multiplexer
    // (stable raw index) but no longer drives any DSP.
    case SUBB:
      break; // removed (low end is in the wavetable)

    // ---- Internal wave-LFO frequency (WAVE_LFO_RATE) -------------------------
    // This LFO now drives the EDITOR wavetable morph: the A+B voices read the
    // 16x256 WTE table (osc_wte.h) at position 7.5 + lfo * 7.5 (see the
    // "WTE wavetable morph" block in osc_mix.h) -- at depth 100% the morph
    // sweeps the full 0..15 frame range (0 = first editor wave, 15 = last).
    // LOG scale (a LINEAR knob mapped to an EXPONENTIAL frequency, 3 decades):
    // raw 0..1023 -> x = raw/1023 (0..1); freq = 10^(3x - 3) Hz, so each
    // equal knob step is a constant MULTIPLIER of the previous frequency
    // (0.001 -> 0.01 -> 0.1 -> 1 Hz). This is the same log convention as the
    // SPDx/SMTx knobs. raw 0 is SPECIAL-CASED to 0 Hz (LFO OFF, the phase
    // does not advance):
    //   raw = 0    -> 0 Hz (LFO OFF, the table is static at frame 7.5)
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
        lfo_sim_.invalidate(); // new period -> new point count, apply now
      }
      break;

    // ---- Internal wave-LFO morph depth (WAVE_LFO_DEPTH) ----------------------
    // LINEAR scale, 0..100% (raw 0..1023 -> 0..1). At 0 the LFO is off (the
    // WTE table stays static at its middle frame, frame 7.5); at 100% the
    // morph sweeps the FULL table (frame 0 to frame 15). Written to
    // lfo_sim_.morph_amt.
    case WAVE_LFO_DEPTH:
      lfo_sim_.morph_amt = param_10bit_to_f32(value); // 0..1
      break;

    // ---- Second wave stream (W2LFR / W2LFD / WAVE_MIX) — REMOVED (no-op) -----
    // Stream 2 (the old WAV2 bank + its own LFO) and the WAVE_MIX stream 1 <->
    // stream 2 crossfade are gone with the stock banks: the A+B voices read
    // the single WTE table (see the WAVE_LFO_RATE block above). These edit
    // params are no-ops kept for the enum/preset layout; lfo_sim2_ no longer
    // processes in the audio path.
    case WAVE2_LFO_RATE:
    case WAVE2_LFO_DEPTH:
    case WAVE_MIX:
      break; // superseded by the WTE wavetable

    // ---- LFO shape (stream 1) -----------------------------------------------
    // raw 0..1023 -> shape 0..2 (quantized: 0..511 = SIN, 512..767 = S,
    // 768..1023 = WALK). Written to lfo_sim_.shape (the WTE morph LFO).
    case WAVE_LFO_SHAPE:
      lfo_sim_.shape = (value < 512) ? 0 : (value < 768) ? 1 : 2;
      if (lfo_sim_.shape == ShapeLfoSim::kShapeWalk)
        lfo_sim_.invalidate(); // enter WALK: build the first scene now
      break;
    case WAVE2_LFO_SHAPE:
      break; // stream-2 LFO removed (no-op, see above)

    // ---- WALK gap (stream 1) -------------------------------------------------
    // LOG scale: raw 0..1023 -> 1..300 s (3 decades).
    // raw 0    -> 1 s (dense)
    // raw 511  -> ~17 s
    // raw 1023 -> 300 s (sparse)
    // Written to lfo_sim_.gap_sec (the WTE morph LFO's WALK scene density).
    case WAVE_LFO_GAP:
      lfo_sim_.gap_sec = 1.f * fasterpowf(300.f, param_10bit_to_f32(value));
      if (lfo_sim_.shape == ShapeLfoSim::kShapeWalk)
        lfo_sim_.invalidate(); // new point density — audible immediately
      break;
    case WAVE2_LFO_GAP:
      break; // stream-2 LFO removed (no-op, see above)

    // ---- Wave-Mix LFO (WMIX_LFO_*) — REMOVED (no-op) -------------------------
    // The wave-mix LFO (the old third internal LFO, lfo_mix_) modulated the
    // stream 1 <-> stream 2 crossfade — gone with stream 2 (see the
    // WAVE2_LFO_RATE block above). No-ops kept for the enum/preset layout.
    case WMIX_LFO_RATE:
    case WMIX_LFO_DEPTH:
    case WMIX_LFO_SHAPE:
    case WMIX_LFO_GAP:
      break; // superseded by the WTE wavetable

    // NOTE: FOLD2 (second cascaded wavefolder) and DCK (sidechain-duck amount)
    // were removed from the editable parameter set. FOLD2 was judged
    // unnecessary; the sidechain duck is gone entirely with the SUB1/SUB2 sub
    // voices (the low end now lives inside the WTE wavetable, so there is no
    // low-end branch to duck the main tone against). The audio path no longer
    // references any fold2 / duck state.

    // ---- FM modulator C: phase-FM on the (A+B) sum --------------------------
    // C is an independent low-frequency oscillator that bends the sum's phase
    // (vibrato / motion colour). C is NOT heard by itself. At FM_CR = 0 (OFF)
    // the sum is unmodulated (regression-safe).
    case FM_CR:
      // LOG 0..1 Hz (raw 0 = OFF). 3 decades like the wave-LFO rate knobs:
      // raw 0 -> 0 Hz, else 10^(3*raw/1023 - 3) * kFmCRateMaxHz (0.001..1 Hz).
      params_.fm_c_rate = (value == 0)
          ? 0.f
          : (float)(fasterpowf(10.f, 3.f * param_10bit_to_f32(value) - 3.f) * kFmCRateMaxHz);
      break;
    case FM_CW:
      // Wave shape (FIVE shapes): 0..341 = SIN, 342..511 = TRI (triangle),
      // 512..682 = SAW, 683..853 = SQR, 854..1023 = NOISE. (NOISE is drawn
      // per-sample from osc_white() in the audio path; the display / randomize
      // use the same 5-band split. See getParameterStrValue.) TRI is the "soft"
      // shape: a triangle's harmonics fall off as 1/n^2 (vs 1/n for SIN / SAW),
      // so it draws the FEWEST upper sidebands of any periodic wave -- the
      // go-to choice for "FM complex but not screechy".
      {
        const uint8_t band = (value < 342) ? 0
            : (value < 512) ? 1
            : (value < 683) ? 2
            : (value < 854) ? 3
            : 4; // 854..1023 = NOISE
        params_.fm_c_wave = band;
      }
      break;
    case FM_CD:
      params_.fm_c_depth = param_10bit_to_f32(value); // raw 0..1 LINEAR depth
      break;

    // ---- FM modulator S (FM_STATIC / FMST): TEETH per carrier cycle --------
    // S is a carrier-locked FM oscillator: it runs at N teeth per (A+B) carrier
    // cycle. LOG kFmStaticMinTeeth..kFmStaticMaxTeeth (raw 0 = OFF, 0 teeth ->
    // the sum is unmodulated, bit-identical; raw 1023 -> kFmStaticMaxTeeth;
    // raw 1 -> kFmStaticMinTeeth = 0.05 teeth). The log scale (4 decades,
    // fasterpowf) gives FINE control below 1 tooth (sub-octave: the S modulator
    // runs slower than the carrier, fm/fc < 1) up to a dense comb at the top.
    // Teeth < 1 draw sidebands that dip below the carrier (deep/organ); 1 =
    // in-tune (fm/fc = 1). Written to params_.fm_static (teeth).
    case FM_STATIC:
      params_.fm_static = (value == 0)
          ? 0.f
          : kFmStaticMinTeeth *
                fasterpowf(kFmStaticMaxTeeth / kFmStaticMinTeeth, param_10bit_to_f32(value));
      break;

    // ---- FM modulator S amplitude (FM_SAMP / FMSA): tooth SIZE --------------
    // The peak swing of each tooth. LINEAR 0..100% (raw 0..1). The audio path
    // (osc_mix.h) computes the effective offset as
    // params_.fm_static_amp * kFmStaticMaxCycles * S_wave, so FMSA sets the
    // amplitude and FMST sets the tooth count. At 0 the offset is 0.
    case FM_SAMP:
      params_.fm_static_amp = param_10bit_to_f32(value); // raw 0..1 LINEAR
      break;

    // NOTE: the S (static teeth) tooth SHAPE is shared with the C modulator --
    // it re-uses params_.fm_c_wave (edit FM_CW / FM Mod Wave), so there is no
    // separate FM_SW case here.

    // ---- FM AMT (FM_AMT / FMAM): the GLOBAL FM MOD amount -------------------
    // The master multiplier (LINEAR 0..100%, raw 0..1) on BOTH the C (vibrato)
    // and S (static teeth) phase offsets. The audio path (osc_mix.h) multiplies
    // fm_c_offset and fm_s_offset by params_.fm_amt before adding them to the
    // A/B phasors. At 1.0 (100%, default) the offsets are unchanged
    // (bit-identical); at 0 (0%) the whole FM MOD block is OFF regardless of
    // FMCD/FMSA. It scales the deviation only, never the C/S rates or shapes.
    case FM_AMT:
      params_.fm_amt = param_10bit_to_f32(value); // raw 0..1 LINEAR
      break;

    // ---- FCUT2: cascaded 2nd-stage SVF dry/wet (12 -> 24 dB/oct) -----------
    // The knob maps 0..1023 -> 0..1 (linear) and is written straight to
    // params_.svf_wet2 (the 2nd-stage wet, consumed in osc_mix.h). The 2nd
    // stage ALREADY shares the 1st stage's cutoff + resonance (set by the FCUT
    // / RES cases above: svf2_.updateCutoff), so only the wet amount is new
    // here -- no K-rate recompute. At 0 the cascade is transparent (regression-
    // safe: a single 12 dB/oct SVF); at 1 it is a full 24 dB/oct LPF.
    case FCUT2:
      params_.svf_wet2 = param_10bit_to_f32(value); // raw 0..1 LINEAR
      break;

    // ---- Editor-wavetable voice (PMORPH..PLEV) — REMOVED (no-op) -------------
    // The old separate editor-wavetable VOICE (a parallel voice reading the
    // 16x256 table at (PMORPH, proc_ph) with its own level PLEV) is gone: the
    // A+B voices now read the WTE table DIRECTLY (see osc_mix.h "Wavetable
    // samples (WTE)"), so PMORPH (frame position) and PLEV (voice level) are
    // superseded -- the morph is the Wave LFO 1 and the level is MAIN. No-ops
    // kept for the enum/preset layout. The table SHAPES always came from the
    // /editor document (wteSetDocument, whole doc in one call -> K-rate
    // regen in blockPrepass), NOT from edit params.
    case PMORPH:
    case PLEV:
      break; // superseded: WTE table is the main voice (morph = Wave LFO 1, level = MAIN)

    default:
      break;
  }
}

