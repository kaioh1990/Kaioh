// Fetches a URL with the runtime HTTP client (ik_http.c): tools/http_probe URL
#include "ik/ik_sim.h"
#include <stdio.h>
int main(int argc, char **argv) {
    gc_init(&argc);
    ik_init_globals();
    GoHttpClient c = {15000000000LL};
    Ret_GoHttpResponsePtr__GoIface r = http_Client_Get(&c, go_cstr(argv[1]));
    if (r.r1.type) { GoString e; ik_fmt_method(r.r1, &e); printf("error: %.*s\n", e.len, e.p); return 1; }
    Ret_Slice_u8__GoIface b = io_ReadAll(r.r0->Body);
    printf("status %d, %d bytes: %.*s\n", r.r0->StatusCode, b.r0.len, b.r0.len > 200 ? 200 : b.r0.len, b.r0.p);
    return 0;
}
