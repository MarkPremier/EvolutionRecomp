/* Console defaults for the native CPU renderer. The enhanced video menu
 * configures the fork's D3D11 renderer and is enabled with that backend. */
#include "video_settings.h"

void video_settings_load(void) {}
void video_settings_start(const char *game_dir) { (void)game_dir; }
/* The keyboard's menu bindings (RECOMP_KBM_MENU_MAP: arrows navigate, Escape
 * is Back) in the front end, its play bindings in a level and on the pause
 * menu (Escape resumes) -- as video_settings.c's follow_menus sets them. */
void video_settings_screen(int front_end, int pause_menu)
{
    extern void xbox_KbmSetMenuMode(int on);
    xbox_KbmSetMenuMode(front_end && !pause_menu);
}
void video_settings_menu_screen(uint32_t screen, const char *name) { (void)screen; (void)name; }
/* This is a guest function override, so it must execute the original body
 * (including its guest return) even without the enhanced menu. */
extern void sub_0004E1A0(void);
void video_settings_draw_menu_widget(void) { sub_0004E1A0(); }
void video_settings_cinema(int on) { (void)on; }
void video_settings_loading(int on) { (void)on; }

float video_settings_draw_distance(void) { return 1.0f; }
void video_settings_level(const char *path) { (void)path; }
int video_settings_indoor_level(void) { return 0; }
int video_settings_keep_decals(void) { return 0; }
float video_settings_wide_factor(void) { return 1.0f; }

void video_settings_mouse_look(void) {}
float video_settings_take_mouse_turn(int axis) { (void)axis; return 0.0f; }
float video_settings_take_mouse_steer(int axis) { (void)axis; return 0.0f; }
int video_settings_modern_aim(void) { return 0; }
int video_settings_ladder_freelook(void) { return 0; }
float video_settings_look_limit(void) { return 70.0f; }
int video_settings_club_charge(void) { return 0; }
int video_settings_audio_output(void) { return 1; }
float video_settings_lod_factor(void) { return 1.0f; }
int video_settings_language(void) { return 0; }
int video_settings_keep_weapons(void) { return 0; }
unsigned video_settings_weapon_input(void) { return 0; }
int video_settings_enhanced_shadows(void) { return 0; }
