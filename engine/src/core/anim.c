#include "core/anim.h"

static inline i32 imax(i32 a, i32 b) { return a > b ? a : b; }

void cvf_anim_init(CvfAnim *a, const CvfAnimDef *def) {
    memset(a, 0, sizeof *a);
    a->def = def;
    // newAnimation defaults
    a->mask = -1;
    a->trans_type = CVF_TT_DEFAULT;
    a->src_alpha = 255;
    a->dst_alpha = 0;
    a->curtrans = CVF_TT_NONE;
    a->interp_blend_src = 255;
    a->interp_blend_dst = 0;
    a->newframe = true;
    a->start_scale[0] = a->start_scale[1] = 1;
    a->last_action_frame = -1;
    if (def) {
        // readAnimation sets mask = 0 and the loop/time fields
        a->mask = def->mask;
        a->loopstart = def->loopstart;
        a->totaltime = def->totaltime;
        a->looptime = def->looptime;
        a->prelooptime = def->prelooptime;
    }
}

void cvf_anim_reset(CvfAnim *a) {
    a->curelem = a->drawidx = 0;
    a->curelemtime = a->curtime = 0;
    a->newframe = true;
    a->loopend = false;
    a->loopcount = 0;
    a->has_spr = false;
}

i32 cvf_anim_time(const CvfAnim *a) { return a->curtime - a->totaltime; }

i32 cvf_anim_elem_time(const CvfAnim *a, i32 elem) {
    i32 n = cvf_anim_nframes(a);
    if (elem > n) {
        i32 t = cvf_anim_time(a);
        if (t > 0) t = 0;
        return t;
    }
    i32 e = imax(0, elem) - 1, t = a->curtime;
    for (i32 i = 0; i < e; i++) t -= imax(0, a->def->frames[i].time);
    return t;
}

i32 cvf_anim_elem_no(const CvfAnim *a, i32 time) {
    i32 n = cvf_anim_nframes(a);
    if (n > 0) {
        const CvfAnimFrame *fr = a->def->frames;
        i32 i = a->curelem, oldt = 0;
        if (time <= 0) {
            time += a->curelemtime;
            bool loop = false;
            for (;;) {
                if (time >= 0) return i + 1;
                i--;
                if (i < 0 || (a->curelem >= a->loopstart && i < a->loopstart)) {
                    if (time == oldt) break;
                    oldt = time;
                    loop = true;
                    i = n - 1;
                }
                time += imax(0, fr[i].time);
                if (loop && i == n - 1 && fr[i].time == -1) return i + 1;
            }
        } else {
            time += a->curelemtime;
            for (;;) {
                time -= imax(0, fr[i].time);
                if (time < 0 || (i == n - 1 && fr[i].time == -1)) return i + 1;
                i++;
                if (i >= n) {
                    if (time == oldt) break;
                    oldt = time;
                    i = a->loopstart;
                }
            }
        }
    }
    return n;
}

i32 cvf_anim_length(const CvfAnim *a) {
    if (!a || cvf_anim_nframes(a) == 0) return 0;
    i32 len = 0;
    for (i32 i = 0; i < a->def->nframes; i++) {
        i32 t = a->def->frames[i].time;
        len += t == -1 ? 1 : (t > 0 ? t : 0);
    }
    return len;
}

const CvfAnimFrame *cvf_anim_cur_frame(const CvfAnim *a) {
    if (cvf_anim_nframes(a) == 0) return NULL;
    return &a->def->frames[a->curelem];
}

const CvfAnimFrame *cvf_anim_draw_frame(const CvfAnim *a) {
    if (cvf_anim_nframes(a) == 0) return NULL;
    return &a->def->frames[a->drawidx];
}

static void anim_seek(CvfAnim *a, i32 elem) {
    i32 n = cvf_anim_nframes(a);
    const CvfAnimFrame *fr = a->def->frames;
    if (elem < 0) elem = 0;
    bool foo = true;
    for (;;) {
        a->curelem = elem;
        while (a->curelem < n && fr[a->curelem].time <= 0) {
            if (a->curelem == n - 1 && fr[a->curelem].time == -1) break;
            a->curelem++;
        }
        if (a->curelem < n) break;
        foo = !foo;
        if (foo) {
            a->curelem = n - 1;
            break;
        }
    }
    if (a->curelem < 0) a->curelem = 0;
    else if (a->curelem >= n) a->curelem = n - 1;
}

