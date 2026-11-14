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
 *  File: modfx.h
 *
 *  DRONE-MOD (modfx) DSP: a 3-stage modulation stack + noise enrichment + tape
 *  wow/flutter for the DRONE path. Parameter model (header.c): TIME / DEPTH are
 *  the two FIXED knobs (always on the A/B encoders); the other 8 parameters
 *  (DRIFT / CMODE / CHORUS / FLANGER / NOISE / NRATE / ENSMBL / TAPWOB) live
 *  behind a PARAM/VALUE multiplexer exactly like drone-osc (slots 2/3 are the
 *  optional edit-menu duplicates of slots 0/1). The per-sample audio path is
 *  defined inline below:
 *    in (stereo) -> [NOISE branch: fx_white burst-gated (NRATE cadence),
 *                    DRIFT-AM, 2-pole SVF band-pass (TIME-swept)] ->
 *    CHORUS (Juno-60 BBD stereo chorus) -> FLANGER (thru-zero flanger) ->
 *    [TAPWOB branch: tape wow/flutter pitch-mod, dry/wet replace] ->
 *    makeup (0.9) -> out.
 *  The NOISE branch is an ADDITIVE "metallic rustle / dust-flash" generator:
 *  LCG white noise, flashed on an exponential cadence (NRATE), AM-modulated
 *  by the DRIFT walk (channel 3), band-passed by a hand-rolled 2-pole SVF
 *  whose cutoff sweeps with TIME. NOISE = 0 -> the branch is never computed
 *  (dry wire, regression-safe).
 *  DRIFT is a K-rate LCG random-walk; channel 3 drives the noise AM (its
 *  other three channels are allocated/zeroed but not yet wired -- see README).
 *  The noise source is the fx-module's platform white-noise API, fx_white()
 *  / fx_rand() (common/fx_api.h, Park-Miller-Carta) -- the FX-module twin of
 *  the OSC unit's osc_white()/osc_rand(): on the NTS-1 these come from the
 *  runtime, in the web chain they are PMC stubs (drone-chain/wasm.cc).
 *  See modfx_constants.h for the tuning and README.md for the full parameter /
 *  signal-path documentation.
 *
 */
#include "processor.h"
#include "unit_modfx.h" // include base definitions for modfx units
#include "modfx_constants.h" // tuning constants + libm-free helpers (modfx_consts::*)

// ---- Platform white-noise API (the FX-module twin of the OSC's osc_*) -------
// The FX runtime (common/fx_api.h) links the same Park-Miller-Carta sources
// into modfx units: fx_rand() -> raw uint32 state, fx_white() -> noise in
// [-1, 1]. On the NTS-1 these come from the runtime (fx_api.h declares them
// in C, hence the extern "C" below); in the web chain the stubs live in
// drone-chain/wasm.cc. The NOISE branch and the DRIFT walk consume them.
#ifdef __cplusplus
extern "C" {
#endif
uint32_t fx_rand(void);
float fx_white(void);
#ifdef __cplusplus
}
#endif

class Modfx : public Processor
{
public:
  uint32_t getBufferSize() const override final { return modfx_consts::kModSramBytes; } // 7 delay lines

  // ---- Header slot ids (header.c) --------------------------------------------
  // The unit has TWO layers of parameters (the same split as drone-osc):
  //   * HEADER SLOTS (header.c) -- what the runtime/UI sees. 4 slots total:
  //       slot 0 = TIME   (fixed A knob,  type none)    -> PARAM_TIME
  //       slot 1 = DEPTH  (fixed B knob,  type none)    -> PARAM_DEPTH
  //       slot 2 = PARAM  (optional,      type strings) -> PARAM_SEL (selector)
  //       slot 3 = VALUE  (optional,      type strings) -> PARAM_VAL (value)
  //     Slots 2/3 are the optional edit-menu duplicates of the OSC's 4-slot
  //     multiplexer: PARAM picks WHICH edit parameter, VALUE edits its raw
  //     10-bit value. unit.cc maps header slot 2->PARAM_SEL, 3->PARAM_VAL.
  //   * EDIT PARAMETERS (the 8 values behind the multiplexer) -- the actual DSP
  //     parameters. select_ 0..7 -> DRIFT..TAPWOB (kEditParamFirst..TAPWOB).
  enum
  {
    PARAM_TIME = 0U,  // header slot 0 (fixed A knob): LFO tempo (0..1)
    PARAM_DEPTH,      // header slot 1 (fixed B knob): modulation depth (0..1)
    PARAM_SEL,        // header slot 2 (optional): MOD multiplexer selector 0..4
    PARAM_VAL,        // header slot 3 (optional): raw value of select_ 0..1023
    PARAM_TSEL,       // header slot 4 (optional): TAPE multiplexer selector 0..4
    PARAM_TVAL,       // header slot 5 (optional): raw value of tsel_ 0..1023
    PARAM_COUNT       // = 6 header slots
  };

  // ---- MOD-group edit-parameter ids (behind the PARAM/VALUE multiplexer) ------
  // The 5 MODULATION parameters the PARAM knob (slot 2) selects. The enum VALUES
  // are the select_ indices (0..4) in display/edit order:
  //   0: DRIFT    - shared self-motion ceiling (random-walk on stage levels)
  //   1: CMODE    - chorus mode (I / I+II / II)
  //   2: CHORUS   - chorus dry/wet level
  //   3: FLANGER  - flanger level
  //   4: ENSMBL   - string-synth ensemble level
  enum
  {
    DRIFT = 0U,
    CMODE,
    CHORUS,
    FLANGER,
    ENSMBL,
    kModEditCount = 5U,        // number of MOD-group edit parameters
  };

  // ---- TAPE-group edit-parameter ids (behind the TPARAM/TVALUE multiplexer) ---
  // The 5 TAPE (KoS-derived) parameters the TPARAM knob (slot 4) selects. The
  // enum VALUES are the tsel_ indices (0..4) in display/edit order:
  //   0: NOISE   - granular rustle level (KoS HurricaneSandy "age")
  //   1: NRATE   - noise burst cadence
  //   2: TAPWOB  - tape wow/flutter level (the only pitch-mod stage)
  //   3: HISS    - tape hiss level (KoS Hiss, procedural)
  //   4: WARP    - magnetic-instability level (KoS "Shame")
  enum
  {
    T_NOISE = 0U,
    T_NRATE,
    T_TAPWOB,
    T_HISS,
    T_WARP,
    kTapeEditCount = 5U,       // number of TAPE-group edit parameters
  };

  // Note: Make sure that default param values correspond to declarations in header.c
  // (header.c fixed: TIME 512, DEPTH 511. MOD multiplexer edit_value_ defaults:
  //  DRIFT 0, CMODE 0, CHORUS 0, FLANGER 0, ENSMBL 0. TAPE multiplexer
  //  tedit_value_ defaults: NOISE 0, NRATE 0, TAPWOB 0, HISS 0, WARP 0).
  struct Params
  {
    float time;   // 0..1 (LFO tempo -> chorus LFO rate + noise BP cutoff)
    float depth;  // 0..1 (per-stage modulation depth -> chorus LFO spread)
    float drift;  // 0..1 (shared self-motion ceiling, 0 = static)
    float cmode;  // 0..1 (chorus mode, 3 equal bands: I / I+II / II)
    float chorus;   // 0..1 (chorus dry/wet: 0=dry, 1=full)
    float flanger;   // 0..1 (flanger wet)
    float noise;    // 0..1 (noise branch level: 0 = off)
    float nrate;    // 0..1 (noise burst cadence: 0.15..8 fl/s, exp)
    float ensemble; // 0..1 (ensemble level: 0 = off)
    float tapwob;   // 0..1 (tape wow/flutter level: 0 = off, dry wire)
    float hiss;     // 0..1 (tape hiss level: 0 = off, dry wire)
    float warp;     // 0..1 (magnetic-instability / "Shame" level: 0 = off)

    void reset()
    {
      time = 0.f;     // 0/1023 (slowest LFO: flanger 0.02 Hz = 50 s, chorus 60 s)
      depth = 0.f;    // 0/1023 (no modulation by default, regression-safe)
      drift = 0.f;    // 0 (static, regression-safe)
      cmode = 0.f;     // 0/1023 (band 0 = I, main voice only)
      chorus = 0.f;     // 0/1023 (dry by default; user enables chorus)
      flanger = 0.f;  // 0 (off)
      noise = 0.f;    // 0 (off: the branch is never computed, regression-safe)
      nrate = 0.f;    // 0/1023 (rarest cadence; only audible when NOISE > 0)
      ensemble = 0.f; // 0 (off: the branch is never computed, regression-safe)
      tapwob = 0.f;   // 0 (off: the branch is never computed, regression-safe)
      hiss = 0.f;     // 0 (off: the branch is never computed, regression-safe)
      warp = 0.f;     // 0 (off: the branch is never computed, regression-safe)
    }

    Params() { reset(); }
  };

  // ---- Parameter entry point (header slot ids only) ---------------------------
  // unit.cc maps the 6 header slots onto these 6 ids before forwarding:
  //   slot 0 (TIME,  fixed)    -> PARAM_TIME
  //   slot 1 (DEPTH, fixed)    -> PARAM_DEPTH
  //   slot 2 (PARAM, optional) -> PARAM_SEL   (MOD multiplexer selector 0..4)
  //   slot 3 (VALUE, optional) -> PARAM_VAL   (raw 0..1023 of select_)
  //   slot 4 (PARAM, optional) -> PARAM_TSEL  (TAPE multiplexer selector 0..4)
  //   slot 5 (VALUE, optional) -> PARAM_TVAL  (raw 0..1023 of tsel_)
  // So setParameter ONLY ever sees PARAM_TIME / PARAM_DEPTH / PARAM_SEL /
  // PARAM_VAL / PARAM_TSEL / PARAM_TVAL -- never the edit-parameter ids directly.
  inline void setParameter(uint8_t index, int32_t value) override final
  {
    switch (index)
    {
    case PARAM_TIME:
      params_.time = param_10bit_to_f32(value);
      break;
    case PARAM_DEPTH:
      params_.depth = param_10bit_to_f32(value);
      break;
    case PARAM_SEL:
      // MOD-group selector: which of the 5 MOD edit params (0..4) is being
      // edited. Clipped to kModEditCount-1 so a stale/larger id can't index
      // past edit_value_.
      select_ = (value < 0 || value >= (int32_t)kModEditCount) ? 0U : (uint8_t)value;
      break;
    case PARAM_VAL:
      // Raw 10-bit value of the selected MOD edit param. Clip to 0..1023 then
      // apply to the DSP.
      if (value < 0) value = 0; else if (value > 1023) value = 1023;
      edit_value_[select_] = value;
      apply_edit_param(select_, value);
      break;
    case PARAM_TSEL:
      // TAPE-group selector: which of the 5 TAPE edit params (0..4) is being
      // edited. Clipped to kTapeEditCount-1.
      tsel_ = (value < 0 || value >= (int32_t)kTapeEditCount) ? 0U : (uint8_t)value;
      break;
    case PARAM_TVAL:
      // Raw 10-bit value of the selected TAPE edit param. Clip to 0..1023 then
      // apply to the DSP.
      if (value < 0) value = 0; else if (value > 1023) value = 1023;
      tedit_value_[tsel_] = value;
      apply_tape_param(tsel_, value);
      break;
    default:
      break;
    }
  }

  // Apply one MOD edit parameter's raw 0..1023 value to the live DSP params.
  // `idx` is a select_ index (0..kModEditCount-1); `raw` the 10-bit value.
  // Called from PARAM_VAL writes and (once) at init for every default.
  inline void apply_edit_param(uint8_t idx, int32_t raw)
  {
    const float v = param_10bit_to_f32(raw);
    switch (idx)
    {
    case DRIFT:   params_.drift    = v; break;
    case CMODE:   params_.cmode    = v; break;
    case CHORUS:  params_.chorus   = v; break;
    case FLANGER: params_.flanger  = v; break;
    case ENSMBL:  params_.ensemble = v; break;
    default: break;
    }
  }

  // Apply one TAPE edit parameter's raw 0..1023 value to the live DSP params.
  // `idx` is a tsel_ index (0..kTapeEditCount-1); `raw` the 10-bit value.
  inline void apply_tape_param(uint8_t idx, int32_t raw)
  {
    const float v = param_10bit_to_f32(raw);
    switch (idx)
    {
    case T_NOISE:  params_.noise    = v; break;
    case T_NRATE:  params_.nrate    = v; break;
    case T_TAPWOB: params_.tapwob   = v; break;
    case T_HISS:   params_.hiss     = v; break;
    case T_WARP:   params_.warp     = v; break;
    default: break;
    }
  }

  // ---- Multiplexer state accessors (web-sim + UI init) ------------------------
  inline uint8_t getSelect() const { return select_; }
  inline int32_t getEditParamValue(uint8_t idx) const
  {
    return (idx < kModEditCount) ? edit_value_[idx] : 0;
  }
  // Raw value of the currently selected MOD param (PARAM_VAL's live state).
  inline int32_t getValue() const { return edit_value_[select_]; }
  // TAPE-group multiplexer accessors.
  inline uint8_t getTapeSelect() const { return tsel_; }
  inline int32_t getTapeEditParamValue(uint8_t idx) const
  {
    return (idx < kTapeEditCount) ? tedit_value_[idx] : 0;
  }
  // Raw value of the currently selected TAPE param (PARAM_TVAL's live state).
  inline int32_t getTapeValue() const { return tedit_value_[tsel_]; }

