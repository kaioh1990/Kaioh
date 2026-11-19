// AIR animation definitions. Port of Ikemen GO anim.go (AnimCompiler,
// ReadAnimFrame, readClsn, compileAction, resolveCopyAction). Only the static
// definition lives here; per-object playback state is in core/anim.h.
#ifndef CVF_AIR_H
#define CVF_AIR_H

#include "core/base.h"

typedef enum { CVF_TT_NONE, CVF_TT_ADD, CVF_TT_SUB, CVF_TT_SUBADD, CVF_TT_DEFAULT } CvfTransType;

typedef struct CvfClsn {
    f32 (*box)[4];  // count boxes (x1, y1, x2, y2), normalized
    i32 count;
} CvfClsn;

typedef struct CvfAnimFrame {
    i32 time, group, number, xoffset, yoffset;
    i32 trans;  // CvfTransType
    u8 src_alpha, dst_alpha;
    i8 hscale, vscale;
    f32 xscale, yscale, angle;
    CvfClsn clsn1, clsn2;
} CvfAnimFrame;

typedef struct CvfAnimDef {
    i32 action_no;
    CvfAnimFrame *frames;
    i32 nframes;
    i32 loopstart;
    i32 *interp_offset, *interp_scale, *interp_angle, *interp_blend;
    i32 n_interp_offset, n_interp_scale, n_interp_angle, n_interp_blend;
    i32 totaltime, looptime, prelooptime;
    i32 copy_action;
    i16 mask;  // as set by readAnimation (0)
} CvfAnimDef;

// Action number -> definition. Open addressing; entries are pointers into
// the table's arena. Sorted iteration order is available via keys.
typedef struct CvfAnimTable {
    CvfArena arena;
    i32 *keys;           // slot keys
    CvfAnimDef **vals;   // slot values (NULL = empty)
    i32 cap, count;
} CvfAnimTable;

void cvf_animtable_init(CvfAnimTable *t);
void cvf_animtable_free(CvfAnimTable *t);
CvfAnimDef *cvf_animtable_get(const CvfAnimTable *t, i32 no);

// AnimCompiler: compile several AIR sources into one table, then finalize
// (resolves Copy Action across all of them).
bool cvf_air_compile_file(CvfAnimTable *t, const char *path);
void cvf_air_compile_text(CvfAnimTable *t, char *text, size_t len);
void cvf_air_finalize(CvfAnimTable *t);

// Go strconv.ParseFloat acceptance test (Ikemen IsNumeric).
bool cvf_is_numeric(CvfStr s);

#endif
