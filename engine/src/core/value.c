#include "core/value.h"

#include <math.h>
#include <stdlib.h>

#include "core/detmath.h"
#include "core/opcodes.h"
#include "core/rng.h"

f64 cvf_go_nan(void) {
    u64 u = 0x7FF8000000000001ull;
    f64 f;
    memcpy(&f, &u, 8);
    return f;
}

void cvf_stack_grow(CvfStack *s) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->v = (CvfValue *)realloc(s->v, sizeof(CvfValue) * s->cap);
    if (!s->v) cvf_fatal("bytecode stack: out of memory");
}

static inline bool is_float_pair(CvfValue a, CvfValue b) {
    // ValueType(Min(v1.vtype, v2.vtype)) == VT_Float
    return CVF_MIN(a.vtype, b.vtype) == VT_Float;
}

void cvf_op_neg(CvfValue *v) {
    if (v->vtype == VT_Float) v->value *= -1;
    else cvf_v_seti(v, -cvf_v_toi(*v));
}

void cvf_op_bwnot(CvfValue *v) { cvf_v_seti(v, ~cvf_v_toi(*v)); }

void cvf_op_blnot(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    cvf_v_setb(v, !cvf_v_tob(*v));
}

void cvf_op_pow(CvfValue *v1, CvfValue v2, bool old_version) {
    if (CVF_MIN(v1->vtype, v2.vtype) == VT_Float || cvf_v_tof(v2) < 0) {
        cvf_v_setf(v1, cvf_powf(cvf_v_tof(*v1), cvf_v_tof(v2)));
    } else {
        i32 i1 = cvf_v_toi(*v1), i2 = cvf_v_toi(v2), hb = -1;
        // Go: uint32(i2) >> uint(hb+1) != 0, shifts >= 32 yield 0
        while ((unsigned)(hb + 1) < 32 && ((u32)i2 >> (unsigned)(hb + 1)) != 0) hb++;
        i32 i = 1, bit = 0, tmp = i1;
        for (; bit <= hb; bit++) {
            unsigned shift = (unsigned)bit;
            if (old_version && bit != hb) shift = (unsigned)((hb - 1) - bit);
            i32 mask = shift < 32 ? (i32)(1u << shift) : 0;
            if ((i2 & mask) != 0) i *= tmp;
            tmp *= tmp;
        }
        cvf_v_seti(v1, i);
    }
    f64 result = (f64)cvf_v_tof(*v1);
    if (cvf_isnan(result) || isinf(result)) cvf_bytecode_error("Invalid exponentiation");
}

void cvf_op_mul(CvfValue *v1, CvfValue v2) {
    if (is_float_pair(*v1, v2)) cvf_v_setf(v1, cvf_v_tof(*v1) * cvf_v_tof(v2));
    else cvf_v_seti(v1, cvf_v_toi(*v1) * cvf_v_toi(v2));
}

void cvf_op_div(CvfValue *v1, CvfValue v2) {
    if (cvf_v_tof(v2) == 0) {
        *v1 = cvf_undef();
        cvf_bytecode_error("Division by 0");
    } else if (is_float_pair(*v1, v2)) {
        cvf_v_setf(v1, cvf_v_tof(*v1) / cvf_v_tof(v2));
    } else {
        i32 a = cvf_v_toi(*v1), b = cvf_v_toi(v2);
        // Go: MinInt32 / -1 == MinInt32 (wraps); C would trap.
        cvf_v_seti(v1, (b == -1) ? (i32)(0u - (u32)a) : a / b);
    }
}

void cvf_op_mod(CvfValue *v1, CvfValue v2) {
    if (cvf_v_toi(v2) == 0) {
        *v1 = cvf_undef();
        cvf_bytecode_error("Modulus by 0");
    } else {
        i32 a = cvf_v_toi(*v1), b = cvf_v_toi(v2);
        cvf_v_seti(v1, b == -1 ? 0 : a % b);
    }
}

void cvf_op_add(CvfValue *v1, CvfValue v2) {
    if (is_float_pair(*v1, v2)) cvf_v_setf(v1, cvf_v_tof(*v1) + cvf_v_tof(v2));
    else cvf_v_seti(v1, cvf_v_toi(*v1) + cvf_v_toi(v2));
}

void cvf_op_sub(CvfValue *v1, CvfValue v2) {
    if (is_float_pair(*v1, v2)) cvf_v_setf(v1, cvf_v_tof(*v1) - cvf_v_tof(v2));
    else cvf_v_seti(v1, cvf_v_toi(*v1) - cvf_v_toi(v2));
}

