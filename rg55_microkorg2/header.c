/**
 * RGSC55 - microKORG2 oscillator unit
 *
 * The drumlogue RG55 unit as a microKORG2 oscillator: a monophonic libEmuSC
 * SC-55 player on one part. It needs the SC-55 ROM set in the userfs:
 *   /var/lib/microkorgd/userfs/Programs/sc55_{rom1,rom2,waverom1,waverom2,waverom3}.bin
 *
 * One parameter, because the keyboard supplies the note and its own level:
 *   0 PROGRAM - SC-55 instrument, shown by the control ROM's own name
 */

#include "unit.h"
#include "runtime.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target      = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api         = UNIT_API_VERSION,
    .dev_id      = 0x72677276U,
    .unit_id     = 0x08U,
    .version     = 0x00010000U,
    .name        = "RG-55",
    .num_presets = 0,
    .num_params  = 1,
    .params = {
        /* Strings because the display should show the instrument's name, not
         * its number. */
        {0, 127, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PROGRAM"}},
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
