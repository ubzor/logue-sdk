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
    .version = 0x00010009U,                                // This unit's version: major.minor.patch (major<<16 minor<<8 patch). Bumped patch: NOISE/NRATE slots (granular noise-sweep branch: LCG white noise, exponential burst cadence, DRIFT-AM, 2-pole SVF band-pass swept by TIME).
    .name = "DRONE",                                       // Name for this unit, will be displayed on device
    .num_params = 8,                                       // Number of valid parameter descriptors. (max. 10)

    .params = {
        // Format: min, max, center (unused), default, type, frac. bits, frac. mode, <reserved>, name

        // See common/runtime.h for type enum and unit_param_t structure

        // [0] TIME: LFO tempo (0..1) — sweeps ENSE chorus LFO rate (0.05..~4.5 Hz)
        // exponentially; also drives FLANGER LFO rate.
        {0, 1023, 0, 512, k_unit_param_type_none, 0, 0, 0, {"TIME"}},

        // [1] DEPTH: per-stage modulation depth (0..1) — chorus LFO spread,
        // flanger delay, tape flutter.
        {0, 1023, 0, 511, k_unit_param_type_none, 0, 0, 0, {"DEPTH"}},

        // [2] DRIFT: shared self-motion ceiling (0..1, 0 = static). The 4 level
        // stages wander on their own, constrained by an L1 budget.
        {0, 1023, 0, 0, k_unit_param_type_none, 0, 0, 0, {"DRIFT"}},

        // [3] CMODE: chorus mode — 3 equal bands -> Juno voice blend.
        //   band 0 = I (main), band 1 = I+II (classic Juno), band 2 = II (sub).
        {0, 1023, 0, 512, k_unit_param_type_none, 0, 0, 0, {"CMODE"}},

        // [4] CHORUS: dry/wet level of the chorus (0..1). 0 = dry only, 1 = full chorus.
        {0, 1023, 0, 1023, k_unit_param_type_none, 0, 0, 0, {"CHORUS"}},

        // [5] FLANGER: thru-zero flanger level (0..1). 0 = off.
        {0, 1023, 0, 0, k_unit_param_type_none, 0, 0, 0, {"FLANGER"}},

        // [6] NOISE: granular noise-sweep branch level (0..1). 0 = off (dry
        // wire, default -- regression-safe). >0 adds an LCG white-noise
        // "metallic rustle / dust-flash" generator on the path input: burst-
        // gated (NRATE cadence), AM-modulated by the DRIFT walk, band-passed
        // by a 2-pole SVF (cutoff swept by TIME), summed additively before the
        // ENSE chorus.
        {0, 1023, 0, 0, k_unit_param_type_none, 0, 0, 0, {"NOISE"}},

        // [7] NRATE: noise burst cadence (0..1) -> average flashes/second
        // 0.15..8 (exp curve). 0 = rare long-lull flashes, 1 = dense shimmer.
        // Only audible when NOISE > 0.
        {0, 1023, 0, 511, k_unit_param_type_none, 0, 0, 0, {"NRATE"}},
    },
};
