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
 *  NTS-1 mkII modulation effect unit header definition
 *
 */

#include "unit_modfx.h"   // Note: Include base definitions for modfx units

// ---- Unit header definition  --------------------------------------------------------------------

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),                  // Size of this header. Leave as is.
    .target = UNIT_TARGET_PLATFORM | k_unit_module_modfx,  // Target platform and module pair for this unit
    .api = UNIT_API_VERSION,                               // API version for which unit was built. See runtime.h
    .dev_id = 0x6f6f627aU,                                 // Developer ID 'oobz' (same author as drone-osc). See https://github.com/korginc/logue-sdk/blob/master/developer_ids.md
    .unit_id = 0x00000002U,                                // ID for this unit (2nd unit of the author; 0x1 = drone-osc). Scoped within dev_id.
    .version = 0x00010010U,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch). Bumped minor: TWO PARAM/VALUE multiplexers (MOD group slots 2/3 + TAPE group slots 4/5) + TAPE stages (NOISE v2 granular rustle / TAPWOB / HISS / WARP, KoS-derived).
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 6,                                       // Number of valid parameter descriptors. (max. 10)

    .params = {
        // Format: min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure.
        //
        // Parameter model (the same split as drone-osc, with TWO multiplexers):
        //   TIME / DEPTH are the two FIXED knobs (slots 0/1) -- always on the
        //   A/B encoders, type_none (formatted as % by the platform / sim).
        //   The 10 other parameters split into two groups, each behind its own
        //   PARAM/VALUE multiplexer:
        //     MOD group (slots 2/3): DRIFT / CMODE / CHOR / FLNGR / ENSMBL
        //     TAPE group (slots 4/5): NOISE / NRATE / TAPWOB / HISS / WARP
        //   (the KoS-derived film/tape stage). unit.cc maps header slot
        //   2->PARAM_SEL, 3->PARAM_VAL, 4->PARAM_TSEL, 5->PARAM_TVAL
        //   (Modfx::setParameter only ever sees TIME/DEPTH/SEL/VAL/TSEL/TVAL).
        //   The 4 string slots show the selected parameter's NAME (PARAM) and
        //   FORMATTED value (VALUE, CMODE as I / I+II / II, everything else %)
        //   via unit_get_param_str_value -> Modfx::getParameterStrValue.

        // [0] TIME: LFO tempo (0..1, FIXED A knob). Sweeps the chorus / flanger /
        // ensemble LFO rate (0.0167..0.6 Hz, shared exp curve) and the noise
        // band-pass cutoff (300 Hz..8 kHz).
        {0, 1023, 0, 512, k_unit_param_type_none, 0, 0, 0, {"TIME"}},

        // [1] DEPTH: per-stage modulation depth (0..1, FIXED B knob). Scales the
        // chorus LFO delay-spread, the flanger center delay (DEPTH^2), the
        // ensemble LFO spread (DEPTH^2), and the tape wow/flutter depth.
        {0, 1023, 0, 511, k_unit_param_type_none, 0, 0, 0, {"DEPTH"}},

        // [2] PARAM: MOD-group multiplexer SELECTOR (0..4), the optional duplicate
        // of the A-knob role for the edit menu. The display shows the selected
        // MOD edit parameter's NAME (DRIFT / CMODE / CHOR / FLNGR / ENSMBL) via
        // getParameterStrValue. Default 0 = DRIFT.
        {0, 4, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},

        // [3] VALUE: raw 10-bit value (0..1023) of the selected MOD edit
        // parameter. The display shows the FORMATTED value (CMODE -> I / I+II /
        // II, everything else -> 0..100%) via getParameterStrValue. Default 0.
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},

        // [4] TPARAM: TAPE-group multiplexer SELECTOR (0..4), the second
        // multiplexer for the KoS-derived tape parameters. The display shows the
        // selected TAPE edit parameter's NAME (NOISE / NRATE / TAPWOB / HISS /
        // WARP) via getParameterStrValue. Default 0 = NOISE.
        {0, 4, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"TPARAM"}},

        // [5] TVALUE: raw 10-bit value (0..1023) of the selected TAPE edit
        // parameter. The display shows the FORMATTED value (0..100%) via
        // getParameterStrValue. Default 0.
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"TVALUE"}},
    },
};
