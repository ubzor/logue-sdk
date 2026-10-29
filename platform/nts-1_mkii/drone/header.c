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
    .version = 0x00000006U,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch).
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 4,                                       // Number of valid parameter descriptors. (max. 10)
    .params = {
        // Format:
        // min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure

        // ---- Fixed/direct UI parameters (hardwired to knobs A and B) ----
        // These two are no-op stubs: turning knob A or knob B in the default
        // mode does nothing. All real control happens through the optional
        // (edit-menu) parameters below. They are type_none so the display
        // shows only the static label and never calls
        // unit_get_param_str_value for them.
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {"PARAM"}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {"VALUE"}},

        // ---- Optional (edit-menu) parameters: the A/B multiplexer ----
        // PARAM SEL: which of the 10 DSP parameters the optional value edits
        // (0..9: DETU, RATE, SPEE, SUB, SUB2, SATU, FOLD, ASYM, WAV1, WAV2).
        // type_strings: in edit mode the display shows the selected
        // parameter's name via unit_get_param_str_value.
        {0, 9, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

        // PARAM VAL: raw 10-bit value (0..1023) of the selected parameter.
        // Each selected parameter maps 0..1023 onto its own range (see
        // apply_edit_param in osc.h). The raw value is cached per-parameter
        // so re-selecting a parameter restores its last value. type_strings:
        // in edit mode the display shows the formatted value (e.g. "50%").
        //
        // Selectable parameters (PARAM SEL value -> name -> default raw value):
        //   0: DETU - detune of osc B (-50..+50 cents), default 511 (0 cents)
        //   1: RATE - random-walk depth (quadratic, 0..100%), default 0 (off)
        //   2: SPEE - random-walk speed (0..100%), default 511 (50%)
        //   3: SUB  - sub-octave (f/2) sine level (0..100%), default 511 (50%)
        //   4: SUB2 - second sub (f/4) saturated level (0..100%), default 0 (off)
        //   5: SATU - saturator drive (0..300), default 682 (200 = 2.0x)
        //   6: FOLD - pre-drive wavefolder depth (0..100%), default 0 (off)
        //   7: ASYM - asymmetric saturator (-100%..+100%), default 511 (0%)
        //   8: WAV1 - osc A wavetable (0..90 across banks A-F), default 0
        //   9: WAV2 - osc B wavetable (0..90 across banks A-F), default 0
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},
    },
};
