// The game on the PS Vita (vita_game.c, VITA_PORT.md M2).
#ifndef CVF_VITA_GAME_H
#define CVF_VITA_GAME_H
#include "platform/gles/gles_thread.h"

// Runs the game in gamedir (stdout/stderr already redirected); returns the
// exit status.
int vita_game_run(const char *gamedir);
// One "mem" line in perf.log: collector, newlib, kernel and vitaGL memory.
void vita_game_mem_line(const char *tag);
#endif
