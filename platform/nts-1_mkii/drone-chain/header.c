/*
 *  File: header.c
 *
 *  Unit header for the web-simulator "drone-chain" module (OSC -> MOD -> DEL
 *  -> REV -> out, run as a single wasm audio worklet; see chain.h).
 *
 *  This header is WEB-ONLY: it exists so the worklet can build its parameter
 *  descriptors (getValidParameters, see wasm.cc) the same way every other unit
 *  does, and so the module keeps the standard unit_header_t shape. The web
 *  simulator does NOT load this as an ARM unit, so the target is set to the
 *  oscillator module (the chain is an extension of the drone OSC) and the
 *  parameter table mirrors drone-osc's six slots (PARAM / VALUE multiplexer +
 *  RndM / Rnd). The UI talks to the DSP directly through the embind bindings
 *  (setEditParam / setFxParam, see wasm.cc), not through these slots, so their
 *  exact ranges only matter for the worklet's parameter descriptor shape.
 */

#include "unit_osc.h" // base definitions for osc units (unit_header_t, param types)

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api = UNIT_API_VERSION,
    .dev_id = 0x6f6f627aU, // 'oobz' (drone author id, same as drone-osc)
    .unit_id = 0x00000002U, // chain is the 2nd unit in the author scope (sim only)
    .version = 0x00010000U,
    .name = "DRONE",
    .num_params = 6,
    .params = {
        // PARAM selector (A knob / UI edit-parameter index), 0..43.
        {0, 43, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},
        // VALUE (raw 0..1023 of the selected parameter).
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},
        // Optional (edit-menu) duplicates of PARAM / VALUE.
        {0, 43, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PARAM"}},
        {0, 1023, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"VALUE"}},
        // RndM (randomization type selector, 0..2).
        {0, 2, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"RndM"}},
        // Rnd (randomization trigger, 0..15).
        {0, 15, 0, 0, k_unit_param_type_enum, 0, 0, 0, {"Rnd"}},
    },
};