  // ---- Display (header slot id -> string) -------------------------------------
  // The PARAM slots (0 and 2) show the SELECTED edit parameter's NAME; the
  // VALUE slots (1 and 3) show that parameter's FORMATTED value (CMODE as
  // I / I+II / II, everything else as 0..100%). TIME / DEPTH (slots 0/1 are
  // type_none, NOT strings) return nullptr -- the platform / web sim format
  // them as percentages natively. This is the same split as drone-osc's
  // getParameterStrValue (see osc_param_display.h).
  inline const char *getParameterStrValue(uint8_t id, int32_t value) const override final
  {
    static const char *const mod_names[kModEditCount] = {
        "DRIFT", "CMODE", "CHOR", "FLNGR", "ENSMBL"};
    static const char *const tape_names[kTapeEditCount] = {
        "NOISE", "NRATE", "TAPWOB", "HISS", "WARP"};
    // Header slots: 0=TIME, 1=DEPTH, 2=PARAM(PARAM_SEL), 3=VALUE(PARAM_VAL),
    // 4=TPARAM(PARAM_TSEL), 5=TVALUE(PARAM_TVAL).
    if (id == PARAM_SEL)   // slot 2: MOD group NAME
    {
      const uint8_t idx = (value >= 0 && value < (int32_t)kModEditCount) ? (uint8_t)value : select_;
      return mod_names[idx];
    }
    if (id == PARAM_TSEL)  // slot 4: TAPE group NAME
    {
      const uint8_t idx = (value >= 0 && value < (int32_t)kTapeEditCount) ? (uint8_t)value : tsel_;
      return tape_names[idx];
    }
    if (id == PARAM_VAL || id == PARAM_TVAL)
    {
      // Format the currently selected parameter's cached raw value.
      const bool is_tape = (id == PARAM_TVAL);
      const uint8_t idx = is_tape
          ? ((tsel_ < kTapeEditCount) ? tsel_ : 0U)
          : ((select_ < kModEditCount) ? select_ : 0U);
      const int32_t raw = is_tape ? tedit_value_[idx] : edit_value_[idx];
      if (!is_tape && idx == CMODE)
      {
        // 3 equal bands (same threshold as the audio path): I / I+II / II.
        const float c = param_10bit_to_f32(raw);
        if (c < 1.f / 3.f) return "I";
        if (c < 2.f / 3.f) return "I+II";
        return "II";
      }
      // Everything else: 0..100%.
      static char buf[8];
      const int pct = (int)((raw * 100) / 1023 + 0.5f);
      // Manual 3-digit + '%' format (no libm / no snprintf needed).
      char *q = buf + sizeof(buf) - 1; *q-- = 0; *q-- = '%';
      const int d100 = pct / 100, d10 = (pct / 10) % 10, d1 = pct % 10;
      *q-- = (char)('0' + d1);
      *q-- = (char)('0' + d10);
      if (d100 > 0) *q-- = (char)('0' + d100);
      return (const char *)(q + 1);
    }
    // TIME / DEPTH: type_none, the platform / sim format as %.
    return nullptr;
  }

  // life-cycle methods
  void init(float *allocated_buffer) override final
  {
    // Slice the single SDRAM allocation (getBufferSize() floats) into the six
    // interleaved delay lines (CHORUS L/R, FLANGER L/R, TAPE L/R). On the NTS-1 this
    // comes from unit.cc's sdram_alloc; in the web chain, Chain::init slices
    // the chain's single allocation and passes the MOD slice here.
    buffer_ = allocated_buffer;
    float *p = allocated_buffer;
    // CHORUS: two mono delay lines (Juno main + sub), each kChorusLineLenSmp floats.
    chorus_main_delay_ = p; p += modfx_consts::kChorusLineLenSmp;
    chorus_sub_delay_  = p; p += modfx_consts::kChorusLineLenSmp;
    chorus_main_w_ = 0U;
    chorus_sub_w_ = 0U;
    // FLANGER: kFlangerLineCount MONO delay lines (bucket-brigade), each kFlangerLineLenSmp.
    flanger_buf_  = p;  p += modfx_consts::kFlangerLineCount * modfx_consts::kFlangerLineLenSmp;
    for (uint32_t i = 0; i < modfx_consts::kFlangerLineCount; ++i) flanger_w_[i] = 0U;
    // ENSEMBLE: one SHARED MONO delay line, kEnsembleLineLenSmp floats.
    ensemble_delay_ = p; p += modfx_consts::kEnsembleLineLenSmp;
    ensemble_w_ = 0U;
    ens_phase_[0] = 0.f; ens_phase_[1] = 1.f/3.f; ens_phase_[2] = 2.f/3.f;
    ens_lfo_freq_ = modfx_consts::kEnsembleLFOMin; // seed at the slowest rate
    ens_spread_ = 0.5f * modfx_consts::kEnsembleMaxSpreadSmp; // seed ~DEPTH=0.5 (DEPTH^2=0.25 -> but use half for a sane mid default)
    ens_lpf_l_ = 0.f; ens_lpf_r_ = 0.f;
    // TAPWOB: one SHARED MONO delay line, kTapeWobLineLenSmp floats.
    tapwob_delay_ = p; p += modfx_consts::kTapeWobLineLenSmp;
    tapwob_w_ = 0U;
    tapwob_wow_phase_ = 0.f; tapwob_flut_phase_ = 0.f;
    tapwob_wow_freq_ = modfx_consts::kTapeWobWowMinHz; // seed at TIME=0 rate
    tapwob_flut_freq_ = modfx_consts::kTapeWobFlutMinHz;
    tapwob_wow_cents_ = 0.f; tapwob_flut_cents_ = 0.f; // set on first K-rate pass
    tapwob_lpf_alpha_ = 0.1f; // mid-range default (set properly on first K-rate pass)
    tapwob_lpf_l_ = 0.f; tapwob_lpf_r_ = 0.f;
    // WARP: one SHARED MONO delay line, kWarpLineLenSmp floats (magnetic instability).
    warp_delay_ = p; p += modfx_consts::kWarpLineLenSmp;
    warp_w_ = 0U;
    warp_phase_ = 0.f; warp_rate_ = 0.f; warp_rate_fluct_ = 0.f;
    warp_depth_ = 0.f; warp_lfo_freq_ = 0.f;
    // PINK-noise grain source ring, kPinkRingLenSmp floats (NOISE v2).
    pink_ring_ = p; p += modfx_consts::kPinkRingLenSmp;
    pink_w_ = 0U;
    pink_lpf1_ = 0.f; pink_lpf2_ = 0.f; pink_lpf3_ = 0.f;


    // Zero-clear the REST of the SDRAM allocation (FLANGER + ENSEMBLE + TAPWOB +
    // WARP + PINK lines, i.e. everything after the two CHORUS lines). This is
    // what makes WARP "regression-safe": the 4096-smp line must be zeroed here,
    // otherwise the first reads (before the line is filled by `in_l`) return
    // uninitialized SDRAM and the WARP dry/wet blend is garbage.
    if (buffer_)
    {
      // All sizes are in floats; getBufferSize() is in bytes, so divide.
      const uint32_t chorus_samples = 2U * modfx_consts::kChorusLineLenSmp;
      const uint32_t total_samples  = getBufferSize() / sizeof(float);
      for (uint32_t i = chorus_samples; i < total_samples; ++i) buffer_[i] = 0.f;
    }

    // Line heads + FLANGER filter state.
    for (uint32_t i = 0; i < modfx_consts::kFlangerLineCount; ++i) flanger_w_[i] = 0U;
    flanger_hpf_x_ = 0.f; flanger_hpf_y_ = 0.f; flanger_lpf_l_ = 0.f; flanger_lpf_r_ = 0.f;


    params_.reset();

    // Multiplexer state: select the FIRST edit param of each group and load every
    // default into the live DSP (so the audio path starts with the header.c
    // defaults, exactly as if each knob had been written once). The defaults
    // are all 0 (CMODE=0 -> band 0 = I, CHORUS=0 -> dry, NRATE=0 -> rarest):
    // this must stay in sync with the in-class initializers of edit_value_ /
    // tedit_value_ (see the "Multiplexer state" section below) and with
    // Params::reset() above.
    select_ = 0U;
    for (uint8_t i = 0U; i < kModEditCount; ++i)
    {
      edit_value_[i] = 0;
      apply_edit_param(i, edit_value_[i]);
    }
    tsel_ = 0U;
    for (uint8_t i = 0U; i < kTapeEditCount; ++i)
    {
      tedit_value_[i] = 0;
      apply_tape_param(i, tedit_value_[i]);
    }

    flanger_lfo_phase_ = 0.f;

    // CHORUS Juno-chorus LFOs. q31 phase accumulators; the increment (w_) is
    // recomputed every block in K-rate from TIME (LFO rate sweep). Seed here
    // at the base Juno rates so the very first block before the first
    // K-rate pass has a sane rate.
    chorus_lfo1_phase_ = 0;
    chorus_lfo2_phase_ = 0;
    chorus_lfo1_w_ = (int32_t)(modfx_consts::kChorusLFOMin * 2147483648.0f / modfx_consts::kSr);
    chorus_lfo2_w_ = (int32_t)(modfx_consts::kChorusLFOMin * modfx_consts::kChorusLfoDetune * 2147483648.0f / modfx_consts::kSr);

    // CHORUS LPFs (1st-order, Juno RC anti-alias). Init alpha at mid-range.
    chorus_main_post_lpf_l_ = 0.f;
    chorus_main_post_lpf_r_ = 0.f;
    chorus_sub_post_lpf_l_ = 0.f;
    chorus_sub_post_lpf_r_ = 0.f;
    chorus_lpf_alpha_ = 0.5f; // mid-range default

    // DRIFT walk state (all off by default). Channel 3 (kDriftCount-1) is the
    // NOISE-branch AM modulator: it starts at 0 (static AM = 0.5) and wanders
    // [-1, +1] only when DRIFT > 0.
    for (int i = 0; i < kDriftCount; ++i)
    {
      drift_target_[i] = 0.f;
      drift_cur_[i] = 0.f;
      drift_tau_[i] = 1.f;
      drift_next_gap_[i] = 1U;
    }

    // TAPWOB state (off by default: TAPWOB = 0 -> never computed).
    tapwob_w_ = 0U;
    tapwob_wow_phase_ = 0.f;
    tapwob_flut_phase_ = 0.f;
    tapwob_wow_cents_ = 0.f;
    tapwob_flut_cents_ = 0.f;
    tapwob_lpf_l_ = 0.f;
    tapwob_lpf_r_ = 0.f;

    // NOISE branch state (off by default: NOISE = 0 -> never computed).
    noise_envelope_ = 0.f;
    noise_phase_ = 0U;
    noise_on_timer_ = 0U;
    noise_gap_timer_ = 0U;
    noise_burst_mean_smp_ = 32000U; // mid NRATE (~1 fl/s) until the first K-rate pass
    noise_burst_level_ = 1.f;
    noise_svf_vel_ = 0.f;
    noise_svf_pos_ = 0.f;
    noise_svf_cc_ = 0.f;
    noise_svf_ae_ = 1.f;
    noise_svf_cc_w_ = 0.f;
    noise_am_alpha_ = 0.f;
    // NOISE v2 (KoS HurricaneSandy) state: granulator voices, pink source, AM-dips,
    // burst scrape, room tone. Off by default (NOISE = 0 -> never computed).
    for (uint32_t g = 0U; g < modfx_consts::kGrainVoices; ++g)
    {
      grain_state_[g] = 0U;
      grain_counter_[g] = 0U;
      grain_pointer_[g] = 0.f;
      grain_escaler_[g] = 0.f;
      grain_erate_[g] = 0.f;
      grain_start_[g] = 0U;
    }
    pink_lpf1_ = 0.f; pink_lpf2_ = 0.f; pink_lpf3_ = 0.f;
    grain_hpf_x_ = 0.f; grain_hpf_y_ = 0.f;
    grain_lpf_l_ = 0.f; grain_lpf_r_ = 0.f;
    dips_x_[0] = 0.f; dips_x_[1] = 1.f; // flat 1.0 until the first re-seed
    dips_incr_ = 0U;
    burst_timer_ = 0U;
    burst_env_ = 0.f;
    roomtone_lpf_ = 0.f;
    hiss_hpf_y_ = 0.f;
    // WARP (magnetic instability) state: off by default (WARP = 0 -> never computed).
    warp_rate_ = 0.f; warp_rate_fluct_ = 0.f; warp_depth_ = 0.f; warp_lfo_freq_ = 0.f;
  }

  void teardown() override final { buffer_ = nullptr; }

  // audio processing callbacks (per-sample path lives inline below in this class)
  void process(const float *__restrict in, float *__restrict out, uint32_t frames) override final
  {
    modfx_process_impl(in, out, frames);
  }

  // Diagnostic: public peak variables (for debugging gain issues).
  volatile float mod_input_peak_ = 0.f;
  volatile float mod_output_peak_ = 0.f;

private:
  // ---- DRIFT walk (K-rate, LCG-driven, see modfx_process_impl) ---------------
  // One walk channel per level stage (CHORUS/FLANGER, channels 0/1 -- allocated
  // but not yet wired into the wet amounts) plus channel 3 (kDriftNoise = 2),
  // the slow AM modulator of the NOISE branch: AM = 0.5 + 0.5 * drift_cur_[2]
  // (0.5 const at DRIFT = 0; wanders 0..1 with the walk when DRIFT > 0).
  static constexpr int kDriftCount = 4;
  static constexpr int kDriftNoise = 3; // noise-branch AM channel index
  float drift_target_[kDriftCount];
  float drift_cur_[kDriftCount];
  float drift_tau_[kDriftCount];
  uint32_t drift_next_gap_[kDriftCount];

