#ifndef VIDEO_SETTINGS_H
#define VIDEO_SETTINGS_H

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

/* An in-engine cutscene began (1) or ended (0): shown 4:3, as it was framed. */
void video_settings_cinema(int on);

/* After the input mapper each frame: raw mouse look, when set. */
void video_settings_mouse_look(void);

#endif
