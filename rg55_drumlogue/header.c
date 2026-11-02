/**
 * RGSC55 - drumlogue synth unit
 *
 * A monophonic libEmuSC SC-55 player on one part. It needs the SC-55 ROM set in
 * the userfs:
 *   /var/lib/drumlogued/userfs/Programs/sc55_{rom1,rom2,waverom1,waverom2,waverom3}.bin
 *
 * Three parameters:
 *   0 PROGRAM - SC-55 instrument, shown by the control ROM's own name
 *   1 LEVEL   - output level, as a percentage
 *   2 NOTE    - the note a gate plays
 */

#include "unit.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target      = UNIT_TARGET_PLATFORM | k_unit_module_synth,
    .api         = UNIT_API_VERSION,
    .dev_id      = 0x72677276U,
    .unit_id     = 0x08U,
    .version     = 0x00010000U,
    .name        = "RG-55",
    .num_presets = 0,
    .num_params  = 3,
    .params = {
        /* Values are k_unit_param_type_strings because what the display should
         * show is the instrument's name, not its number. */
        {0, 127, 0, 0,  k_unit_param_type_strings,   0, 0, 0, {"PROGRAM"}},
        {0, 100, 0, 80, k_unit_param_type_percent,   0, 0, 0, {"LEVEL"}},
        {0, 127, 60, 60, k_unit_param_type_midi_note, 0, 0, 0, {"NOTE"}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
    }
};
