// Animation playback state. Port of the runtime half of Ikemen GO's
// anim.go Animation (timing, element seeking, interpolation). The static
// definition (frames, clsn, loop info) is a CvfAnimDef shared by all copies.
#ifndef CVF_ANIM_H
#define CVF_ANIM_H

#include "formats/air.h"

typedef struct CvfAnim {
    const CvfAnimDef *def;  // NULL = no animation
    // Copied from the definition when the animation is instantiated (Ikemen
    // copies the whole struct; some fields can then be changed per copy).
    i32 loopstart, totaltime, looptime, prelooptime;
    i32 curtime, curelem, curelemtime, last_action_frame, drawidx;
    i16 mask;
    i32 trans_type;
    i16 src_alpha, dst_alpha;
    bool newframe, loopend;
    f32 interp_offset_x, interp_offset_y;
    f32 scale_x, scale_y;
    f32 rot_angle;
    i32 curtrans;
    f32 interp_blend_src, interp_blend_dst;
    f32 start_scale[2];
    i32 loopcount;
    // Sprite selection (render only; the simulation never reads it).
    i32 spr_group, spr_number;
    bool has_spr;
} CvfAnim;

static inline i32 cvf_anim_nframes(const CvfAnim *a) { return a->def ? a->def->nframes : 0; }
static inline const CvfAnimFrame *cvf_anim_frame(const CvfAnim *a, i32 i) { return &a->def->frames[i]; }

void cvf_anim_init(CvfAnim *a, const CvfAnimDef *def);  // newAnimation + *ret = *def
void cvf_anim_reset(CvfAnim *a);
i32 cvf_anim_time(const CvfAnim *a);                      // AnimTime
i32 cvf_anim_elem_time(const CvfAnim *a, i32 elem);       // AnimElemTime
i32 cvf_anim_elem_no(const CvfAnim *a, i32 time);         // AnimElemNo
i32 cvf_anim_length(const CvfAnim *a);                    // GetLength
const CvfAnimFrame *cvf_anim_cur_frame(const CvfAnim *a); // CurrentFrame (NULL when no frames)
const CvfAnimFrame *cvf_anim_draw_frame(const CvfAnim *a);
void cvf_anim_set_elem(CvfAnim *a, i32 elem, i32 elemtime);  // SetAnimElem
void cvf_anim_update_sprite(CvfAnim *a);                  // UpdateSprite
void cvf_anim_action(CvfAnim *a);                         // Action

#endif
