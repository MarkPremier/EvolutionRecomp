#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "video_settings_under_test.inc"

int main(void)
{
    assert(k_settings[SET_HEAT_HAZE].count == 3);
    assert(k_settings[SET_HEAT_HAZE].fallback == 2);
    assert(value_index(SET_HEAT_HAZE, "off") == 0 && k_haze_modes[0] == 3);
    assert(value_index(SET_HEAT_HAZE, "Xbox") == 1 && k_haze_modes[1] == 0);
    assert(value_index(SET_HEAT_HAZE, "Enhanced") == 2 && k_haze_modes[2] == 2);
    assert(value_index(SET_HEAT_HAZE, "original") == 1);
    assert(value_index(SET_HEAT_HAZE, "smooth") == 2);
    assert(value_index(SET_HEAT_HAZE, "sharp") == 2);
    assert(value_index(SET_HEAT_HAZE, "invalid") == -1);
    puts("PASS: Off/Xbox/Enhanced modes retain the correct rendering paths and migrate saved legacy names");
    return 0;
}
