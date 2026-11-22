#include <assert.h>
#include <stdio.h>
#include <string.h>
static int s_front_end = 1, s_pause_menu, s_cinema, s_loading, dlaa_allowed = -1;
static int shadow_indoor;
static void nv2a_gpu_set_shadow_indoor(int indoor) { shadow_indoor = indoor; }
static void nv2a_gpu_allow_dlaa(int allowed) { dlaa_allowed = allowed; }
static void apply_aspect(void) {}
static void apply_frames(void) {}
#include "video_settings_under_test.inc"

int main(void)
{
    assert(k_settings[SET_HEAT_HAZE].count == 4);
    assert(k_settings[SET_HEAT_HAZE].fallback == 2);
    assert(value_index(SET_HEAT_HAZE, "off") == 0 && k_haze_modes[0] == 3);
    assert(value_index(SET_HEAT_HAZE, "Xbox") == 1 && k_haze_modes[1] == 0);
    assert(value_index(SET_HEAT_HAZE, "Enhanced") == 2 && k_haze_modes[2] == 2);
    assert(value_index(SET_HEAT_HAZE, "GameCube") == 3 && k_haze_modes[3] == 4);
    assert(in_game_text(SET_HEAT_HAZE));
    assert(k_settings[SET_DLAA_WIDTH].count == 2 && k_settings[SET_DLAA_WIDTH].fallback == 0);
    assert(value_index(SET_DLAA_WIDTH, "double") == 0 && value_index(SET_DLAA_WIDTH, "Screen") == 1);
    assert(in_game_text(SET_DLAA_WIDTH));
    assert(value_index(SET_HEAT_HAZE, "original") == 1);
    assert(value_index(SET_HEAT_HAZE, "smooth") == 2);
    assert(value_index(SET_HEAT_HAZE, "sharp") == 2);
    assert(value_index(SET_HEAT_HAZE, "invalid") == -1);
    puts("PASS: Off/Xbox/Enhanced/GameCube modes retain the correct rendering paths and migrate saved legacy names");
    assert(k_settings[SET_SHADOWS].count == 3 && k_settings[SET_SHADOWS].fallback == 1);
    assert(value_index(SET_SHADOWS, "Xbox") == 0);
    assert(value_index(SET_SHADOWS, "Enhanced") == 1);
    assert(value_index(SET_SHADOWS, "Enhanced+") == 2);
    for (int setting = 0; setting < SETTING_COUNT; setting++) {
        if (!in_game_text(setting)) continue;
        for (int other = 0; other < SETTING_COUNT; other++) {
            if (other == setting || !in_game_text(other)) continue;
            int key = text_label(other);
            assert(key < text_label(setting) || key >= text_values(setting) + k_settings[setting].count);
        }
    }
    video_settings_level("D:\\Data\\Levels\\Pod-12-S-City-Defend-Escape\\sc-12-a.atr");
    assert(shadow_indoor);
    video_settings_level("D:\\Data\\Levels\\Pod-01-Through-The-Jungle\\pod01_03_ttjungle.atr");
    assert(!shadow_indoor);
    puts("PASS: Enhanced+ has a distinct saved value and menu text, and indoor state follows level changes");
    assert(k_settings[SET_CONTROLLER_AIM].fallback == 0);
    assert(k_settings[SET_CONTROLLER_AIM].count == 2);
    assert(value_index(SET_CONTROLLER_AIM, "original") == 0);
    assert(value_index(SET_CONTROLLER_AIM, "Modern") == 1);
    assert(value_index(SET_CONTROLLER_AIM, "invalid") == -1);
    assert(in_game_text(SET_CONTROLLER_AIM));
    assert(text_label(SET_CONTROLLER_AIM) > text_values(SET_SHADOWS) + 1);
    assert(text_values(SET_CONTROLLER_AIM) + 1 < 188);
    puts("PASS: controller aiming defaults to Original, supports Modern, and has distinct visible menu text");
    apply_dlaa_scene();
    assert(!dlaa_allowed);
    video_settings_loading(1);
    video_settings_screen(0, 0);
    assert(!dlaa_allowed);
    video_settings_loading(0);
    assert(dlaa_allowed);
    video_settings_cinema(1);
    assert(!dlaa_allowed);
    video_settings_cinema(0);
    assert(dlaa_allowed);
    video_settings_screen(1, 0);
    assert(!dlaa_allowed);
    puts("PASS: DLAA is suspended in the front end, loading screens and cutscenes and resumes in gameplay");
    return 0;
}