#define CMP_OP(name, OP)                                                          \
    void cvf_op_##name(CvfValue *v1, CvfValue v2) {                               \
        if (is_float_pair(*v1, v2)) cvf_v_setb(v1, cvf_v_tof(*v1) OP cvf_v_tof(v2)); \
        else cvf_v_setb(v1, cvf_v_toi(*v1) OP cvf_v_toi(v2));                     \
    }
CMP_OP(gt, >)
CMP_OP(ge, >=)
CMP_OP(lt, <)
CMP_OP(le, <=)

void cvf_op_range(CvfValue *v1, CvfValue min, CvfValue max, int op) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(min) || cvf_v_isundef(max)) {
        *v1 = cvf_undef();
        return;
    }
    bool minp, maxp;
    if (CVF_MIN(v1->vtype, CVF_MIN(min.vtype, max.vtype)) == VT_Float) {
        f32 v = cvf_v_tof(*v1), lo = cvf_v_tof(min), hi = cvf_v_tof(max);
        minp = (op == OC_range_ii || op == OC_range_ie) ? v >= lo : v > lo;
        maxp = (op == OC_range_ii || op == OC_range_ei) ? v <= hi : v < hi;
    } else {
        i32 v = cvf_v_toi(*v1), lo = cvf_v_toi(min), hi = cvf_v_toi(max);
        minp = (op == OC_range_ii || op == OC_range_ie) ? v >= lo : v > lo;
        maxp = (op == OC_range_ii || op == OC_range_ei) ? v <= hi : v < hi;
    }
    cvf_v_setb(v1, minp && maxp);
}

void cvf_op_eq(CvfValue *v1, CvfValue v2) {
    if (v1->vtype == VT_String || v2.vtype == VT_String) {
        if (v1->vtype != VT_String || v2.vtype != VT_String) {
            cvf_v_setb(v1, false);
            return;
        }
        cvf_v_setb(v1, strcmp(cvf_v_tolows(*v1), cvf_v_tolows(v2)) == 0);
        return;
    }
    if (v1->vtype != VT_None && v2.vtype != VT_None && (v1->vtype == VT_Float || v2.vtype == VT_Float))
        cvf_v_setb(v1, cvf_v_tof(*v1) == cvf_v_tof(v2));
    else
        cvf_v_setb(v1, cvf_v_toi(*v1) == cvf_v_toi(v2));
}

void cvf_op_ne(CvfValue *v1, CvfValue v2) {
    if (v1->vtype == VT_String || v2.vtype == VT_String) {
        if (v1->vtype != VT_String || v2.vtype != VT_String) {
            cvf_v_setb(v1, true);
            return;
        }
        cvf_v_setb(v1, strcmp(cvf_v_tolows(*v1), cvf_v_tolows(v2)) != 0);
        return;
    }
    if (v1->vtype != VT_None && v2.vtype != VT_None && (v1->vtype == VT_Float || v2.vtype == VT_Float))
        cvf_v_setb(v1, cvf_v_tof(*v1) != cvf_v_tof(v2));
    else
        cvf_v_setb(v1, cvf_v_toi(*v1) != cvf_v_toi(v2));
}

void cvf_op_bwand(CvfValue *v1, CvfValue v2) { cvf_v_seti(v1, cvf_v_toi(*v1) & cvf_v_toi(v2)); }
void cvf_op_bwxor(CvfValue *v1, CvfValue v2) { cvf_v_seti(v1, cvf_v_toi(*v1) ^ cvf_v_toi(v2)); }
void cvf_op_bwor(CvfValue *v1, CvfValue v2) { cvf_v_seti(v1, cvf_v_toi(*v1) | cvf_v_toi(v2)); }
void cvf_op_bland(CvfValue *v1, CvfValue v2) { cvf_v_setb(v1, cvf_v_tob(*v1) && cvf_v_tob(v2)); }
void cvf_op_blxor(CvfValue *v1, CvfValue v2) { cvf_v_setb(v1, cvf_v_tob(*v1) != cvf_v_tob(v2)); }
void cvf_op_blor(CvfValue *v1, CvfValue v2) { cvf_v_setb(v1, cvf_v_tob(*v1) || cvf_v_tob(v2)); }

void cvf_op_abs(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    if (v->vtype == VT_Float) v->value = fabs(v->value);
    else {
        i32 i = cvf_v_toi(*v);
        cvf_v_seti(v, i < 0 ? -i : i);
    }
}

