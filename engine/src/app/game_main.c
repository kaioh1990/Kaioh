// cvf: the game. Runs the translated Ikemen GO main() (window, renderer,
// Lua menus, input, audio) in the given game directory (default: the
// executable's working directory).
//   cvf [gamedir] [ikemen args...]
#include <stdio.h>
#include <string.h>
int chdir(const char *path);  // <unistd.h> clashes with generated names

#include "ik/ik_sim.h"

int main(int argc, char **argv) {
    gc_init(&argc);
    int first = 1;
    if (argc > 1 && argv[1][0] != '-') {
        if (chdir(argv[1]) != 0) {
            perror(argv[1]);
            return 2;
        }
        first = 2;
    }
    ik_init_globals();
    Slice_GoString args = {(GoString *)gc_alloc(sizeof(GoString) * (size_t)(argc + 1)), 0, argc + 1};
    args.p[args.len++] = GOSTR("cvf");
    for (int i = first; i < argc; i++) args.p[args.len++] = go_cstr(argv[i]);
    os_Args = args;
    fn_main();
    return 0;
}
