// cvf_headless: runs a quick match without window or audio.
//   cvf_headless <gamedir> -p1 A -p2 B [-p1.ai N -p2.ai N] -s stage [-rounds N]
// With <gamedir>/vita_statedump.txt present, writes the per-tick state dump
// (statedump.txt) used to verify the engine against Ikemen GO.
#include <stdio.h>
int chdir(const char *path);  // <unistd.h> clashes with generated names

#include "ik/ik_sim.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <gamedir> [ikemen args...]\n", argv[0]);
        return 2;
    }
    if (chdir(argv[1]) != 0) {
        perror(argv[1]);
        return 2;
    }
    ik_init_globals();
    Slice_GoString args = {(GoString *)malloc(sizeof(GoString) * (size_t)(argc > 2 ? argc - 2 : 1)), 0, argc - 2};
    for (int i = 2; i < argc; i++) args.p[args.len++] = go_cstr(argv[i]);
    i32 w = fn_cvfHeadlessMain(args);
    printf("winner: %d\n", w);
    return w < 0 ? 1 : 0;
}