void cvf_op_exp(CvfValue *v) { cvf_v_setf(v, (f32)cvf_exp(v->value)); }

void cvf_op_ln(CvfValue *v) {
    if (v->value <= 0) {
        *v = cvf_undef();
        cvf_bytecode_error("Invalid logarithm");
    } else {
        cvf_v_setf(v, (f32)cvf_ln(v->value));
    }
}

void cvf_op_log(CvfValue *v1, CvfValue v2) {
    if (v1->value <= 0 || v2.value <= 0) {
        *v1 = cvf_undef();
        cvf_bytecode_error("Invalid logarithm");
    } else {
        cvf_v_setf(v1, (f32)(cvf_ln(v2.value) / cvf_ln(v1->value)));
    }
}

#define TRIG(name, fn)                               \
    void cvf_op_##name(CvfValue *v) {                \
        if (cvf_v_isundef(*v)) return;               \
        cvf_v_setf(v, (f32)fn(v->value));            \
    }
TRIG(cos, cvf_cos)
TRIG(sin, cvf_sin)
TRIG(tan, cvf_tan)
TRIG(acos, cvf_acos)
TRIG(asin, cvf_asin)
TRIG(atan, cvf_atan)

void cvf_op_floor(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    if (v->vtype == VT_Float) {
        f64 f = floor(v->value);
        if (cvf_isnan(f)) *v = cvf_undef();
        else cvf_v_seti(v, cvf_f2i32(f));
    }
}

void cvf_op_ceil(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    if (v->vtype == VT_Float) {
        f64 f = ceil(v->value);
        if (cvf_isnan(f)) *v = cvf_undef();
        else cvf_v_seti(v, cvf_f2i32(f));
    }
}

void cvf_op_max(CvfValue *v1, CvfValue v2) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2)) { *v1 = cvf_undef(); return; }
    if (v1->value >= v2.value) cvf_v_setf(v1, (f32)v1->value);
    else cvf_v_setf(v1, (f32)v2.value);
}

void cvf_op_min(CvfValue *v1, CvfValue v2) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2)) { *v1 = cvf_undef(); return; }
    if (v1->value <= v2.value) cvf_v_setf(v1, (f32)v1->value);
    else cvf_v_setf(v1, (f32)v2.value);
}

void cvf_op_random(CvfValue *v1, CvfValue v2, i32 *seed) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2)) { *v1 = cvf_undef(); return; }
    cvf_v_seti(v1, cvf_randi(seed, cvf_f2i32(v1->value), cvf_f2i32(v2.value)));
}

void cvf_op_round(CvfValue *v1, CvfValue v2) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2)) { *v1 = cvf_undef(); return; }
    f64 shift = cvf_pow(10, v2.value);
    cvf_v_setf(v1, (f32)(floor((v1->value * shift) + 0.5) / shift));
}

void cvf_op_clamp(CvfValue *v1, CvfValue v2, CvfValue v3) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2) || cvf_v_isundef(v3)) { *v1 = cvf_undef(); return; }
    if (v1->value <= v2.value) cvf_v_setf(v1, (f32)v2.value);
    else if (v1->value >= v3.value) cvf_v_setf(v1, (f32)v3.value);
    else cvf_v_setf(v1, (f32)v1->value);
}

void cvf_op_atan2(CvfValue *v1, CvfValue v2) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2)) { *v1 = cvf_undef(); return; }
    cvf_v_setf(v1, (f32)cvf_atan2(v1->value, v2.value));
}

void cvf_op_sign(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    if (v->value < 0) cvf_v_seti(v, -1);
    else if (v->value > 0) cvf_v_seti(v, 1);
    else cvf_v_seti(v, 0);
}

void cvf_op_rad(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    cvf_v_setf(v, (f32)(v->value * GO_PI / 180));
}

void cvf_op_deg(CvfValue *v) {
    if (cvf_v_isundef(*v)) return;
    cvf_v_setf(v, (f32)(v->value * 180 / GO_PI));
}

void cvf_op_lerp(CvfValue *v1, CvfValue v2, CvfValue v3) {
    if (cvf_v_isundef(*v1) || cvf_v_isundef(v2) || cvf_v_isundef(v3)) { *v1 = cvf_undef(); return; }
    f64 amount = v3.value;
    if (v3.value <= 0) amount = 0;
    else if (v3.value >= 1) amount = 1;
    cvf_v_setf(v1, (f32)(v1->value + (v2.value - v1->value) * amount));
}
