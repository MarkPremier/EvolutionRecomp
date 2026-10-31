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

/* An in-engine cutscene began (1) or ended (0): shown 4:3, as it was framed. */
void video_settings_cinema(int on);

/* A level is loading (1) or has begun (0): its loading screen is a 4:3
 * picture, shown 4:3 until the level's first frame. */
void video_settings_loading(int on);

/* The draw distance setting: a factor on actors' fade distances. */
float video_settings_draw_distance(void);
void video_settings_level(const char *path);
/* The level is one of the indoor ones (pods 05, 06 and 10-12). */
int video_settings_indoor_level(void);
int video_settings_keep_bodies(void);
int video_settings_keep_decals(void);
float video_settings_wide_factor(void);

/* After the input mapper each frame: raw mouse look, when set. */
void video_settings_mouse_look(void);

/* The mouse's turn since last taken (0 yaw, 1 pitch), in stick units x
 * seconds: the look functions divide by their frame time. */
float video_settings_take_mouse_turn(int axis);

#endif