static void update_interpolation(CvfAnim *a) {
    const CvfAnimDef *d = a->def;
    const CvfAnimFrame *fr = d->frames;
    a->interp_offset_x = 0;
    a->interp_offset_y = 0;
    a->interp_blend_src = (f32)fr[a->drawidx].src_alpha;
    a->interp_blend_dst = (f32)fr[a->drawidx].dst_alpha;
    i32 next = a->drawidx + 1;
    if (a->drawidx >= d->nframes - 1) next = a->loopstart;
    f32 progress = 0;
    if (fr[a->drawidx].time > 0) progress = (f32)a->curelemtime / (f32)fr[a->curelem].time;
    if (progress <= 0) return;
    for (i32 k = 0; k < d->n_interp_offset; k++) {
        if (next != d->interp_offset[k]) continue;
        a->interp_offset_x = (f32)(fr[next].xoffset - fr[a->drawidx].xoffset) * progress;
        a->interp_offset_y = (f32)(fr[next].yoffset - fr[a->drawidx].yoffset) * progress;
        break;
    }
    for (i32 k = 0; k < d->n_interp_scale; k++) {
        if (next != d->interp_scale[k]) continue;
        a->scale_x += (fr[next].xscale - fr[a->drawidx].xscale) * progress;
        a->scale_y += (fr[next].yscale - fr[a->drawidx].yscale) * progress;
        break;
    }
    for (i32 k = 0; k < d->n_interp_angle; k++) {
        if (next != d->interp_angle[k]) continue;
        a->rot_angle += (fr[next].angle - fr[a->drawidx].angle) * progress;
        break;
    }
    for (i32 k = 0; k < d->n_interp_blend; k++) {
        if (next != d->interp_blend[k]) continue;
        if (a->curtrans != CVF_TT_NONE) {
            a->interp_blend_src += ((f32)fr[next].src_alpha - a->interp_blend_src) * progress;
            a->interp_blend_dst += ((f32)fr[next].dst_alpha - a->interp_blend_dst) * progress;
        }
        break;
    }
}

void cvf_anim_update_sprite(CvfAnim *a) {
    if (cvf_anim_nframes(a) == 0) return;
    const CvfAnimFrame *fr = a->def->frames;
    if (a->totaltime > 0) {
        if (a->curtime >= a->totaltime) {
            a->curelemtime = 0;
            a->newframe = true;
            a->curelem = a->loopstart;
        }
        anim_seek(a, a->curelem);
        if (a->prelooptime < 0 && a->curtime >= a->totaltime + a->prelooptime &&
            a->curtime >= a->totaltime - a->looptime &&
            (a->curtime == a->totaltime + a->prelooptime || a->curtime == a->totaltime - a->looptime)) {
            a->curelemtime = 0;
            a->newframe = true;
            a->curelem = 0;
        }
    }
    // Sprite selection (Ikemen: only when an SFF is attached). Rendering
    // resolves the sprite from these numbers; the simulation ignores them.
    if (a->newframe && fr[a->curelem].time != 0) {
        i32 g = fr[a->curelem].group, n = fr[a->curelem].number;
        a->has_spr = g != -1 && n != -1;
        a->spr_group = g;
        a->spr_number = n;
    }
    a->newframe = false;
    a->drawidx = a->curelem;
    a->curtrans = fr[a->drawidx].trans;
    a->scale_x = fr[a->drawidx].xscale;
    a->scale_y = fr[a->drawidx].yscale;
    a->rot_angle = fr[a->drawidx].angle;
    update_interpolation(a);
    a->scale_x *= a->start_scale[0];
    a->scale_y *= a->start_scale[1];
}

void cvf_anim_set_elem(CvfAnim *a, i32 elem, i32 elemtime) {
    i32 n = cvf_anim_nframes(a);
    a->curelem = imax(0, elem - 1);
    if (a->curelem >= n) a->curelem = 0;
    a->drawidx = a->curelem;
    if (elemtime != 0) {
        i32 ft = a->def->frames[a->curelem].time;
        if (elemtime < 0 || (ft != -1 && elemtime >= ft)) elemtime = 0;
    }
    a->curelemtime = elemtime;
    a->newframe = true;
    a->loopend = false;
    a->loopcount = 0;
    cvf_anim_update_sprite(a);
    a->curtime = 0;
    a->curtime = -cvf_anim_elem_time(a, a->curelem + 1) + a->curelemtime;
}

static void anim_next(CvfAnim *a) {
    i32 n = cvf_anim_nframes(a);
    const CvfAnimFrame *fr = a->def->frames;
    if (a->totaltime != -1 || a->curelem < n - 1) {
        a->curelemtime = 0;
        a->newframe = true;
        for (;;) {
            a->curelem++;
            if ((a->totaltime == -1 && a->curelem == n - 1) || a->curelem >= n || fr[a->curelem].time > 0) break;
        }
    }
}

void cvf_anim_action(CvfAnim *a) {
    if (!a || !a->def) return;  // Go: a == nil || a.frames == nil
    i32 n = cvf_anim_nframes(a);
    if (n == 0) {
        // Go checks a.frames == nil first: an empty (non-nil) slice sets
        // loopend. Cooked/AIR empty actions have frames == nil in Go
        // (append never called), so they return without setting loopend.
        return;
    }
    cvf_anim_update_sprite(a);
    const CvfAnimFrame *fr = a->def->frames;
    if (fr[a->curelem].time <= 0) anim_next(a);
    if (a->curelem < n) {
        a->curelemtime++;
        if (a->curelemtime >= fr[a->curelem].time) {
            anim_next(a);
            if (a->curelem >= n) a->curelem = a->loopstart;
        }
    } else {
        a->curelem = a->loopstart;
    }
    if (a->totaltime != -1 && a->curtime >= a->totaltime) {
        a->curtime = a->totaltime - a->looptime;
        a->loopcount++;
    }
    a->curtime++;
    if (a->totaltime != -1 && a->curtime >= a->totaltime) a->loopend = true;
}
