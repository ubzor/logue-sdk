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
    .version = 0x00000009U,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch).
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 4,                                       // Number of valid parameter descriptors. (max. 10)
    .params = {
        // Format:
        // min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure

        // ---- Fixed/direct UI parameters (slots 0/1) ----
        // A knob (PARAM, slot 0): parameter selector, 0..10. The display
        // shows the name of the selected parameter via unit_get_param_str_value().
        // Turning the B knob (VALUE, slot 1) then edits that parameter's raw value.
        //
        // Selectable parameters (A knob value -> name -> default raw value):
        //   0: DET1 - detune of osc B (-100..+100 cents), default 511 (0 cents)
        //   1: DET2 - detune of osc A (-100..+100 cents), default 511 (0 cents)
        //   2: RATE - random-walk depth (quadratic, 0..100%), default 0 (off)
        //   3: SPEE - random-walk speed (0..100%), default 511 (50%)
        //   4: SUB1 - sub-octave (f/2) sine level (0..100%), default 511 (50%)
        //   5: SUB2 - second sub (f/4) saturated level (0..100%), default 0 (off)
        //   6: SATU - saturator drive (0..300), default 682 (200 = 2.0x)
        //   7: ASYM - asymmetric saturator (-100%..+100%), default 511 (0%)
        //   8: FOLD - pre-drive wavefolder depth (0..100%), default 0 (off)
        //   9: WAV1 - osc A wavetable (0..90 across banks A-F), default 0
        //   10: WAV2 - osc B wavetable (0..90 across banks A-F), default 0
        {0, 10, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

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
        // Slot 2 (PARAM): parameter selector, 0..10. Identical to slot 0.
        {0, 10, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

        // Slot 3 (VALUE): raw 10-bit value (0..1023) of the selected parameter.
        // Identical to slot 1.
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},
    },
};
