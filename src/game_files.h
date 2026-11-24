#ifndef GAME_FILES_H
#define GAME_FILES_H

#include <stddef.h>

/* The folder of the player's USA disc files (default.xbe, supertree*.tre)
 * into `dir`: found, extracted from a disc image, or asked for (see
 * game_files.c). `build_dir` is a folder to try before asking (the one the
 * build was made with); NULL for none. 0 if there is none (the player
 * cancelled). */
int game_files_locate(const char *build_dir, char *dir, size_t size);

/* Another release's disc image beside the program (the European one): its
 * Data\Video language folders that `dir` lacks are copied in, once. */
void game_files_movies(const char *dir);

#endif
