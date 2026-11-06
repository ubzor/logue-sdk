/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, KORG INC. All rights reserved.
 * Full license text: LICENSE in the repository root.
 */
/*
 *  File: header.c
 *
 *  NTS-1 mkII oscillator unit header definition
 *
 */

#include "unit_osc.h"   // Note: Include base definitions for osc units

// ---- Unit header definition  --------------------------------------------------------------------

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),                  // Size of this header. Leave as is.
    .target = UNIT_TARGET_PLATFORM | k_unit_module_osc,    // Target platform and module pair for this unit
    .api = UNIT_API_VERSION,                               // API version for which unit was built. See runtime.h
    .dev_id = 0x6f6f627aU,                                 // Developer ID 'oobz' as displayed on device. See https://github.com/korginc/logue-sdk/blob/master/developer_ids.md
    .unit_id = 0x00000001U,                                // ID for this unit (1st unit of the author). Scoped within the context of a given dev_id.
    .version = 0x0000000BU,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch).
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 6,                                       // Number of valid parameter descriptors. (max. 10)
    .params = {
        // Format:
        // min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure

        // ---- Fixed/direct UI parameters (slots 0/1) ----
        // A knob (PARAM, slot 0): parameter selector, 0..38. The display
        // shows the name of the selected parameter via unit_get_param_str_value().
        // Turning the B knob (VALUE, slot 1) then edits that parameter's raw value.
        //
        // Selectable parameters (A knob value -> name -> default raw value):
        //   0: DETU - static symmetric detune of A/B (-10..+10 c, A=+c/2, B=-c/2), default 511 (0 c)
        //   1: RATE - shared random-walk depth ceiling (quadratic, 0..100%), default 0 (off)
        //   2: SPD1 - Tone stream period (0..100% -> 0.1 s..10 s, log), default 511
        //   3: SMT1 - Tone stream time constant (0..100% -> 10 ms..1.5 s, log), default 511
        //   4: SPD2 - Timbre stream period (log), default 511
        //   5: SMT2 - Timbre stream time constant (log), default 511
        //   6: SPD3 - Density stream period (log), default 511
        //   7: SMT3 - Density stream time constant (log), default 511
        //   8: SUB1 - sub-octave (f/2) sine level (0..100%), default 511 (50%)
        //   9: SUB2 - second sub (f/4) saturated level (0..100%), default 205 (~20%)
        //   10: SATU - saturator drive (0..300), default 341 (100 = 1.0x, gentle)
        //   11: ASYM - asymmetric saturator (-100%..+100%), default 511 (0%)
        //   12: FOLD - pre-drive wavefolder depth (0..100%), default 0 (off)
        //   13: WAV1 - osc A wavetable (0..90 across banks A-F), default 0
        //   14: WAV2 - osc B wavetable (0..90 across banks A-F), default 128 (~11)
        //   15: WAV3 - osc C (chord) wavetable (0..90 across banks A-F), default 0
        //   16: CHRD - chord-voice level (0..100%), default 205 (~20%)
        //   17: CINT - chord interval (0..1900 cents, continuous), default 377 (~700c, fifth)
        //   18: MAIN - main (A+B) voice level (0..100%), default 1023 (100%)
        //   19: RWAV - random-walk depth on wavetable morph (0..100%), default 0 (off)
        //   20: RDRV - random-walk depth on saturator drive (0..100%), default 0 (off)
        //   21: RFLD - random-walk depth on wavefolder depth (0..100%), default 0 (off)
        //   22: FCUT - internal SVF cutoff + dry/wet (80 Hz..12 kHz, 0% = raw), default 921
        //   23: RESO - internal SVF resonance Q (0.5..2.5), default 0 (0.5)
        //   24: RWFC - random-walk depth on SVF cutoff (0..100%), default 0 (off)
        //   25: RVLL - breath (voice-level wander) depth (0..100%), default 0 (off)
        //   26: RVLS - breath stream period (log), default 511
        //   27: RVLS - breath stream time constant (log), default 511
        //   28: SATC - chord-voice saturator drive (0..300), default 341
        //   29: FLDC - chord-voice wavefolder depth (0..100%), default 0 (off)
        //   30: INT2 - second chord interval (0..1900 c, continuous), default 511 (~950 c, M6)
        //   31: JIT  - random phase jitter on A/B (0..100% -> 0..0.5% cycle peak), default 0 (off)
        //   32: RMOD - A*B ring-mod parallel voice (0..100%), default 0 (off)
        //   33: FM1  - STATIC phase-FM on osc A by B (0..100% -> 0..0.2 c peak), default 0 (off)
        //   34: FM2  - STATIC phase-FM on osc B by A (0..100% -> 0..0.2 c peak), default 0 (off)
        //   35: RFMA - random-walk depth on FM A index (0..100%, RATE-gated), default 0 (pure static)
        //   36: RFMB - random-walk depth on FM B index (0..100%, RATE-gated), default 0 (pure static)
        {0, 38, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

        // B knob (VALUE, slot 1): raw 10-bit value (0..1023) of the selected
        // parameter. Each selected parameter maps 0..1023 onto its own range
        // (see apply_edit_param in osc.h). The raw value is cached per-parameter
        // so re-selecting a parameter restores its last value. Displayed as a
        // formatted string via unit_get_param_str_value (e.g. "50%").
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},

        // ---- Optional (edit-menu) duplicates of slots 0/1 ----
        // These are FULL duplicates of the fixed A/B knobs: same range, type,
        // and behavior. unit_set_param_value remaps slot 2 -> PARAM_SEL and
        // slot 3 -> PARAM_VAL before forwarding to the processor, so all four
        // slots read and write the same underlying state (select_ and
        // edit_value_[select_]). This exposes the multiplexer to the
        // edit-menu (Kontrol Editor / loguecli) where fixed slots are not
        // visible, and lets the web simulator render the PARAM/VALUE pair
        // through the standard edit-parameter UI.
        //
        // Slot 2 (PARAM): parameter selector, 0..38. Identical to slot 0.
        {0, 38, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

        // Slot 3 (VALUE): raw 10-bit value (0..1023) of the selected parameter.
        // Identical to slot 1.
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},

        // Slot 4 (Rnd): RANDOMIZE TRIGGER. This is NOT an edit parameter with
        // its own DSP state -- it is a one-shot command. Changing the value to
        // ANY of 0..15 makes the processor re-roll every edit parameter to a
        // fresh audition-friendly random preset (the same windows the web
        // simulator's "Random" button uses). It is type_enum so the platform
        // renders the integer natively (1..16) with no custom display code;
        // the value itself carries no sound, only the side-effect. Optional
        // (edit-menu) slot, reachable via Kontrol Editor / loguecli / the web
        // simulator (the device's two fixed knobs are the PARAM/VALUE
        // multiplexer, so Rnd is a menu control, not a physical knob).
        //   0 = never (default, no randomization until changed)
        //   1..16 = fire on any change to this value.
        {0, 15, 0, 0, k_unit_param_type_enum, 0, 0, 0, {"Rnd"}},

        // Slot 5 (Legato): key-release behavior switch. type_onoff so the
        // platform renders it as a 0/1 toggle natively (no custom display code;
        // unit_get_param_str_value returns "" like Rnd). The raw value maps:
        //   0..511   = OFF (default): key release starts the 2 s release.
        //   512..1023 = ON (legato): key release does NOT stop the sound;
        //              pressing the same key again fades it out; pressing a
        //              different key retargets the note (2 s pitch glide) and
        //              it keeps playing.
        // Delivered to the DSP through unit_set_param_value(id=5) ->
        // Osc::setLegato(value). Optional (edit-menu) slot, like Rnd.
        {0, 1023, 0, 0, k_unit_param_type_onoff, 0, 0, 0, {"Legato"}},
    },
};
