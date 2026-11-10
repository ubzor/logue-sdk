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
    .version = 0x00010001U,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch). Bumped patch: real author IDs + name.
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 10,                                      // Number of valid parameter descriptors. (max. 10)

    .params = {
        // Format: min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure

        // [0] TIME: LFO tempo ceiling (0..1) — drives ENSE/TREMO/FLNG LFO rate.
        {0, 1023, 0, 256, k_unit_param_type_none, 1, 0, 0, {"TIME"}},

        // [1] DEPTH: per-stage modulation depth (0..1) — flanger delay, chorus
        // spread, tape flutter, tremolo depth.
        {0, 1023, 0, 511, k_unit_param_type_none, 1, 0, 0, {"DEPTH"}},

        // [2] DRIFT: shared self-motion ceiling (0..1, 0 = static). The 7 level
        // stages wander on their own, constrained by an L1 budget.
        {0, 1023, 0, 0, k_unit_param_type_none, 1, 0, 0, {"DRIFT"}},

        // [3] ENSE: stereo chorus level (0..1). 0 = wire.
        {0, 1023, 0, 1023, k_unit_param_type_none, 1, 0, 0, {"ENSE"}},

        // [4] TREMO: harmonic/optical tremolo level (0..1). 0 = wire.
        {0, 1023, 0, 307, k_unit_param_type_none, 1, 0, 0, {"TREMO"}},

        // [5] FLNG: thru-zero flanger level (0..1). 0 = off.
        {0, 1023, 0, 0, k_unit_param_type_none, 1, 0, 0, {"FLNG"}},

        // [6] TAPE: tape wow+flutter level (0..1). 0 = off.
        {0, 1023, 0, 0, k_unit_param_type_none, 1, 0, 0, {"TAPE"}},

        // [7] FORM: formant vowel position (0..1, morph A-E-I-O-U). strings.
        {0, 1023, 0, 511, k_unit_param_type_strings, 1, 0, 0, {"FORM"}},

        // [8] FLVL: formant stage level (0..1, 0 = dry/transparent).
        {0, 1023, 0, 0, k_unit_param_type_none, 1, 0, 0, {"FLVL"}},

        // [9] COMB: comb resonator feedback (0..1 -> 0..0.85). 0 = off.
        {0, 1023, 0, 0, k_unit_param_type_none, 1, 0, 0, {"COMB"}},
    },
};
