#ifndef VIDEO_SETTINGS_H
#define VIDEO_SETTINGS_H

#include <stdint.h>

/* Reads turok_settings.ini and hands the runtime its start-up values; before
 * anything reads RECOMP_RENDER_SCALE or RECOMP_VSYNC. */
void video_settings_load(void);

/* Writes the Video Options page into patch0.tre in the game's folder and
 * starts applying what the player picks on it. Before the title mounts its
 * archives (xbe_entry_point), after the memory layout. */
void video_settings_start(const char *game_dir);

/* The title entered a screen: the front end (shown 4:3 and at the console's
 * frame rate), the pause menu's first screen, or else the game. */
void video_settings_screen(int front_end, int pause_menu);

/* The screen loader's object and the file it is loading: a menu screen's
 * entries take the mouse (the menu pointer). */
void video_settings_menu_screen(uint32_t screen, const char *name);
void video_settings_draw_menu_widget(void);

/* An in-engine cutscene began (1) or ended (0): shown 4:3, as it was framed. */
void video_settings_cinema(int on);

/* A level is loading (1) or has begun (0): its loading screen is a 4:3
 * picture, shown 4:3 until the level's first frame. */
void video_settings_loading(int on);

/* The draw distance setting: a factor on actors' fade distances. */
float video_settings_draw_distance(void);
void video_settings_level(const char *path);
/* The level is one of the indoor ones (pods 06 and 10-12). */
int video_settings_indoor_level(void);
int video_settings_keep_decals(void);
float video_settings_wide_factor(void);

/* After the input mapper each frame: raw mouse look, when set. */
void video_settings_mouse_look(void);

/* The mouse's turn since last taken (0 yaw, 1 pitch), in stick units x
 * seconds: the look functions divide by their frame time. */
float video_settings_take_mouse_turn(int axis);
/* Flying: the mouse's steer, axis 0 right, 1 up, in stick deflection. */
float video_settings_take_mouse_steer(int axis);

/* On-foot controller look; Original remains the default. */
int video_settings_modern_aim(void);
int video_settings_ladder_freelook(void);
float video_settings_look_limit(void);
int video_settings_club_charge(void);
/* Audio output (0 auto, 1 stereo, 2 5.1, 3 7.1, 4 mono: the toolkit's XA2_OUTPUT_*),
 * read at start-up. */
int video_settings_audio_output(void);
/* LOD range: 1, 2 or 4 times the game's level-of-detail distances, or 1e6 (max). */
float video_settings_lod_factor(void);
/* The game language Game Options' Language row asks for (the table at
 * 0x2054D0: 1 American, 3 French, 4 German, 6 Spanish, 7 Italian), or 0
 * before Options.hst is read. */
int video_settings_language(void);
/* Owned weapons carry over between levels (off: as on the Xbox). */
int video_settings_keep_weapons(void);
#define VIDEO_WEAPON_SLOTS 10
/* One bit per configured weapon slot, in default key order 1–0. */
unsigned video_settings_weapon_input(void);

#endif