  // ---- NOISE branch state (granular noise-sweep, see modfx_process_impl) -----
  // White noise (fx_white) is gated by a PHASE machine (ON = flash, GAP = lull)
  // with a one-pole envelope that hard-switches target by phase (so the envelope
  // reliably decays to zero in the gap -- the old single-timer + 0.999-threshold
  // bug left the gate almost always open, which is why NRATE was inaudible).
  // AM-modulated by the DRIFT walk, band-passed by a 2-pole SVF (cutoff K-rate
  // from TIME + per-burst tone wander, Q fixed) and summed ADDITIVELY into the
  // input. For "maximum diversity" each burst ALSO randomizes its on-time,
  // level (per-burst gain) and tone (per-burst BP center), all drawn from
  // fx_rand() once per burst. L/R run independent noise draws (stereo
  // decorrelation, 0 SDRAM: everything here is a handful of floats in the
  // class). NOISE = 0 -> none of this state is touched (branch skipped,
  // regression-safe).
  float noise_envelope_ = 0.f;      // burst envelope 0..1 (one-pole, target by phase)
  uint32_t noise_phase_ = 0U;       // 0 = GAP (lull), 1 = ON (flash)
  uint32_t noise_on_timer_ = 0U;    // samples left in the current flash (ON)
  uint32_t noise_gap_timer_ = 0U;   // samples left in the current lull (GAP)
  uint32_t noise_burst_mean_smp_ = 32000U; // K-rate: exp(-) gap mean from NRATE
  float noise_burst_level_ = 1.f;   // per-burst gain (drawn at burst start)
  float noise_svf_vel_ = 0.f;       // SVF velocity state (band-pass output)
  float noise_svf_pos_ = 0.f;       // SVF position state (low-pass)
  float noise_svf_cc_ = 0.f;        // K-rate: SVF cutoff coeff (2*sin(w0), clamped)
  float noise_svf_ae_ = 1.f;        // K-rate: SVF damping coeff (1 - cc/2Q, clamped)
  float noise_svf_cc_w_ = 0.f;      // per-burst: cc * 2^(+/-wander*dev), clamped
  float noise_am_alpha_ = 0.f;      // per-sample: AM = 0.5 + 0.5*drift_cur_[kDriftNoise]

  // ---- LFO phase accumulators (CHORUS / FLANGER) ---------------------------------
  // CHORUS uses two q31 triangle LFOs: main at the base Juno 0.513 Hz, sub
  // detuned x1.0618; both swept live by TIME (the increments w_ are recomputed
  // K-rate). FLANGER uses a hand-rolled float phase accumulator.
  // CHORUS LFOs are hand-rolled q31 phase accumulators (same as dsp::SimpleLFO
  // but without the CMSIS/arm_math.h dependency needed for the web sim).
  int32_t chorus_lfo1_phase_ = 0;  // Juno LFO 1 phase (q31)
  int32_t chorus_lfo1_w_ = 0;      // Juno LFO 1 increment (q31, set K-rate from TIME)
  int32_t chorus_lfo2_phase_ = 0;  // Juno LFO 2 phase (q31)
  int32_t chorus_lfo2_w_ = 0;      // Juno LFO 2 increment (q31, set K-rate from TIME)
  float flanger_lfo_phase_;

  // ---- ENSEMBLE state (string-synth ensemble, RS-202 style) ------------------
  // One SHARED MONO delay line; three read taps at LFO phases 0 / 1/3 / 2/3 of
  // a cycle (120 deg apart). The LFO is a binary-sine (base + 10x harmonic),
  // hand-rolled per tap (no libm, no table). The line head advances once per
  // sample (mono write: the mono input is written to the shared line). R reads
  // the same taps shifted by kEnsemblePhase90 (367 smp) for the stereo spread.
  float *ensemble_delay_ = nullptr; // mono line, kEnsembleLineLenSmp floats
  uint32_t ensemble_w_ = 0U;        // line write head
  float ens_phase_[3] = {0.f, 1.f/3.f, 2.f/3.f}; // tap phases (cycles), 120 deg apart
  float ens_lfo_freq_ = 0.f;        // K-rate: base LFO rate from TIME (Hz)
  float ens_spread_ = 0.f;          // K-rate: LFO delay spread (samples), DEPTH^2-driven
  float ens_lpf_l_ = 0.f;           // 1st-order LPF state, L channel
  float ens_lpf_r_ = 0.f;           // 1st-order LPF state, R channel

  // ---- TAPWOB state (tape wow/flutter pitch-mod) ------------------------------
  // One SHARED MONO delay line (kTapeWobLineLenSmp). The mono input is written
  // once per sample (line head advances once); L/R each read a tap offset by
  // (kTapeWobCenterSmp + wow + flutter) samples, where wow/flutter are TWO
  // independent LFOs (cents -> sample offset). A 1st-order LPF per channel
  // dulls the highs as TAPWOB deepens. Dry/wet replace (NOT additive).
  float *tapwob_delay_ = nullptr;   // mono line, kTapeWobLineLenSmp floats
  uint32_t tapwob_w_ = 0U;          // line write head
  float tapwob_wow_phase_ = 0.f;    // slow wow LFO phase (cycles)
  float tapwob_flut_phase_ = 0.f;   // fast flutter LFO phase (cycles)
  float tapwob_wow_freq_ = 0.f;     // K-rate: wow rate from TIME (Hz)
  float tapwob_flut_freq_ = 0.f;    // K-rate: flutter rate from TIME (Hz)
  float tapwob_wow_cents_ = 0.f;    // K-rate: wow depth from DEPTH^3 (cents peak)
  float tapwob_flut_cents_ = 0.f;   // K-rate: flutter depth from DEPTH^2 (cents peak)
  float tapwob_lpf_alpha_ = 0.f;    // K-rate: LPF alpha from TAPWOB cutoff
  float tapwob_lpf_l_ = 0.f;        // 1st-order LPF state, L channel
  float tapwob_lpf_r_ = 0.f;        // 1st-order LPF state, R channel

  // ---- WARP state (magnetic instability, KoS "Shame") --------------------------
  // One SHARED MONO delay line (kWarpLineLenSmp). The mono input is written once
  // per sample; the READ TAP is (center + cosine-LFO * depth). The cosine LFO
  // rate is set K-rate from WARP; after each full cycle the rate gets a random
  // kick (warp_rate_fluct_) so the wobble never repeats. L/R read the SAME tap
  // (mono instability) -- stereo image is preserved by the other stages.
  float *warp_delay_ = nullptr;     // mono line, kWarpLineLenSmp floats
  uint32_t warp_w_ = 0U;            // line write head
  float warp_phase_ = 0.f;          // cosine LFO phase (cycles)
  float warp_rate_ = 0.f;           // K-rate: base LFO rate from WARP (cycles/sample)
  float warp_rate_fluct_ = 0.f;     // random rate kick (re-drawn each LFO cycle)
  float warp_depth_ = 0.f;          // K-rate: LFO depth (samples) from WARP
  float warp_lfo_freq_ = 0.f;       // K-rate: LFO rate (Hz) from WARP (diagnostic)

  // ---- HISS state (KoS Hiss, procedural white->HPF) ---------------------------
  float hiss_hpf_y_ = 0.f;          // 1st-order HPF state (white -> hiss color)

  // ---- NOISE v2 state (KoS HurricaneSandy granular rustle) ---------------------
  // PINK source: 3 one-pole LPFs (Voss) over fx_white -> pink_ring_ (mono ring).
  float *pink_ring_ = nullptr;      // pink-noise grain source ring, kPinkRingLenSmp floats
  float pink_lpf1_ = 0.f, pink_lpf2_ = 0.f, pink_lpf3_ = 0.f;
  uint32_t pink_w_ = 0U;            // pink ring write head
  // GRANULATOR: kGrainVoices linear-env grains reading the pink ring (random
  // pointer/offset, the classic MacPod scheme). State per voice:
  //   state: 0 = GAP (waiting), 1 = FADEIN, 2 = SUSTAIN, 3 = FADEOUT.
  uint8_t  grain_state_[modfx_consts::kGrainVoices] = {0};
  uint32_t grain_counter_[modfx_consts::kGrainVoices] = {0}; // samples left in state
  float    grain_pointer_[modfx_consts::kGrainVoices] = {0}; // pink-ring read pos (float)
  float    grain_escaler_[modfx_consts::kGrainVoices] = {0}; // env value (0..1)
  float    grain_erate_[modfx_consts::kGrainVoices] = {0};   // env slope
  uint32_t grain_start_[modfx_consts::kGrainVoices] = {0};   // grain start pointer
  // Grain band-shape: HPF + LPF (KoS 50 Hz / 2 kHz) per channel.
  float grain_hpf_x_ = 0.f, grain_hpf_y_ = 0.f;   // shared HPF (mono grain sum)
  float grain_lpf_l_ = 0.f, grain_lpf_r_ = 0.f;   // per-channel LPF
  // AM "dips": 2 cached envelope points (prev/next) + the current x position.
  // Re-seeded every kDipsDomainSmp samples (fx_rand). dips_x_/dips_y_ hold the
  // two bracketing points; dips_incr_ is the domain counter.
  float dips_x_[2] = {0.f, 1.f};
  float dips_y_[2] = {1.f, 1.f};
  float dips_pts_x_[modfx_consts::kDipsCount] = {0.f};
  float dips_pts_y_[modfx_consts::kDipsCount] = {1.f};
  uint32_t dips_incr_ = 0U;
  // Noise-burst "scrape": a fast attack/release white-noise flash that DUCKS the
  // dry signal (KoS sigEnv). Cadence from NRATE (kNoiseBurstMean*). burst_timer_
  // counts the burst window; burst_env_ is the 0..1 scrape envelope.
  uint32_t burst_timer_ = 0U;
  float burst_env_ = 0.f;
  // Low-freq room tone (KoS lowFreqGrainNoise): pink -> 1-pole LPF, quiet.
  float roomtone_lpf_ = 0.f;
  uint32_t grain_suslen_[modfx_consts::kGrainVoices] = {0}; // per-voice sustain length
  uint32_t dips_cur_i_ = 0U;                                // dips: current bracket index

  // ---- Multiplexer state (TWO PARAM/VALUE pairs, like drone-osc x2) -------------
  // MOD group: select_ = which of the 5 MOD edit params (0..kModEditCount-1) is
  // selected by the PARAM knob (slot 2). edit_value_[i] = raw 10-bit cache of
  // MOD param i; the VALUE knob (slot 3) edits edit_value_[select_].
  // Defaults mirror header.c (DRIFT 0, CMODE 0, CHORUS 0, FLANGER 0, ENSMBL 0).
  uint8_t select_ = 0U;
  int32_t edit_value_[kModEditCount] = {0, 0, 0, 0, 0};
  // TAPE group: tsel_ = which of the 5 TAPE edit params (0..kTapeEditCount-1) is
  // selected by the TPARAM knob (slot 4). tedit_value_[i] = raw 10-bit cache;
  // the TVALUE knob (slot 5) edits tedit_value_[tsel_].
  // Defaults mirror header.c (NOISE 0, NRATE 0, TAPWOB 0, HISS 0, WARP 0).
  uint8_t tsel_ = 0U;
  int32_t tedit_value_[kTapeEditCount] = {0, 0, 0, 0, 0};

  // ---- Filter / feedback states ----------------------------------------------
  // CHORUS Juno-chorus LPFs: hand-rolled 1st-order LPF (one-pole) states,
  // per-channel, per-voice. Same math as dsp::BiQuad::setFOLP + process_fo
  // but without the CMSIS dependency. State: y = ff0*x + z; z = ff1*x - fb1*y.
  // For setFOLP(k): ff0=ff1=k/(k+1), fb1=(k-1)/(k+1).
  // Simplified: for a 1st-order LPF with cutoff k=tan(pi*wc), the recurrence
  // is: y[n] = y[n-1] + alpha*(x[n] - y[n-1]) where alpha = (k-1)/(k+1) + k/(k+1)
  // Actually simplest: use the standard one-pole: y += alpha*(x-y), alpha from k.
  // We store the LPF state (z1) per channel per voice.
  float chorus_main_post_lpf_l_ = 0.f;  // main voice post-LPF state, L
  float chorus_main_post_lpf_r_ = 0.f;  // main voice post-LPF state, R
  float chorus_sub_post_lpf_l_ = 0.f;   // sub voice post-LPF state, L
  float chorus_sub_post_lpf_r_ = 0.f;   // sub voice post-LPF state, R
  // K-rate LPF alpha (computed per block from the TIME knob).
  float chorus_lpf_alpha_ = 0.f;        // 1st-order LPF alpha (0..1)

  // ---- Real delay lines (SDRAM, see getBufferSize()/init) --------------------
  // CHORUS: two mono delay lines (Juno main + sub voices, 512 smp each).
  // Hand-rolled (raw float* + write_idx) to avoid the CMSIS dependency.
  float *chorus_main_delay_ = nullptr;  // Juno main voice (mono, 512 smp)
  float *chorus_sub_delay_ = nullptr;   // Juno sub voice (mono, 512 smp)
  uint32_t chorus_main_w_ = 0U;         // main delay write head
  uint32_t chorus_sub_w_ = 0U;          // sub delay write head
  float *flanger_buf_;  // flanger delay lines: kFlangerLineCount MONO lines x kFlangerLineLenSmp
  uint32_t flanger_w_[modfx_consts::kFlangerLineCount]; // per-line write head, FLANGER

  // FLANGER filter state (cFlanger-inspired: HPF on feedback + LPF on wet output).
  float flanger_hpf_x_ = 0.f;  // HPF: previous input (feedback tap)
  float flanger_hpf_y_ = 0.f;  // HPF: previous output (filtered feedback)
  float flanger_lpf_l_ = 0.f;  // LPF: L wet output state
  float flanger_lpf_r_ = 0.f;  // LPF: R wet output state
  float *buffer_; // SDRAM base (getBufferSize() floats, owned by the runtime)

