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
    if (first == 1) {
        // macOS app bundle: the game data sits next to the .app (as with
        // Ikemen's bundles); start there
        Ret_GoString__GoIface exe = os_Executable();
        const char *app = exe.r1.type ? NULL : strstr(go_strz(exe.r0), ".app/Contents/MacOS/");
        if (app) {
            char dir[4096];
            const char *z = go_strz(exe.r0);
            app = strstr(z, ".app/Contents/MacOS/");
            size_t n = (size_t)(app - z);
            while (n > 0 && z[n - 1] != '/') n--;  // folder containing X.app
            if (n > 0 && n < sizeof dir) {
                memcpy(dir, z, n);
                dir[n] = 0;
                if (chdir(dir) != 0) perror(dir);
            }
        }
    }
    ik_init_globals();
    Slice_GoString args = {(GoString *)gc_alloc(sizeof(GoString) * (size_t)(argc + 1)), 0, argc + 1};
    args.p[args.len++] = GOSTR("cvf");
    for (int i = first; i < argc; i++) args.p[args.len++] = go_cstr(argv[i]);
    os_Args = args;
    fn_main();
    return 0;
}
