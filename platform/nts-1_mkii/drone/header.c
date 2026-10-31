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
    .version = 0x0000000AU,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch).
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 4,                                       // Number of valid parameter descriptors. (max. 10)
    .params = {
        // Format:
        // min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure

        // ---- Fixed/direct UI parameters (slots 0/1) ----
        // A knob (PARAM, slot 0): parameter selector, 0..15. The display
        // shows the name of the selected parameter via unit_get_param_str_value().
        // Turning the B knob (VALUE, slot 1) then edits that parameter's raw value.
        //
        // Selectable parameters (A knob value -> name -> default raw value):
        //   0: DETU - static symmetric detune of A/B (-10..+10 cents, A=+c/2, B=-c/2), default 511 (0 c)
        //   1: RATE - random-walk depth (quadratic, 0..100%), default 0 (off)
        //   2: SPEE - random-walk period (0..100% -> 0.1 s..10 s, log), default 511
        //   3: SMTH - random-walk time constant (0..100% -> 10 ms..1.5 s, log), default 511
        //   4: SUB1 - sub-octave (f/2) sine level (0..100%), default 511 (50%)
        //   5: SUB2 - second sub (f/4) saturated level (0..100%), default 205 (~20%)
        //   6: SATU - saturator drive (0..300), default 341 (100 = 1.0x, gentle)
        //   7: ASYM - asymmetric saturator (-100%..+100%), default 511 (0%)
        //   8: FOLD - pre-drive wavefolder depth (0..100%), default 0 (off)
        //   9: WAV1 - osc A wavetable (0..90 across banks A-F), default 0
        //   10: WAV2 - osc B wavetable (0..90 across banks A-F), default 128 (~11)
        //   11: CHRD - chord-voice level (0..100%), default 205 (~20%)
        //   12: CINT - chord interval (0..1900 cents, continuous), default 377 (~700c, fifth)
        //   13: RWAV - random-walk depth on wavetable morph (0..100%), default 0 (off)
        //   14: RDRV - random-walk depth on saturator drive (0..100%), default 0 (off)
        //   15: RFLD - random-walk depth on wavefolder depth (0..100%), default 0 (off)
        //   16: XFM  - cross-FM depth on osc A (B modulates A, 0..15 c), default 0 (off)
        //   17: XFM2 - cross-FM depth on osc B (A modulates B, 0..15 c), default 0 (off)
        //   18: FCUT - internal SVF cutoff + dry/wet (80 Hz..12 kHz, 0% = raw), default 921
        //   19: RES  - internal SVF resonance Q (0.5..2.5), default 0 (0.5)
        //   20: RWFC - random-walk depth on SVF cutoff (0..100%), default 0 (off)
        {0, 20, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

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
        // Slot 2 (PARAM): parameter selector, 0..20. Identical to slot 0.
        {0, 20, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

        // Slot 3 (VALUE): raw 10-bit value (0..1023) of the selected parameter.
        // Identical to slot 1.
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},
    },
};