  // ---- Per-sample DSP helper functions (static inline, no libm) ----
  // These are defined here (inside the class scope) so that the inline
  // modfx_process_impl() below can call them directly (no separate dsp header).

  // Advance one LFO phase accumulator by (freq/sr) cycles and return its sine
  // (4th-order Taylor; no libm). `phase` is in CYCLES [0,1); `freq` is in
  // cycles/sec (Hz). step = freq/sr cycles per sample. The sine is computed
  // from ang = 2*pi*phase (radians). OLD bug: step was 2*pi*freq/sr, which
  // over-rotated the phase by 2*pi -> the LFO ran at 2*pi x the intended rate
  // (0.02 Hz -> 0.1256 Hz, a 50 s sweep read as an ~8 s beat).
  static float lfoStep(float &phase, float freq)
  {
    const float step = freq / modfx_consts::kSr;
    phase += step;
    if (phase >= 1.f) phase -= 1.f;
    const float ang = 6.283185307179586f * phase;
    const float x2 = ang * ang;
    return ang * (1.f - x2 / 6.f + x2 * x2 / 120.f);
  }

  // Pure sine of a phase in [0,1) (no phase advance, no libm). Used to read a
  // phase-shifted copy of an LFO without disturbing the shared accumulator.
  static float lfoSine(float phase)
  {
    const float ang = 6.283185307179586f * phase;
    const float x2 = ang * ang;
    return ang * (1.f - x2 / 6.f + x2 * x2 / 120.f);
  }
  // Triangle LFO of a phase in [0,1): -1..+1 with constant slope (no phase
  // advance, no libm). A triangle gives the constant-velocity sweep of a BBD
  // chorus (the smooth swishing wash); a sine dwells at the peaks and reads as
  // pitch-vibrato wobble. Used for the chorus LFO. Pure float math.
  static float lfoTri(float phase)
  {
    // phase in [0,1): 0 -> -1, 0.5 -> +1, 1 -> -1 (linear ramps).
    const float x = phase < 0.5f ? phase * 2.f : (1.f - phase) * 2.f;
    return 2.f * x - 1.f;
  }

  // One-pole low-pass (alpha in [0,1]) on a running state. No libm.
  static float onelpf(float &state, float x, float alpha)
  {
    state += alpha * (x - state);
    return state;
  }

  // One-pole high-pass (alpha in [0,1]) on a running state. No libm.
  static float onehpf(float &state, float x, float alpha)
  {
    state += alpha * (x - state);
    return x - state;
  }

  // ---- NOISE branch helpers (hand-rolled, no libm) ---------------------------
  // Exponential gap draw from one fx_rand() sample, exactly like the OSC
  // random-walk (osc_random_walk.h: -ln(U) * mean, U in (0,1] from the raw
  // LCG state). A 2nd-order Taylor of -ln(1-u) avoids libm logf. The gap is a
  // realization of a Poisson process with mean `mean_smp` samples -> a
  // "breathing" cadence (short bursts and long lulls, no metronome).
  static uint32_t noiseDrawGap(float mean_smp)
  {
    const uint32_t s = fx_rand() & 0x7FFFFFFFU; // [0, 0x7FFFFFFF]
    // u in (0, 1): map the raw state into the OPEN unit interval so ln(1-u)
    // stays finite (u = 1 would be ln 0 = inf -> a 0-length immediate gap).
    const float u = static_cast<float>(s) * (1.0f / 2147483648.0f) + 1.0f / 2147483648.0f; // (0, 1]
    // -ln(1 - u) via a 2nd-order Taylor: u + u^2/2. Accurate for u <= 0.5
    // (error < 1e-3); the tail (u > 0.5) is slightly under-drawn, which only
    // makes long lulls a touch rarer -- inaudible for a Poisson cadence.
    // (mean gap = mean(u + u^2/2) ~ 0.625 for uniform u, vs exact 1.0 -- the
    // cadence is ~40% denser than nominal; NRATE is a taste knob, not a meter.)
    const float lw = u + u * u * 0.5f; // = -ln(1 - u) approx, in (0, ~0.56]
    const float samples = lw * (mean_smp * 1.6f); // renormalize toward the true mean
    uint32_t n = static_cast<uint32_t>(samples);
    if (n < 1U)
      n = 1U;
    return n;
  }

  // 2-pole (12 dB/oct) state-variable filter, BAND-PASS tap (velocity state).
  // Same topology as drone-osc's osc_svf.h Svf::State (Ladder):
  //   vel += cc * (in - pos - ae*vel);  pos += cc * vel;  out = vel.
  // Coefficients cc = 2*sin(w0) / ae = 1 - cc/(2Q) are set K-rate (the caller
  // clamps them -- see the kNoiseBpCcMax/kNoiseBpAeMin in modfx_constants.h)
  // so the poles stay inside the unit circle across the whole 300 Hz .. 8 kHz
  // sweep. `in` is the gated, AM-modulated noise sample.
  static float noiseSvfBp(float &vel, float &pos, float in, float cc, float ae)
  {
    const float v = vel + cc * (in - pos - ae * vel);
    const float p = pos + cc * v;
    vel = v;
    pos = p;
    return v; // band-pass output (velocity)
  }

  // One step of the DRIFT random-walk channel `i` (per sample). Mirrors the
  // OSC RandomWalk (osc_random_walk.h): exponentially-drawn target gaps,
  // one-pole tracking with a per-channel time constant (tau 5..20 s, drawn at
  // each re-roll), amplitude scaled by the DRIFT ceiling, clamped to
  // [-1, +1]. Consumes fx_rand() ONLY when the channel actually re-rolls (or
  // once at startup via the initial drift_next_gap_ value), so a static
  // (DRIFT = 0) unit consumes no LCG state at all.
  void driftWalkStep(int i, float drift_ceiling)
  {
    if (drift_ceiling <= 0.f)
      return; // static: no re-roll, no fx_rand() consumption
    if (drift_next_gap_[i] > 0U)
      --drift_next_gap_[i];
    else
    {
      // New random target in [-1, +1] (one fx_rand() sample).
      const float u = static_cast<float>(fx_rand() & 0x7FFFFFFFU) *
                      (2.0f / 2147483648.0f); // (0, 1]
      drift_target_[i] = u * 2.f - 1.f;
      // Exponential next gap, mean 2..12 s (a second sample picks the mean,
      // a third draws the realization -- exactly the OSC's two-stage draw).
      const float mean_s = modfx_consts::kDriftGapMinSec +
                          (modfx_consts::kDriftGapMaxSec - modfx_consts::kDriftGapMinSec) *
                              (static_cast<float>(fx_rand() & 0x7FFFFFFFU) *
                               (1.0f / 2147483648.0f));
      const float draw = static_cast<float>(fx_rand() & 0x7FFFFFFFU) *
                         (1.0f / 2147483648.0f) + 1.0f / 2147483648.0f; // (0, 1]
      const float lw = draw + draw * draw * 0.5f; // -ln(1-draw) approx (0, ~0.56]
      uint32_t gap = static_cast<uint32_t>(lw * (mean_s * modfx_consts::kSr) * 1.6f);
      if (gap < 1U) gap = 1U;
      drift_next_gap_[i] = gap;
      // Per-channel time constant 5..20 s (a fourth sample).
      const float tau = modfx_consts::kDriftWalkTauMin +
                        (modfx_consts::kDriftWalkTauMax - modfx_consts::kDriftWalkTauMin) *
                            (static_cast<float>(fx_rand() & 0x7FFFFFFFU) *
                             (1.0f / 2147483648.0f));
      const float alpha = modfx_one_minus_expf(-1.0f / tau); // per-sample 1-e^(-1/tau)
      drift_tau_[i] = alpha;
    }
    // One-pole track of the (ceiling-scaled) target.
    const float tt = drift_target_[i] * drift_ceiling;
    drift_cur_[i] += drift_tau_[i] * (tt - drift_cur_[i]);
    if (drift_cur_[i] > 1.f) drift_cur_[i] = 1.f;
    else if (drift_cur_[i] < -1.f) drift_cur_[i] = -1.f;
  }

  // Soft-clip (same formula as fastertanh2f in common/utils/float_math.h).
  // Bounded: |out| < 1 for all finite |in|. No libm.
  static float softClip(float x)
  {
    const float xx = x * x;
    return x * (27.f + xx) / (27.f + 9.f * xx);
  }

  // ---- Real delay-line helpers (SDRAM line buffers, no libm) -----------------
  // A line is a pair of per-channel circular buffers (interleaved L/R, length
  // 2*len). The HEAD is an INTEGER (exact wrap, no float drift over 48k sps);
  // a per-sample fractional read offset `off` in [0,1) is tracked separately for
  // the fractional (LFO-modulated) delay reads. `write` (the head) is advanced
  // once per audio sample, storing both channels at the new head so L/R stay
  // in sync.
  static void lineWrite(float *buf, uint32_t len, uint32_t &head, float xl, float xr)
  {
    head = (head + 1U) % len;
    buf[head]     = xl; // ch 0 = L
    buf[head + 1] = xr; // ch 1 = R
  }

  // Fractional read: the sample `delay` (float, in samples) behind the newest
  // written sample, linearly interpolated. `head` is the newest-written slot.
  // `delay` is clamped to [1, len-1].
  static float lineReadF(float *buf, uint32_t len, int ch, uint32_t head, float delay)
  {
    if (delay < 1.f) delay = 1.f;
    const float lenf = (float)len;
    if (delay > lenf - 1.f) delay = lenf - 1.f;
    const float i0f = (float)head - delay; // newest is at `head`; one slot back is head-1
    // i0f is in (-(len-1), len-1]; one wrap each direction is sufficient.
    float w0 = i0f;
    if (w0 < 0.f) w0 += lenf;
    if (w0 >= lenf) w0 -= lenf;
    const uint32_t i0 = (uint32_t)w0;
    const uint32_t i1 = (i0 + 1U >= len) ? 0U : (i0 + 1U);
    // frac in [0,1): the fractional part of the PRE-WRAP anchor i0f.
    // floor(i0f) is i0f's integer part rounded DOWN (toward -inf).
    // OLD bug: (int)(i0f) truncates toward zero, so for i0f in (-1,0) it
    // gave 0 and frac = i0f - 0 was NEGATIVE (e.g. -0.3), which extrapolated
    // a + (b-a)*frac outside [a,b] -> a broadband click/hum on every read-tap
    // wraparound, alternating L/R (the stereo LFO offsets make L and R wrap
    // at different instants -> "each ear in turn").
    // floor via: trunc toward -inf. For i0f >= 0: (int)i0f. For i0f < 0:
    // (int)i0f - 1 (because trunc gives 0 or -0 for -0.x, but floor is -1).
    const float trunc_i0f = (float)(int)i0f;
    const float floor_i0f = (i0f < 0.f && i0f != trunc_i0f) ? (trunc_i0f - 1.0f) : trunc_i0f;
    float frac = i0f - floor_i0f; // now always in [0,1)
    if (frac < 0.f) frac += 1.f;  // safety net (should never trigger)
    const float a = buf[i0 + ch];
    const float b = buf[i1 + ch];
    return a + (b - a) * frac;
  }

  // Flanger write with FEEDBACK: store (x + fb * one-slot-back) at the new
  // head. This is the Reiss/McPherson flanger topology -- the feedback tap is
  // RE-INJECTED into the delay line (not summed at the output), so the
  // resonant "jet" build-up happens inside the line and stays smooth. `head`
  // is advanced once per sample (both channels), exactly like lineWrite.
  static void flangerWrite(float *buf, uint32_t len, uint32_t &head, float xl, float xr,
                        float fb, float prevl, float prevr)
  {
    head = (head + 1U) % len;
    buf[head]     = xl + fb * prevl;
    buf[head + 1] = xr + fb * prevr;
  }

  // Shift an LFO phase accumulator by a fixed fraction of a cycle (for the
  // per-channel stereo LFO offsets). No libm.
  static float phaseShift(float phase, float shift)
  {
    float p = phase + shift;
    while (p >= 1.f) p -= 1.f;
    while (p < 0.f) p += 1.f;
    return p;
  }

  // Advance ONE ensemble tap: advance its phase accumulator by (freq/sr)
  // cycles, return the binary-sine waveform value:
  //   w(phase) = 1.0*sin(2*pi*phase) + kEnsembleHarmonicWeight*sin(20*pi*phase)
  // The BASE sine has FULL amplitude 1.0 (NOT halved -- the original Teensy
  // LFO_RANGE=100 is an 8-bit unit ~0.098 full scale, so the original sweep is
  // much narrower; here we keep a clear ensemble motion). The 10x harmonic
  // (weight 1/16, kEnsembleHarmonicWeight) is the RS-202's "breathing" -- a
  // gentle amplitude modulation of the delay sweep. Both sines are hand-rolled
  // (no libm): each argument is first reduced to [-pi, pi] so the Taylor
  // series stays accurate, and the 6th-order Taylor is used (the 10x harmonic
  // argument is up to ~pi after reduction; 6th order is exact to <1e-9 there,
  // which removes the "graininess" a 4th-order series left on the fast term).
  static float ensLfoStep(float &phase, float freq)
  {
    const float step = freq / modfx_consts::kSr;
    phase += step;
    if (phase >= 1.f) phase -= 1.f;

    // Reduce an angle to [-pi, pi] (no libm): wrap into [0, 2*pi) then map the
    // upper half down by 2*pi. pi ~ 3.14159265, 2*pi ~ 6.28318531.
    auto reduce = [](float a) -> float
    {
      const float two_pi = 6.283185307179586f;
      while (a >= two_pi) a -= two_pi;
      while (a < 0.f) a += two_pi;
      if (a > 3.141592653589793f) a -= two_pi;
      return a;
    };

    // 6th-order Taylor of sin on a reduced argument (|x| <= pi):
    //   x - x^3/6 + x^5/120 - x^7/5040
    // Exact to < 1e-9 for |x| <= pi. No libm.
    auto sinT6 = [](float x) -> float
    {
      const float x2 = x * x;
      return x * (1.f + x2 * (-1.f / 6.f + x2 * (1.f / 120.f - x2 / 5040.f)));
    };

    const float base_arg = reduce(6.283185307179586f * phase);            // 2*pi*phase in [-pi,pi]
    const float harm_arg = reduce(62.83185307179586f * phase);            // 10*2*pi*phase in [-pi,pi]
    const float s0 = sinT6(base_arg);                                    // sin(2*pi*phase)
    const float s1 = sinT6(harm_arg);                                    // sin(10*2*pi*phase)
    return 1.0f * s0 + modfx_consts::kEnsembleHarmonicWeight * s1;
  }

