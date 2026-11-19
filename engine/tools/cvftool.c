// cvftool: command-line access to the engine core for checking content
// against the reference engine.
//   cvftool airdigest FILE...   digest of compiled AIR files (see tests/golden/air.go)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "formats/air.h"

static u64 H;
static void mix(u64 v) { H = (H ^ v) * 1099511628211ull; }
static u32 fb(f32 f) { u32 u; memcpy(&u, &f, 4); return u; }

static int cmp_i32(const void *a, const void *b) {
    i32 x = *(const i32 *)a, y = *(const i32 *)b;
    return x < y ? -1 : x > y;
}

static int airdigest(int argc, char **argv) {
    H = 1469598103934665603ull;
    int nact = 0;
    for (int f = 0; f < argc; f++) {
        CvfAnimTable t;
        cvf_animtable_init(&t);
        if (!cvf_air_compile_file(&t, argv[f])) { cvf_animtable_free(&t); continue; }
        cvf_air_finalize(&t);
        i32 *keys = (i32 *)malloc(sizeof(i32) * (size_t)(t.count + 1));
        int nk = 0;
        for (i32 s = 0; s < t.cap; s++)
            if (t.vals[s]) keys[nk++] = t.keys[s];
        qsort(keys, (size_t)nk, sizeof(i32), cmp_i32);
        for (int k = 0; k < nk; k++) {
            const CvfAnimDef *a = cvf_animtable_get(&t, keys[k]);
            nact++;
            mix((u32)keys[k]);
            mix((u64)a->nframes);
            mix((u32)a->loopstart);
            mix((u32)a->totaltime);
            mix((u32)a->looptime);
            mix((u32)a->prelooptime);
            mix((u32)a->action_no);
            const i32 *lists[4] = {a->interp_offset, a->interp_scale, a->interp_angle, a->interp_blend};
            const i32 ns[4] = {a->n_interp_offset, a->n_interp_scale, a->n_interp_angle, a->n_interp_blend};
            for (int l = 0; l < 4; l++) {
                mix((u64)ns[l]);
                for (int x = 0; x < ns[l]; x++) mix((u32)lists[l][x]);
            }
            for (i32 i = 0; i < a->nframes; i++) {
                const CvfAnimFrame *fr = &a->frames[i];
                mix((u64)(u32)fr->time | (u64)(u32)fr->group << 32);
                mix((u64)(u32)fr->number | (u64)(u32)fr->xoffset << 32);
                mix((u64)(u32)fr->yoffset | (u64)(u32)fr->trans << 32);
                mix((u64)fr->src_alpha | (u64)fr->dst_alpha << 8 | (u64)(u8)fr->hscale << 16 | (u64)(u8)fr->vscale << 24);
                mix((u64)fb(fr->xscale) | (u64)fb(fr->yscale) << 32);
                mix((u64)fb(fr->angle));
                const CvfClsn *cs[2] = {&fr->clsn1, &fr->clsn2};
                for (int c = 0; c < 2; c++) {
                    mix((u64)cs[c]->count);
                    for (i32 b = 0; b < cs[c]->count; b++)
                        for (int x = 0; x < 4; x++) mix((u64)fb(cs[c]->box[b][x]));
                }
            }
        }
        free(keys);
        cvf_animtable_free(&t);
    }
    printf("%d 0x%016llx\n", nact, (unsigned long long)H);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "airdigest")) return airdigest(argc - 2, argv + 2);
    fprintf(stderr, "usage: cvftool airdigest FILE...\n");
    return 2;
}
