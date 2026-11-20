// cvf_nearby_test: the translated Nearby code end to end, through its Lua
// bindings, on this machine: Host Nearby listens on UDP 7520, Join Nearby
// probes the local subnets plus 127.0.0.1:7520 (the loopback target is the
// test hook cvf_nearby.go keeps in cvfNearbyState.extra), and the host must
// show up in cvfNearbyHosts() with its port and build tag. It also checks
// the desktop answers of the touch / mobile / name-box / P2P bindings that
// main.lua and start.lua call (cvf_p2p_other.go).
//
//   cvf_nearby_test <dir> [both | host SECONDS | scan PORT NAME]
//                            (dir gets data/cvf_online.ini and save/cvf_online_profile.json; UDP 7520 must be free)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int chdir(const char *path);  // <unistd.h> clashes with generated names
int mkdir(const char *path, unsigned mode);

#include "ik/ik_sim.h"

static lua_LState *L;
static int failures;

static bool run(const char *what, const char *src, bool quiet) {
    GoIface err = lua_LState_DoString(L, go_cstr(src));
    if (err.type) {
        if (!quiet) {
            GoString m = go_sprintf(GOSTR("%v"), &err, 1);
            fprintf(stderr, "FAIL %s: %.*s\n", what, m.len, m.p);
            failures++;
        }
        return false;
    }
    if (!quiet) printf("ok   %s\n", what);
    return true;
}

int main(int argc, char **argv) {
    gc_init(&argc);
    const char *dir = argc > 1 ? argv[1] : ".";
    if (chdir(dir) != 0) {
        perror(dir);
        return 2;
    }
    mkdir("data", 0755);
    FILE *f = fopen("data/cvf_online.ini", "w");
    if (!f) {
        perror("data/cvf_online.ini");
        return 2;
    }
    fputs("LobbyURL =\nBuildTag = nearby-test-1\n", f);
    fclose(f);
    mkdir("save", 0755);
    f = fopen("save/cvf_online_profile.json", "w");
    if (!f) {
        perror("save/cvf_online_profile.json");
        return 2;
    }
    fputs("{\"name\":\"Tester\"}", f);
    fclose(f);

    ik_init_globals();
    L = lua_NewState((Slice_lua_Options){0});
    fn_systemScriptInit(L);

    // desktop answers (cvf_p2p_other.go, cvf_nearby.go)
    run("desktop bindings",
        "assert(cvfIsMobile() == false, 'cvfIsMobile')\n"
        "assert(cvfTouchUI() == false, 'cvfTouchUI')\n"
        "assert(cvfTouchMenus() == true, 'cvfTouchMenus')\n"
        "assert(select('#', cvfTouchTap()) == 0, 'cvfTouchTap')\n"
        "assert(cvfAskNameBegin() == false, 'cvfAskNameBegin')\n"
        "assert(cvfAskNamePoll() == nil, 'cvfAskNamePoll')\n"
        "assert(select('#', cvfNearbyDirectJoin('x')) == 0, 'cvfNearbyDirectJoin')\n"
        "cvfNearbyDirectClose()\n"
        "local n = cvfDefaultName()\n"
        "assert(n:match('^Player %d%d%d%d$'), 'cvfDefaultName ' .. n)\n"
        "assert(cvfNearbyHint():find('hotspot'), 'cvfNearbyHint')\n"
        "cvfSetRollbackPort(7601)\n"
        "assert(cvfGetRollbackPort() == 7601, 'rollback port')\n"
        "cvfSetRollbackPort(0)\n"
        "assert(cvfGetRollbackPort() == 7601, 'rollback port range')\n",
        false);

    // modes: both (default), "host SECONDS" (answer probes only), "scan PORT NAME"
    // (find a host someone else runs, e.g. the Go reference in tests/ik/nearby_go)
    const char *mode = argc > 2 ? argv[2] : "both";
    bool host = strcmp(mode, "scan") != 0, scan = strcmp(mode, "host") != 0;
    int want_port = 7555;
    const char *want_name = "Tester";
    if (!strcmp(mode, "scan") && argc > 4) want_port = atoi(argv[3]), want_name = argv[4];

    if (host)
        run("host begin",
            "cvfNearbyHostBegin(7555)\n"
            "local s = cvfNearbyHostStatus()\n"
            "assert(s:find('Press Esc to stop hosting'), s)\n"
            "assert(s:find('Your address here: ') or s:find('Not connected'), s)\n"
            "assert(not s:find('unavailable'), s)\n",
            false);
    if (!scan) {
        int secs = argc > 3 ? atoi(argv[3]) : 10;
        printf("hosting for %d s\n", secs);
        fflush(stdout);
        go_sleep_ns((i64)secs * 1000000000);
        printf("ok   host answered %d probe(s)\n", g_cvfNearby->hostProbes);
        run("host end", "cvfNearbyHostEnd()", false);
        printf("%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

    Slice_GoString ex = {0};
    ex = go_append_Slice_GoString(ex, GOSTR("127.0.0.1:7520"));
    g_cvfNearby->extra = ex;
    run("scan begin", "cvfNearbyScanBegin()", false);

    char check[1024];
    snprintf(check, sizeof check,
             "local t, status = cvfNearbyHosts()\n"
             "for _, h in ipairs(t) do\n"
             "  if h.ip == '127.0.0.1' then\n"
             "    assert(h.port == %d, 'port ' .. h.port)\n"
             "    assert(h.name == '%s', 'name ' .. h.name)\n"
             "    assert(h.compatible == true, 'compatible')\n"
             "    assert(h.direct == false, 'direct')\n"
             "    assert(#h.id == 10, 'id ' .. h.id)\n"
             "    return\n"
             "  end\n"
             "end\n"
             "error('host not found (' .. #t .. ' hosts, status: ' .. status .. ')')\n",
             want_port, want_name);
    bool found = false;
    for (int i = 0; i < 60 && !found; i++) {  // up to 6 s
        go_sleep_ns(100000000);
        found = run("host listed", check, true);
    }
    if (found) printf("ok   host listed\n");
    else run("host listed", check, false);
    if (host) {  // a second sweep must come from the ticker (every 2.5 s)
        int probes = g_cvfNearby->hostProbes;
        for (int i = 0; i < 100 && g_cvfNearby->hostProbes == probes; i++) go_sleep_ns(100000000);
        if (g_cvfNearby->hostProbes > probes) printf("ok   periodic sweep\n");
        else fprintf(stderr, "FAIL periodic sweep: no probe after the first\n"), failures++;
    }

    run("scan end / host end",
        "cvfNearbyScanEnd()\n"
        "cvfNearbyHostEnd()\n"
        "local t = cvfNearbyHosts()\n"
        "assert(#t == 0, 'list after scan end')\n",
        false);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