  // Read the shared mono ensemble line at a fractional offset `pos` (samples
  // back from the newest written sample, pos in [0, lineLen)). Linear
  // interpolation, exact wrap via a power-of-2 mask (kEnsembleLineLenSmp = 1024).
  // `head` is the newest-written slot; one sample back is (head + len - 1).
  static float ensRead(float *buf, uint32_t len, uint32_t head, float pos)
  {
    if (pos < 0.f) pos = 0.f;
    if (pos > static_cast<float>(len - 1U)) pos = static_cast<float>(len - 1U);
    const uint32_t mask = len - 1U;
    const uint32_t base = (uint32_t)pos;
    const float frac = pos - base;
    const uint32_t i0 = (head + len - 1U - base) & mask;
    const uint32_t i1 = (i0 + 1U) & mask;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
  }

  // ---- NOISE v2 (KoS HurricaneSandy) helpers ---------------------------------
  // The NOISE branch is a granular rustle: a procedural PINK source (Voss 3-LPF)
  // feeds a ring buffer; a small granulator (kGrainVoices linear-env grains with
  // random pointer/offset, the classic MacPod scheme, written from scratch)
  // reads the ring; an AM "dips" curve makes it breathe; periodic white-noise
  // bursts DUCK the dry signal (the KoS sigEnv scrape). All libm-free.
  static constexpr uint32_t kGrainGap  = 0U;   // grain state: waiting
  static constexpr uint32_t kGrainIn   = 1U;   // grain state: fade-in
  static constexpr uint32_t kGrainSus  = 2U;   // grain state: sustain
  static constexpr uint32_t kGrainOut  = 3U;   // grain state: fade-out

  // Voss pink-noise step: 3 one-pole LPFs over fx_white; their subtraction gives
  // ~ -3 dB/oct. Returns one pink sample (roughly [-1,1], unscaled).
  inline float pinkStep()
  {
    const float w = fx_white();
    pink_lpf1_ = modfx_consts::kPinkA1 * pink_lpf1_ + (1.f - modfx_consts::kPinkA1) * w;
    pink_lpf2_ = modfx_consts::kPinkA2 * pink_lpf2_ + (1.f - modfx_consts::kPinkA2) * w;
    pink_lpf3_ = modfx_consts::kPinkA3 * pink_lpf3_ + (1.f - modfx_consts::kPinkA3) * w;
    return 0.5f * (pink_lpf1_ + pink_lpf2_) - 0.5f * pink_lpf3_;
  }

  // Draw a new grain for voice g: random start pointer (in the pink ring) and a
  // linear FADEIN -> SUSTAIN -> FADEOUT -> GAP schedule. Durations scale with the
  // random factor (KoS). All durations in samples at kSr.
  inline void grainDraw(uint32_t g)
  {
    const uint32_t len = modfx_consts::kPinkRingLenSmp;
    const float dur_smp = modfx_consts::kGrainDurationMs * 0.001f * modfx_consts::kSr;
    const float rnd = (static_cast<float>(fx_rand() & 0x7FFFFFFFU) * (2.0f / 2147483648.0f)) - 1.f; // (-1,1]
    float cnt = dur_smp * (1.f + modfx_consts::kGrainRandomFactor * rnd);
    if (cnt < 4.f) cnt = 4.f;
    const uint32_t total = static_cast<uint32_t>(cnt);
    const uint32_t ramp = static_cast<uint32_t>(total * (modfx_consts::kGrainRampPct / 100.0f));
    const uint32_t fadein  = ramp;
    grain_suslen_[g] = (total > 2U * ramp) ? (total - 2U * ramp) : 0U;
    grain_start_[g] = static_cast<uint32_t>(
        (static_cast<float>(fx_rand() & 0x7FFFFFFFU) * (1.0f / 2147483648.0f)) * (float)len);
    grain_pointer_[g] = (float)grain_start_[g];
    grain_state_[g] = kGrainIn;
    grain_counter_[g] = fadein;
    grain_escaler_[g] = 0.f;
    grain_erate_[g] = (fadein > 0U) ? (1.f / (float)fadein) : 1.f;
  }

  // Advance one granulator voice by one sample; returns its output (0 when GAP).
  inline float grainStep(uint32_t g)
  {
    if (grain_state_[g] == kGrainGap)
    {
      grainDraw(g);
      return 0.f;
    }
    const uint32_t len = modfx_consts::kPinkRingLenSmp;
    float pos = grain_pointer_[g];
    uint32_t base = static_cast<uint32_t>(pos);
    if (base >= len) base = 0U;
    const float frac = pos - (float)base;
    const uint32_t i0 = base;
    const uint32_t i1 = (base + 1U < len) ? (base + 1U) : 0U;
    float s = pink_ring_[i0] * (1.f - frac) + pink_ring_[i1] * frac;
    grain_pointer_[g] = pos + 1.f;
    if (grain_pointer_[g] >= (float)len) grain_pointer_[g] -= (float)len;
    if (grain_state_[g] == kGrainIn || grain_state_[g] == kGrainOut)
    {
      s *= grain_escaler_[g];
      grain_escaler_[g] += grain_erate_[g];
    }
    if (grain_counter_[g] > 0U)
      --grain_counter_[g];
    else
    {
      const uint32_t half = (uint32_t)(modfx_consts::kGrainDurationMs * 0.001f * modfx_consts::kSr * 0.5f);
      switch (grain_state_[g])
      {
      case kGrainIn:
        if (grain_suslen_[g] > 0U) { grain_state_[g] = kGrainSus; grain_counter_[g] = grain_suslen_[g]; }
        else { grain_state_[g] = kGrainOut; grain_counter_[g] = half; grain_escaler_[g] = 1.f; grain_erate_[g] = -grain_erate_[g]; }
        break;
      case kGrainSus:
        grain_state_[g] = kGrainOut; grain_counter_[g] = half; grain_escaler_[g] = 1.f; grain_erate_[g] = -grain_erate_[g];
        break;
      case kGrainOut:
        grain_state_[g] = kGrainGap;
        break;
      default: break;
      }
    }
    return s;
  }

  // Re-seed the AM "dips" envelope (kDipsCount random points over 1 s, 1.0 down
  // to (1-extremity)). Called every kDipsDomainSmp samples.
  inline void dipsReseed()
  {
    const uint32_t n = modfx_consts::kDipsCount;
    const float part = 1.f / (float)(n + 1U);
    dips_pts_x_[0] = 0.f; dips_pts_y_[0] = 1.f;
    for (uint32_t i = 1U; i < n; ++i)
    {
      const float rnd = static_cast<float>(fx_rand() & 0x7FFFFFFFU) * (1.0f / 2147483648.0f);
      dips_pts_x_[i] = (float)i * part + (rnd - 0.5f) * part * 0.4f;
      dips_pts_y_[i] = 1.f - modfx_consts::kDipsExtremity * static_cast<float>(fx_rand() & 0x7FFFFFFFU) * (1.0f / 2147483648.0f);
    }
    dips_pts_x_[n - 1U] = 1.f; dips_pts_y_[n - 1U] = 1.f;
    dips_incr_ = 0U;
    dips_cur_i_ = 0U;
    dips_x_[0] = 0.f; dips_y_[0] = 1.f;
    dips_x_[1] = (n > 1U) ? dips_pts_x_[1] : 1.f;
    dips_y_[1] = (n > 1U) ? dips_pts_y_[1] : 1.f;
  }

  // Per-sample AM-dips value: linear interp of the current bracketing pair;
  // re-seed when the domain counter wraps.
  inline float dipsStep()
  {
    ++dips_incr_;
    if (dips_incr_ >= (uint32_t)modfx_consts::kDipsDomainSmp)
    {
      dipsReseed();
      return 1.f;
    }
    const float x = static_cast<float>(dips_incr_) / modfx_consts::kDipsDomainSmp;
    if (x >= dips_x_[1])
    {
      const uint32_t n = modfx_consts::kDipsCount;
      while ((dips_cur_i_ + 1U < n) && (x >= dips_x_[1]))
      {
        ++dips_cur_i_;
        dips_x_[0] = dips_x_[1]; dips_y_[0] = dips_y_[1];
        dips_x_[1] = dips_pts_x_[dips_cur_i_ + 1U];
        dips_y_[1] = dips_pts_y_[dips_cur_i_ + 1U];
      }
      if (dips_cur_i_ + 1U >= n) return dips_y_[1]; // tail: hold the last point
    }
    const float span = dips_x_[1] - dips_x_[0];
    const float d = (span > 0.f) ? ((x - dips_x_[0]) / span) : 0.f;
    return dips_y_[0] + d * (dips_y_[1] - dips_y_[0]);
  }

  // The per-sample audio path (upmix -> CHORUS -> FLANGER -> TAPE ->
  // makeup -> out). Defined INLINE inside the class so that
  // multiple translation units can each #include modfx.h through different
  // absolute paths without a duplicate-symbol link error (ODR: inline
  // member functions may be defined in every TU; the linker keeps one).
  inline void modfx_process_impl(const float *__restrict in, float *__restrict out, uint32_t frames)
{
  const Params p = params_;
  float in_peak = 0.f, out_peak = 0.f;
  const float sr = modfx_consts::kSr;

  // ---- K-rate LFO frequencies (from TIME; Taylor exp, no libm) --------------
  const float t = p.time;
  const float flanger_freq = modfx_consts::kFlangerLFOMin * modfx_expf(modfx_consts::kFlangerLFOlnR * t);

  // ---- CHORUS Juno-chorus K-rate setup ----------------------------------------
  // CMODE knob -> Juno voice blend, 3 EQUAL bands (Formant-Vowel style):
  //   band 0 (CMODE < 1/3)  -> mode I    (main voice only)
  //   band 1 (1/3..2/3)     -> mode I+II (both voices, classic Juno)
  //   band 2 (>= 2/3)       -> mode II   (sub voice only)
  // Equal thirds of 0..1 so every knob value lands cleanly in a band.
  const int chorus_mode = (p.cmode < 1.f/3.f) ? 0 : (p.cmode < 2.f/3.f) ? 1 : 2;
  const float chorus_gain_main = (chorus_mode == 0) ? modfx_consts::kChorusGainI[0]
                            : (chorus_mode == 1) ? modfx_consts::kChorusGainI2[0] : modfx_consts::kChorusGainII[0];
  const float chorus_gain_sub  = (chorus_mode == 0) ? modfx_consts::kChorusGainI[1]
                            : (chorus_mode == 1) ? modfx_consts::kChorusGainI2[1] : modfx_consts::kChorusGainII[1];

  // TIME knob -> chorus LFO rate (shared exp curve, same as FLANGER).
  // The main-voice LFO frequency comes from the common
  // kChorusLFOMin * exp(kChorusLFOlnR * TIME) curve (0.0167 Hz = 60 s at TIME=0
  // -> 0.6 Hz = 1.7 s at TIME=1). The sub-voice LFO is DETUNED against the
  // main by kChorusLfoDetune (x1.0618): the two voices share one "heartbeat"
  // but slowly drift in and out of phase, producing the classic chorus
  // "swelling" (beating) instead of two static layers.
  const float chorus_lfo1_freq = modfx_consts::kChorusLFOMin * modfx_expf(modfx_consts::kChorusLFOlnR * t); // Hz (main)
  const float chorus_lfo2_freq = chorus_lfo1_freq * modfx_consts::kChorusLfoDetune;                        // Hz (sub, detuned)
  // q31 increment = freq * 2^31 / sr. Recomputed every block so TIME moves
  // the rate live with no phase jump: the phase accumulator keeps running,
  // only its step size changes.
  chorus_lfo1_w_ = (int32_t)(chorus_lfo1_freq * 2147483648.0f / sr);
  chorus_lfo2_w_ = (int32_t)(chorus_lfo2_freq * 2147483648.0f / sr);

  // DEPTH knob -> LFO spread (how far the LFO modulates the delay).
  // DEPTH=0 -> no modulation (delay stays at center, chorus is "dry").
  // DEPTH=1 -> full modulation (delay sweeps min..max, chorus is "deep").
  // CHORUS knob -> dry/wet crossfade (0=dry only, 1=full chorus).
  const float chorus_depth = p.depth; // 0..1 (LFO spread)
  const float chorus_master = p.chorus; // 0..1 (dry/wet)
  // Fixed 1st-order LPF alpha (Juno RC anti-alias, mid cutoff). No longer
  // swept by TIME: CMODE selects the voice, TIME drives the LFO rate.
  chorus_lpf_alpha_ = 0.5f;

  // FLANGER (multi-line / bucket-brigade): small feedback + center delay. FOUR mono
  // lines, each with its own LFO phase (kFlangerLinePhase), average into a smooth
  // thru-zero comb WASH. Small feedback (FLANGER × 0.15, Q ~1.18) smooths the comb
  // peaks; the 4 de-tuned lines do not align at the LFO turnaround. Center delay
  // scales with DEPTH^2 (48..360 smp = 1..7.5 ms); each line's LFO sweeps the
  // read tap ±20% × DEPTH^2.
  // p.flanger is already clamped to [0,1] in setParameter, and
  // kFlangerFbMax (0.3) is below 1.0, so flanger_fb <= kFlangerFbMax always.
  // The ternary (not a raw multiply) keeps the old "no feedback at FLANGER=0"
  // behavior and avoids a floating-point 0.0 * 0.3 = 0.0 edge case.
  const float flanger_fb = (p.flanger > 0.f) ? (p.flanger * modfx_consts::kFlangerFbMax) : 0.f;
  const float d2 = p.depth * p.depth;
  const float flanger_delay_smp = modfx_consts::kFlangerDelayMinSmp +
      d2 * (modfx_consts::kFlangerDelayMaxSmp - modfx_consts::kFlangerDelayMinSmp);

  // CHORUS Juno-chorus delay ranges (samples). The LFO sweeps within these.
  // constexpr in modfx_constants.h (pre-computed at kSr=48000) -- saves 8
  // float multiplies per block, audio is bit-identical.
  const float chorus_main_dmin = modfx_consts::kChorusMainDelayMinSmp;
  const float chorus_main_dmax = modfx_consts::kChorusMainDelayMaxSmp;
  const float chorus_sub_dmin  = modfx_consts::kChorusSubDelayMinSmp;
  const float chorus_sub_dmax  = modfx_consts::kChorusSubDelayMaxSmp;

  // ---- NOISE v2 K-rate setup (burst cadence from NRATE) ----------------------
  // NOISE = 0 -> dry wire (the branch is never computed, regression-safe).
  // The granular rustle / AM-dips / room-tone are PER-SAMPLE. The only K-rate
  // value is the burst lull mean (NRATE -> flashes/second, exponential curve),
  // stored in noise_burst_mean_smp_ and drawn by noiseDrawGap() per lull.
  const float noise_mix = p.noise * modfx_consts::kNoiseWetNorm; // 0..0.5 final wet
  {
    // NRATE 0 -> 0.15 fl/s (rare), NRATE 1 -> 8 fl/s (dense). Gap mean (samples)
    // = 1 / (rate * sr), where rate = minPerSec * exp(lnR * NRATE).
    const float rate_hz = modfx_consts::kNoiseBurstMinPerSec *
                          modfx_expf(modfx_consts::kNoiseBurstLnR * p.nrate);
    uint32_t mean = static_cast<uint32_t>(1.f / (rate_hz * modfx_consts::kSr));
    if (mean < 32U) mean = 32U;
    noise_burst_mean_smp_ = mean;
  }


  // ---- ENSEMBLE K-rate setup (LFO rate from TIME, spread from DEPTH) ---------
  // The base LFO rate sweeps 0.0167..0.6 Hz with the SAME exp curve as CHORUS /
  // FLANGER (kEnsembleLFOMin * exp(kEnsembleLFOlnR * TIME)), so the ensemble
  // moves in lockstep with the other stages. The 10x harmonic (the RS-202
  // "breathing") is computed inside ensLfoStep. The LFO DELAY SPREAD is driven
  // by DEPTH^2 (like the FLANGER): 0 at DEPTH=0 (near-static layer) up to
  // kEnsembleMaxSpreadSmp at DEPTH=1 (a full 9.6..20.4 ms swell). The CENTER
  // delay is fixed at kEnsembleDelayCenterSmp (15 ms). Recomputed every block
  // so TIME / DEPTH move the branch live with no phase jump.
  const float ens_wet = p.ensemble * modfx_consts::kEnsembleWetNorm; // 0..1 final wet (matched to CHORUS)
  if (p.ensemble > 0.f)
  {
    ens_lfo_freq_ = modfx_consts::kEnsembleLFOMin * modfx_expf(modfx_consts::kEnsembleLFOlnR * t);
    const float d2 = p.depth * p.depth; // DEPTH^2 (like the FLANGER's depth curve)
    ens_spread_ = d2 * modfx_consts::kEnsembleMaxSpreadSmp;
  }

  // ---- TAPWOB (tape wow/flutter) per-block setup ------------------------------
  // TAPWOB is the ONLY pitch-mod stage. It uses the FIXED TIME / DEPTH knobs,
  // like every other stage:
  //   * TIME  -> LFO tempo (wow/flutter rates follow the shared exp curve, so
  //     the whole stack breathes together: slow at TIME=0, fast at TIME=1).
  //   * DEPTH -> wow/flutter depth in CENTS. WOW uses a CUBIC curve (DEPTH^3,
  //     much shallower than the FLANGER's DEPTH^2) so low DEPTH stays a subtle
  //     pitch shimmer; FLUTTER stays on DEPTH^2 (12 cents is already tame).
  //   * TAPWOB -> DRY/WET level only (0 = off, 1 = full tape deck) and the LPF
  //     "dullness". Recomputed per block (no phase jump). TAPWOB = 0 -> the
  //     branch is never computed (dry wire, regression-safe).
  const float tapwob_wet = p.tapwob * modfx_consts::kTapeWobWetMax; // 0..1
  if (p.tapwob > 0.f)
  {
    // LFO rates from TIME (shared exp curve, same as CHORUS/FLANGER/ENSEMBLE).
    tapwob_wow_freq_ = modfx_consts::kTapeWobWowMinHz * modfx_expf(modfx_consts::kTapeWobWowLnR * p.time);
    tapwob_flut_freq_ = modfx_consts::kTapeWobFlutMinHz * modfx_expf(modfx_consts::kTapeWobFlutLnR * p.time);
    // Depth (cents): WOW uses a cubic curve (DEPTH^3, much shallower than the
    // FLANGER's DEPTH^2) so low DEPTH stays a subtle pitch shimmer rather than
    // an audible wobble; FLUTTER stays on DEPTH^2 (12 cents is already tame).
    const float tapwob_depth2 = p.depth * p.depth;
    const float tapwob_depth3 = tapwob_depth2 * p.depth;
    tapwob_wow_cents_ = tapwob_depth3 * modfx_consts::kTapeWobWowCentsMax;
    tapwob_flut_cents_ = tapwob_depth2 * modfx_consts::kTapeWobFlutCentsMax;
    // LPF cutoff 2..8 kHz from TAPWOB (the "dullness" of the deck); alpha via
    // one_minus_expf (valid for small |x|, x = -2*pi*fc/sr ~ -1..-4).
    const float tapwob_lpf_hz = modfx_consts::kTapeWobLPFMinHz * modfx_expf(1.38629f * p.tapwob); // ln(4)
    const float tapwob_x = -2.f * 3.14159265f * tapwob_lpf_hz / modfx_consts::kSr;
    tapwob_lpf_alpha_ = modfx_one_minus_expf(tapwob_x);
  }

  // ---- WARP (magnetic instability) per-block setup ----------------------------
  // WARP is the KoS "Shame" stage: a 4096-smp mono line whose read tap is a
  // cosine LFO. WARP knob -> LFO rate (7..77 Hz, shared exp curve) AND depth
  // (0..60 smp). The per-cycle random rate kick (warp_rate_fluct_) is re-drawn
  // in the per-sample path (fx_rand), so this only sets the BASE rate/depth.
  // WARP = 0 -> off (dry wire, regression-safe).
  const float warp_on = (p.warp > 0.f);
  if (warp_on)
  {
    warp_lfo_freq_ = modfx_consts::kWarpLFOMinHz * modfx_expf(modfx_consts::kWarpLFOlnR * p.warp);
    warp_rate_ = warp_lfo_freq_ / modfx_consts::kSr; // cycles/sample (base)
    warp_depth_ = p.warp * modfx_consts::kWarpMaxDepthSmp; // 0..60 smp
  }

  // ---- HISS (tape hiss) per-block setup ---------------------------------------
  // HISS knob -> hiss level (0..kHissMaxGain). The HPF alpha is fixed
  // (kHissHpfFc); no per-sample work beyond the one-pole. HISS = 0 -> off.
  const float hiss_level = p.hiss * modfx_consts::kHissMaxGain; // 0..0.08

  // ---- Per-sample path ------------------------------------------------------
  for (uint32_t n = 0; n < frames; ++n)
  {
    // `in` is INTERLEAVED STEREO (out[2n]=L, out[2n+1]=R) -- both the NTS-1
    // platform (modfx units are stereo, see drone-modfx/unit.cc) and the web
    // chain (Chain::process upmixes the mono OSC to L/R) deliver it this way.
    // The OLD code read `in[n]` (mono layout, frames samples) from this
    // 2*frames interleaved buffer: it consumed ONLY the even samples (the L
    // channel) and treated the odd (R) samples as the next frame of L --
    // a half-rate garbled signal. In the web chain L==R (the upmix is a
    // plain duplicate), so the L half still happened to be clean; on the
    // NTS-1, where the platform feeds true stereo, it would have been broken.
    const float in_l = in[2 * n];
    const float in_r = in[2 * n + 1];
    const float in_l_abs = (in_l < 0.f) ? -in_l : in_l;
    if (in_l_abs > in_peak) in_peak = in_l_abs;
    const float in_r_abs = (in_r < 0.f) ? -in_r : in_r;
    if (in_r_abs > in_peak) in_peak = in_r_abs;

    // Per-channel working vars (L/R) + per-stage wet amounts.
    float L = in_l;
    float R = in_r;
    float fln_wet = p.flanger;

    // ---- NOISE v2: granular rustle (KoS HurricaneSandy, additive, before CHORUS)
    // A procedural PINK source (Voss) feeds a ring buffer; a small granulator
    // (kGrainVoices linear-env grains) reads it -> HPF/LPF; an AM "dips" curve
    // makes the rustle breathe; periodic white-noise bursts DUCK the dry signal
    // (the KoS sigEnv scrape); a low-freq room tone fills the low end. Summed
    // into the input so the rustle passes through the chorus/flanger "in the
    // wash". Skipped entirely at NOISE = 0 (dry wire -- regression-safe).
    if (noise_mix > 0.f)
    {
      // DRIFT walk, channel kDriftNoise (the AM modulator). Channels 0/1 are the
      // reserved CHORUS/FLANGER level walks -- kept advancing (see state block).
      driftWalkStep(kDriftNoise, p.drift);
      driftWalkStep(0, p.drift);
      driftWalkStep(1, p.drift);
      const float am = 0.5f + 0.5f * drift_cur_[kDriftNoise]; // [0, 1]

      // 1) Fill the pink ring with one pink sample per frame (mono source).
      const uint32_t plen = modfx_consts::kPinkRingLenSmp;
      pink_ring_[pink_w_] = pinkStep();
      ++pink_w_;
      if (pink_w_ >= plen) pink_w_ = 0U;

      // 2) Granular rustle: sum the voices, then band-shape (HPF + per-ch LPF).
      float grain = 0.f;
      for (uint32_t g = 0U; g < modfx_consts::kGrainVoices; ++g)
        grain += grainStep(g);
      grain *= modfx_consts::kGrainGain / (float)modfx_consts::kGrainVoices;
      // Shared HPF (50 Hz): y = (1-a)*(x - x[n-1]) + y[n-1].
      const float hpf_a = 0.99790f; // precomputed exp(-2*pi*50/48000)
      const float hpf = (1.f - hpf_a) * (grain - grain_hpf_x_);
      grain_hpf_x_ = grain;
      grain_hpf_y_ = hpf_a * grain_hpf_y_ + hpf;
      // Per-channel LPF (2 kHz): alpha = 1-exp(-2*pi*2000/48000).
      const float glpf_a = 0.23010f;
      grain_lpf_l_ = glpf_a * grain_hpf_y_ + (1.f - glpf_a) * grain_lpf_l_;
      grain_lpf_r_ = glpf_a * grain_hpf_y_ + (1.f - glpf_a) * grain_lpf_r_;

      // 3) AM "dips": the rustle BREATHEs (1.0 -> 0.5 over 1 s, random points).
      const float dips = dipsStep();
      const float rustle_l = grain_lpf_l_ * am * dips;
      const float rustle_r = grain_lpf_r_ * am * dips;

      // 4) Noise-burst "scrape": a fast white-noise flash that DUCKS the dry
      // signal (KoS sigEnv). Cadence from NRATE (noise_burst_mean_smp_).
      float burst = 0.f;
      float duck = 1.f;
      if (burst_timer_ > 0U)
      {
        --burst_timer_;
        const float prog = 1.f - (float)burst_timer_ / modfx_consts::kBurstWindowSmp; // 0..1
        // Attack/release envelope (KoS noiseEnv shape): rise to kBurstPeak, fall.
        if (prog < modfx_consts::kBurstAtkFrac)
          burst = (prog / modfx_consts::kBurstAtkFrac) * modfx_consts::kBurstPeak;
        else
          burst = (1.f - (prog - modfx_consts::kBurstAtkFrac) / (1.f - modfx_consts::kBurstAtkFrac)) * modfx_consts::kBurstPeak;
        duck = 1.f - modfx_consts::kBurstDuckDepth * (burst / modfx_consts::kBurstPeak);
      }
      else
      {
        // Idle: count down the lull (NRATE cadence); when it expires, fire a
        // scrape flash and draw the next lull. (The gap is set on init /
        // re-fire, so this just gates when the next burst starts.)
        if (noise_gap_timer_ > 0U)
          --noise_gap_timer_;
        else
        {
          burst_timer_ = (uint32_t)modfx_consts::kBurstWindowSmp;
          noise_gap_timer_ = noiseDrawGap(static_cast<float>(noise_burst_mean_smp_));
        }
      }

      // 5) Low-freq room tone (KoS lowFreqGrainNoise): pink -> 1-pole LPF 80 Hz.
      const float rt_a = 0.01047f; // precomputed 1-exp(-2*pi*80/48000)
      roomtone_lpf_ = rt_a * pink_ring_[pink_w_] + (1.f - rt_a) * roomtone_lpf_;
      const float room = roomtone_lpf_ * modfx_consts::kRoomToneLevel;

      // 6) Mix: rustle + scrape burst + room tone, scaled by NOISE, DUCK the dry.
      const float n_l = rustle_l + burst * fx_white() + room;
      const float n_r = rustle_r + burst * fx_white() + room;
      L = L * duck + noise_mix * n_l;
      R = R * duck + noise_mix * n_r;
    }

    // ---- ENSEMBLE: string-synth ensemble (RS-202 style, additive, before CHORUS)
    // One SHARED MONO delay line; three read taps at LFO phases 120 deg apart,
    // summed and averaged (x 1/3). The LFO is a binary-sine (base + 10x
    // harmonic) -- see ensLfoStep. R reads the same taps shifted by
    // kEnsemblePhase90 (367 smp ~ 90 deg of the line) for the stereo spread.
    // A 1st-order LPF per channel keeps the comb edges smooth. Skipped entirely
    // at ENSMBL = 0 (dry wire -- regression-safe, state untouched).
    if (ens_wet > 0.f)
    {
      // Mono input: the web chain upmixes OSC mono to L/R as a plain duplicate,
      // and on the NTS-1 the OSC->MOD feed is mono-duplicated by the platform
      // too. Write the L sample to the shared mono line (L==R in practice).
      ensemble_delay_[(ensemble_w_++) & (modfx_consts::kEnsembleLineLenSmp - 1U)] = L;
      // Advance the three tap LFOs and read the shared line at each.
      const float w0 = ensLfoStep(ens_phase_[0], ens_lfo_freq_);
      const float w1 = ensLfoStep(ens_phase_[1], ens_lfo_freq_);
      const float w2 = ensLfoStep(ens_phase_[2], ens_lfo_freq_);
      // Delay center + spread: the LFO waveform w in [-1, 1] maps to a delay
      // of (center + spread * w) samples. Center is FIXED at
      // kEnsembleDelayCenterSmp (15 ms -- a string-machine / chorus-main delay
      // zone). The SPREAD is DEPTH^2-driven (computed in the K-rate setup as
      // ens_spread_ = DEPTH^2 * kEnsembleMaxSpreadSmp): 0 at DEPTH=0 (a
      // near-static layer) up to a full 9.6..20.4 ms swell at DEPTH=1. Both
      // stay well inside the 1024-smp line (max 720 + 260 + 367 = 1347 < 2048
      // after the R-channel +367 shift, clamped in ensRead).
      const float center = modfx_consts::kEnsembleDelayCenterSmp;
      const float spread = ens_spread_;
      // L: taps at their native offsets. R: the SAME taps shifted +367 smp.
      const uint32_t LEN = modfx_consts::kEnsembleLineLenSmp;
      const uint32_t P90 = modfx_consts::kEnsemblePhase90;
      const float dl0 = center + spread * w0;
      const float dl1 = center + spread * w1;
      const float dl2 = center + spread * w2;
      // L channel reads (no shift); R channel reads (+P90, clamped inside ensRead).
      const float l_sum = ensRead(ensemble_delay_, LEN, ensemble_w_ - 1U, dl0) +
                          ensRead(ensemble_delay_, LEN, ensemble_w_ - 1U, dl1) +
                          ensRead(ensemble_delay_, LEN, ensemble_w_ - 1U, dl2);
      const float r_sum = ensRead(ensemble_delay_, LEN, ensemble_w_ - 1U, dl0 + P90) +
                          ensRead(ensemble_delay_, LEN, ensemble_w_ - 1U, dl1 + P90) +
                          ensRead(ensemble_delay_, LEN, ensemble_w_ - 1U, dl2 + P90);
      // Average the three taps (x 1/3) and apply the 1st-order LPF per channel.
      const float a3 = l_sum * (1.f / 3.f);
      const float b3 = r_sum * (1.f / 3.f);
      const float alpf = modfx_consts::kEnsembleLpfAlpha;
      const float o_l = ens_lpf_l_ + alpf * (a3 - ens_lpf_l_);
      const float o_r = ens_lpf_r_ + alpf * (b3 - ens_lpf_r_);
      ens_lpf_l_ = o_l;
      ens_lpf_r_ = o_r;
      // Additive mix (like NOISE): the ensemble "strings" pass through the
      // CHORUS / FLANGER "in the wash".
      L += ens_wet * o_l;
      R += ens_wet * o_r;
    }

    // ---- CHORUS: Juno-60 stereo chorus (2 BBD voices, raw primitives) ----------
    // Architecture: peterall/junologue-chorus (MIT), adapted with hand-rolled
    // delay lines / LFOs / LPFs (no CMSIS dependency for the web sim).
    // Two mono delay lines (main + sub), each driven by a triangle LFO whose
    // rate is set K-rate from TIME (main = base Juno 0.513 Hz, sub detuned
    // x1.0618 -> shared heartbeat that drifts in/out of phase = chorus pulsing).
    // Each LFO reads the delay at two fractional taps (t and 1-t) and sums them
    // with the CMODE-selected per-voice gains. 1st-order LPF post-delay per
    // channel. CHORUS(dry/wet)=0 -> dry only. CHORUS=1 -> full chorus.
    if (chorus_master > 0.f)
    {
      // Advance the two triangle LFOs (q31 phase accumulators).
      chorus_lfo1_phase_ += chorus_lfo1_w_;
      chorus_lfo2_phase_ += chorus_lfo2_w_;
      // Triangle unipolar 0..1 from q31 phase: |q31_to_f32(phase)|
      const float lfo1 = (chorus_lfo1_phase_ < 0) ? -(chorus_lfo1_phase_ * 4.6566128730773926e-10f)
                                                 :  (chorus_lfo1_phase_ * 4.6566128730773926e-10f);
      const float lfo2 = (chorus_lfo2_phase_ < 0) ? -(chorus_lfo2_phase_ * 4.6566128730773926e-10f)
                                                 :  (chorus_lfo2_phase_ * 4.6566128730773926e-10f);

      // SoftLimit (Juno BBD saturation, stmlib).
      const float in_mono = (L + R) * 0.5f;
      const float x2 = in_mono * in_mono;
      const float sl_in = in_mono * (27.f + x2) / (27.f + 9.f * x2);

      // Write to the mono delay lines (raw, power-of-2 mask).
      const uint32_t dl_mask = modfx_consts::kChorusLineLenSmp - 1U;
      chorus_main_delay_[(chorus_main_w_--) & dl_mask] = sl_in;
      chorus_sub_delay_[(chorus_sub_w_--) & dl_mask] = sl_in;

      // Read at the two LFO taps (fractional, linear interpolation).
      auto readFrac = [](float *buf, uint32_t w, uint32_t mask, float pos) -> float {
        const uint32_t base = (uint32_t)pos;
        const float frac = pos - base;
        const float s0 = buf[(w + base) & mask];
        const float s1 = buf[(w + base + 1U) & mask];
        return s0 + frac * (s1 - s0);
      };

      // LFO spread from DEPTH: delay = center +/- spread * DEPTH * (lfo - 0.5).
      // DEPTH=0 -> delay=center (no modulation). DEPTH=1 -> delay=min..max.
      const float main_center = (chorus_main_dmin + chorus_main_dmax) * 0.5f;
      const float main_spread = (chorus_main_dmax - chorus_main_dmin) * 0.5f * chorus_depth;
      const float sub_center  = (chorus_sub_dmin  + chorus_sub_dmax)  * 0.5f;
      const float sub_spread  = (chorus_sub_dmax  - chorus_sub_dmin)  * 0.5f * chorus_depth;
      const float main_t  = main_center + main_spread * (lfo1 - 0.5f);
      const float main_t2 = main_center + main_spread * (0.5f - lfo2);
      const float sub_t   = sub_center  + sub_spread  * (lfo1 - 0.5f);
      const float sub_t2  = sub_center  + sub_spread  * (0.5f - lfo2);

      const float main_wet = readFrac(chorus_main_delay_, chorus_main_w_, dl_mask, main_t) * chorus_gain_main +
                             readFrac(chorus_main_delay_, chorus_main_w_, dl_mask, main_t2) * chorus_gain_sub;
      const float sub_wet  = readFrac(chorus_sub_delay_, chorus_sub_w_, dl_mask, sub_t) * chorus_gain_main +
                             readFrac(chorus_sub_delay_, chorus_sub_w_, dl_mask, sub_t2) * chorus_gain_sub;

      // Post-LPF (1st-order one-pole, per channel for stereo phase image).
      const float wet_total_l = main_wet + sub_wet;
      const float wet_total_r = main_wet + sub_wet; // mono sum (Juno BBD is mono)
      chorus_main_post_lpf_l_ += chorus_lpf_alpha_ * (wet_total_l - chorus_main_post_lpf_l_);
      chorus_main_post_lpf_r_ += chorus_lpf_alpha_ * (wet_total_r - chorus_main_post_lpf_r_);
      const float wet_l = chorus_main_post_lpf_l_ + chorus_sub_post_lpf_l_;
      const float wet_r = chorus_main_post_lpf_r_ + chorus_sub_post_lpf_r_;

      // Dry/wet blend: CHORUS = crossfade (0=dry only, 1=full chorus).
      // At CHORUS=0: out = dry (wire). At CHORUS=1: out = dry + wet.
      // DEPTH already applied to the LFO delay (spread), not the gain.
      L = L + chorus_master * wet_l;
      R = R + chorus_master * wet_r;
    }

    // ---- FLANGER: thru-zero flanger (multi-line / bucket-brigade) ----------------
    // Based on the cFlanger reference (calgoheen/cgo_modules Flanger.h):
    //   - 4 mono delay lines, average into a smooth comb WASH.
    //   - Feedback (FLANGER x 0.15, Q ~1.18) re-injected through a HPF (~200 Hz)
    //     so bass does NOT build up in the feedback loop (cFlanger's
    //     "safeBass" -- the fix for the "distorted peaks" / "harsh" symptom).
    //   - Wet output goes through a LPF (~2 kHz) to tame the comb's high-end
    //     harshness (cFlanger's "warmth" filter).
    //   - Wet is x 0.5 (normalization: dry+wet sums to ~unity, not 2x).
    //   - LFO is TRIANGLE (constant-velocity sweep, no "dwelling" at the
    //     peaks where the interpolation artifacts are worst; cFlanger uses
    //     triangle, not sine).
    //   - LFO sweeps +/-10% x DEPTH^2 around the DEPTH^2 center (48..360 smp).
    //   - R is half a cycle out of L (stereo comb).
    // out = dry + FLANGER * 0.5 * LPF(mean of 4 line wet taps). fln_wet == 0 -> wire.
    if (fln_wet > 0.f)
    {
      // Triangle LFO (constant velocity) instead of sine: no dwelling at peaks.
      lfoStep(flanger_lfo_phase_, flanger_freq);
      const float lfo_tri_l = lfoTri(flanger_lfo_phase_);
      const float lfo_tri_r = lfoTri(phaseShift(flanger_lfo_phase_, 0.5f)); // R half-cycle out

      const uint32_t LEN = modfx_consts::kFlangerLineLenSmp;
      const float flanger_len_f = static_cast<float>(LEN);
      const float sweep = flanger_delay_smp * 0.15f * d2; // +/-15% x DEPTH^2 (deeper wash)
      const float ninv = 0.75f / (float)modfx_consts::kFlangerLineCount; // 0.75 wet gain / 4 lines (deeper wash)

      float wetsum_l = 0.f, wetsum_r = 0.f;
      for (uint32_t li = 0; li < modfx_consts::kFlangerLineCount; ++li)
      {
        float *line = flanger_buf_ + li * LEN;   // mono line li
        const uint32_t H = flanger_w_[li];
        const uint32_t Hp = (H > 0U) ? (H - 1U) : (LEN - 1U); // one slot back (feedback tap)

        const float lfo_l = lfo_tri_l;  // all lines share the same LFO phase (kFlangerLinePhase = {0,0,0,0})
        const float lfo_r = lfo_tri_r;  // R is half a cycle out

        // cFlanger: feedback goes through a HPF (safeBass) before re-injection.
        // One-pole HPF: y[n] = (1-a)*(x[n]-x[n-1]) + y[n-1], a = exp(-2*pi*fc/sr).
        // fc = 200 Hz @ 48 kHz -> a = exp(-2*pi*200/48000) = exp(-0.02618) = 0.97416.
        const float hpf_a = 0.97416f; // precomputed: exp(-2*pi*200/48000)
        const float x = L;
        const float prev = line[Hp];
        // HPF the feedback tap (per line, shared state -- use a single HPF for
        // all 4 lines since they all process the same signal L==R).
        const float fb_hpf = (1.f - hpf_a) * (prev - flanger_hpf_x_);
        flanger_hpf_x_ = prev;
        flanger_hpf_y_ = hpf_a * flanger_hpf_y_ + fb_hpf;
        // Write (x + fb * HPF(prev)) at the new head.
        flangerWrite(line, LEN, flanger_w_[li], x, x, flanger_fb, flanger_hpf_y_, flanger_hpf_y_);

        const float mod_l = flanger_delay_smp + sweep * lfo_l;
        const float mod_r = flanger_delay_smp + sweep * lfo_r;
        const float d_l = (mod_l < 1.f) ? 1.f : ((mod_l > flanger_len_f - 1.f) ? (flanger_len_f - 1.f) : mod_l);
        const float d_r = (mod_r < 1.f) ? 1.f : ((mod_r > flanger_len_f - 1.f) ? (flanger_len_f - 1.f) : mod_r);
        const float w_l = lineReadF(line, LEN, 0, flanger_w_[li], d_l);
        const float w_r = lineReadF(line, LEN, 0, flanger_w_[li], d_r);
        wetsum_l += w_l;
        wetsum_r += w_r;
      }
      // cFlanger: wet output goes through a LPF (warmth, ~2 kHz) to tame harshness.
      // One-pole LPF: y[n] = a*x[n] + (1-a)*y[n-1], a = 1-exp(-2*pi*fc/sr).
      // fc = 2000 Hz @ 48 kHz -> a = 1-exp(-2*pi*2000/48000) = 1-exp(-0.2618) = 0.2301.
      const float lpf_a = 0.5768f; // precomputed: 1-exp(-2*pi*5000/48000) (5 kHz cutoff: more "sparkle" in the wash)
      flanger_lpf_l_ = lpf_a * (wetsum_l * ninv) + (1.f - lpf_a) * flanger_lpf_l_;
      flanger_lpf_r_ = lpf_a * (wetsum_r * ninv) + (1.f - lpf_a) * flanger_lpf_r_;
      // out = dry + FLANGER * LPF(wet_mean)  (ninv already includes 0.5 normalization)
      const float y_l = L + fln_wet * flanger_lpf_l_;
      const float y_r = R + fln_wet * flanger_lpf_r_;
      L = y_l; R = y_r;
    }

    // ---- TAPWOB branch: tape wow/flutter pitch-mod (dry/wet replace) ----------
    // The LAST stage (after CHORUS/FLANGER), right before the makeup gain.
    // TAPWOB = 0 -> dry wire (the branch is never computed, regression-safe).
    //
    // TAPWOB uses the FIXED TIME / DEPTH knobs (like every other stage):
    //   * TIME  -> LFO tempo: the wow/flutter rates follow the same exp curve
    //     as CHORUS / FLANGER (kTapeWobWowMinHz..MaxHz / kTapeWobFlutMinHz..
    //     MaxHz), so the whole stack breathes together.
    //   * DEPTH -> modulation depth: WOW is scaled as DEPTH^3 (cubic, much
    //     shallower than the FLANGER's DEPTH^2) so low DEPTH stays a subtle
    //     pitch shimmer; FLUTTER is scaled as DEPTH^2 (12 cents is already tame).
    //   * TAPWOB -> DRY/WET level only (0 = off, 1 = full tape deck) and the
    //     LPF "dullness" (2..8 kHz).
    //
    // Architecture: one SHARED MONO delay line (kTapeWobLineLenSmp = 512 smp).
    // The mono input (L, == R in the chain) is written ONCE per sample at
    // tapwob_w_; the line head advances. L and R each read a tap offset by
    //   read = center + (wow_cents + flutter_cents) * kTapeWobCents2Smp
    // samples behind the write head, where wow/flutter are TWO independent
    // LFOs (sine, libm-free Taylor) advanced per sample. The cents->sample
    // conversion (kTapeWobCents2Smp = center/1200) is a constant, so the pitch
    // shift is exactly (wow+flutter) cents. A 1st-order LPF per channel
    // (alpha from the TAPWOB-swept 2..8 kHz cutoff) dulls the highs as the
    // wobble deepens. out = dry*(1-T) + wet*T, T = tapwob_wet (full wet at
    // TAPWOB=1).
    if (tapwob_wet > 0.f)
    {
      // Write the mono input once (the chain upmixes L==R; the device's stereo
      // input uses L as the "tape" source -- the right channel is re-derived
      // from the same line with a different LFO phase below).
      tapwob_delay_[tapwob_w_] = L;
      ++tapwob_w_;
      if (tapwob_w_ >= modfx_consts::kTapeWobLineLenSmp) tapwob_w_ = 0U;

      // Advance the two LFO phases (cycles). w = 2*pi*freq/sr.
      const float wow_w = 2.f * 3.14159265f * tapwob_wow_freq_ / modfx_consts::kSr;
      const float flut_w = 2.f * 3.14159265f * tapwob_flut_freq_ / modfx_consts::kSr;
      tapwob_wow_phase_ += wow_w;
      tapwob_flut_phase_ += flut_w;
      if (tapwob_wow_phase_ >= 1.f) tapwob_wow_phase_ -= 1.f;
      if (tapwob_flut_phase_ >= 1.f) tapwob_flut_phase_ -= 1.f;

      // LFO values (sine, Taylor) in [-1, 1]. R is the same two LFOs but the
      // flutter phase is offset by 0.25 (90 deg) so the L/R flutter is
      // decorrelated -- a stereo "tape" image.
      const float wow_l = lfoSine(tapwob_wow_phase_);
      const float flut_l = lfoSine(tapwob_flut_phase_);
      float flut_r_phase = tapwob_flut_phase_ + 0.25f;
      if (flut_r_phase >= 1.f) flut_r_phase -= 1.f;
      const float flut_r = lfoSine(flut_r_phase);

      // Total pitch offset per channel (sample units). The CENTS come from
      // the member fields set by the per-block DEPTH^2 (see tapwob_wow_cents_
      // / tapwob_flut_cents_ above); off_* is in SAMPLE units because
      // kTapeWobCents2Smp = center/1200 converts cents -> samples.
      const float off_l = tapwob_wow_cents_ * wow_l + tapwob_flut_cents_ * flut_l;
      const float off_r = tapwob_wow_cents_ * wow_l + tapwob_flut_cents_ * flut_r;

      // Read tap: center delay + LFO offset (off_* is already in SAMPLE units
      // because kTapeWobCents2Smp = center/1200 converts cents -> samples).
      // Clamp the total to the line (the offset is small, ~ +/-6 smp).
      float rd_l = modfx_consts::kTapeWobCenterSmp + off_l;
      float rd_r = modfx_consts::kTapeWobCenterSmp + off_r;
      if (rd_l < 1.f) rd_l = 1.f; else if (rd_l > (float)modfx_consts::kTapeWobLineLenSmp - 1.f) rd_l = (float)modfx_consts::kTapeWobLineLenSmp - 1.f;
      if (rd_r < 1.f) rd_r = 1.f; else if (rd_r > (float)modfx_consts::kTapeWobLineLenSmp - 1.f) rd_r = (float)modfx_consts::kTapeWobLineLenSmp - 1.f;

      const uint32_t len = modfx_consts::kTapeWobLineLenSmp;
      const uint32_t base_l = (tapwob_w_ + len - (uint32_t)rd_l) % len;
      const uint32_t base_r = (tapwob_w_ + len - (uint32_t)rd_r) % len;
      const float frac_l = rd_l - (float)(uint32_t)rd_l;
      const float frac_r = rd_r - (float)(uint32_t)rd_r;
      const uint32_t next_l = (base_l + 1U < len) ? (base_l + 1U) : 0U;
      const uint32_t next_r = (base_r + 1U < len) ? (base_r + 1U) : 0U;
      const float wet_l = tapwob_delay_[base_l] * (1.f - frac_l) + tapwob_delay_[next_l] * frac_l;
      const float wet_r = tapwob_delay_[base_r] * (1.f - frac_r) + tapwob_delay_[next_r] * frac_r;

      // 1st-order LPF per channel (the "old tape" color).
      tapwob_lpf_l_ = tapwob_lpf_alpha_ * wet_l + (1.f - tapwob_lpf_alpha_) * tapwob_lpf_l_;
      tapwob_lpf_r_ = tapwob_lpf_alpha_ * wet_r + (1.f - tapwob_lpf_alpha_) * tapwob_lpf_r_;

      // Dry/wet replace: out = dry*(1-T) + wet*T.
      L = L * (1.f - tapwob_wet) + tapwob_lpf_l_ * tapwob_wet;
      R = R * (1.f - tapwob_wet) + tapwob_lpf_r_ * tapwob_wet;
    }

    // ---- WARP: magnetic instability (KoS "Shame", dry/wet replace) ------------
    // The mono input (the current L, before CHORUS) is written once per sample to
    // a shared 4096-smp line; the read tap is (center + cosine-LFO * depth). After
    // each full LFO cycle the rate gets a random kick (fx_rand) so the wobble never
    // repeats. L/R read the same tap (mono instability); the stereo image is kept
    // by the other stages. WARP = 0 -> dry wire (regression-safe).
    if (warp_on)
    {
      // Write the mono input to the line (once per frame, shared mono).
      // `in` is interleaved L/R -- take the L channel (L==R in the web chain,
      // and the NTS-1 platform upmixes the OSC mono too, so L is the signal).
      warp_delay_[warp_w_] = in_l;
      ++warp_w_;
      if (warp_w_ >= modfx_consts::kWarpLineLenSmp) warp_w_ = 0U;
      // Advance the cosine LFO phase at (base rate + random fluctuation). The
      // fluctuation is re-drawn on each full cycle (KoS rateFluctuation), so the
      // wobble speed drifts randomly and never repeats.
      const float inst_rate = warp_rate_ + warp_rate_fluct_;
      warp_phase_ += inst_rate;
      if (warp_phase_ >= 1.f)
      {
        warp_phase_ -= 1.f;
        const float periodicity = modfx_consts::kWarpFluctMin -
            p.warp * (modfx_consts::kWarpFluctMin - modfx_consts::kWarpFluctMax);
        const float rnd = static_cast<float>(fx_rand() & 0x7FFFFFFFU) * (1.0f / 2147483648.0f);
        warp_rate_fluct_ = rnd * warp_rate_ * periodicity;
      }
      // Cosine LFO value in [-1, 1] (lfoSine on the phase).
      const float w = lfoSine(warp_phase_);
      // Read tap: center + LFO * depth (clamped to the line).
      float read_pos = modfx_consts::kWarpCenterSmp + w * warp_depth_;
      if (read_pos < 0.f) read_pos = 0.f;
      else if (read_pos > (float)(modfx_consts::kWarpLineLenSmp - 1U)) read_pos = (float)(modfx_consts::kWarpLineLenSmp - 1U);
      // The line is written newest-at-warp_w_; read_pos samples back is
      // (warp_w_ - read_pos), wrapped modulo the line length.
      const uint32_t len = modfx_consts::kWarpLineLenSmp;
      const uint32_t base = (warp_w_ + len - (uint32_t)read_pos) & (len - 1U);
      const float frac = read_pos - (float)(uint32_t)read_pos;
      const uint32_t i0 = (base + len - 1U) & (len - 1U);
      const uint32_t i1 = (i0 + 1U) & (len - 1U);
      const float wet = warp_delay_[i0] + frac * (warp_delay_[i1] - warp_delay_[i0]);
      // Dry/wet replace (T = WARP; the instability is a "re-tape" of the signal).
      const float wt = p.warp;
      L = L * (1.f - wt) + wet * wt;
      R = R * (1.f - wt) + wet * wt;
    }

    // ---- HISS: tape hiss (KoS Hiss, procedural, additive) ----------------------
    // fx_white -> 1-pole HPF (kHissHpfFc) -> a quiet level, summed to L/R.
    // HISS = 0 -> off (regression-safe). The HPF turns white into "hiss" color.
    // At HISS > 0 we also attenuate the dry signal by (1 - hiss*kHissDryDuck)
    // so the effect reads as "tape on the master bus" (the KoS Hiss class does
    // the same: signalLevel = 1 - hissLevel, dry = signalLevel * input +
    // hissLevel * hiss). Without the dry-attenuation the hiss reads as a
    // parallel additive noise layer, not as tape; with it, the drone ducks
    // slightly as the hiss rises and the whole thing reads as one "deck".
    if (hiss_level > 0.f)
    {
      const float hpf_a = 0.99790f; // precomputed exp(-2*pi*1500/48000)
      const float hpf = (1.f - hpf_a) * (fx_white() - hiss_hpf_y_);
      hiss_hpf_y_ = hpf_a * hiss_hpf_y_ + hpf;
      // Dry attenuation (KoS signalLevel = 1 - hissLevel, scaled by kHissDryDuck
      // so HISS=1 ducks the dry to 0.6, not to 0.0 -- the drone stays present).
      const float dry_scale = 1.f - p.hiss * modfx_consts::kHissDryDuck;
      L *= dry_scale;
      R *= dry_scale;
      L += hiss_level * hiss_hpf_y_;
      R += hiss_level * hiss_hpf_y_;
    }

    // ---- Makeup gain + write the stereo output -------------------------------
    // NO DC-block here: with REAL delay lines the stages no longer accumulate a
    // DC-leaning envelope, so a ~20 Hz one-pole HPF (tau ~2 s) would have eaten
    // the low-frequency DRONE signal for no benefit. kModMakeup is UNITY 1.0:
    // at all levels 0 the unit is a pure wire, so the output gain must be
    // unity or enabling the MOD slot would be an audible ~0.9 dB step (the
    // stage peaks are bounded by their own wet ceilings, not by a makeup).
    out[2 * n]     = L * modfx_consts::kModMakeup;
    out[2 * n + 1] = R * modfx_consts::kModMakeup;
    const float out_abs = (out[2 * n] < 0.f) ? -out[2 * n] : out[2 * n];
    const float out_abs2 = (out[2 * n + 1] < 0.f) ? -out[2 * n + 1] : out[2 * n + 1];
    if (out_abs > out_peak) out_peak = out_abs;
    if (out_abs2 > out_peak) out_peak = out_abs2;
  }

  // Publish the block peaks (diagnostic: chain.h exposes them via
  // getModInputPeak()/getModOutputPeak()).
  mod_input_peak_ = in_peak;
  mod_output_peak_ = out_peak;
}

  Params params_;
};
// (The per-sample path modfx_process_impl lives INLINE in this class above,
// so no separate modfx_dsp.h include is needed; the legacy modfx_dsp.h is
// dead code and not included anywhere.)
