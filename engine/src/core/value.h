// Bytecode values and their operators. Port of Ikemen GO bytecode.go
// (BytecodeValue, BytecodeStack and the BytecodeExp value helpers).
#ifndef CVF_VALUE_H
#define CVF_VALUE_H

#include "core/base.h"

typedef enum { VT_None, VT_Float, VT_Int, VT_Bool, VT_String, VT_Undefined } CvfVType;

typedef struct CvfValue {
    i32 vtype;
    f64 value;
} CvfValue;

// Go float64 -> int conversions as compiled for amd64 (the reference
// platform): out-of-range and NaN give the "integer indefinite" value.
static inline i32 cvf_f2i32(f64 f) {
    if (!(f > -2147483649.0 && f < 2147483648.0)) return INT32_MIN;
    return (i32)f;
}
static inline i64 cvf_f2i64(f64 f) {
    if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) return INT64_MIN;
    return (i64)f;
}
static inline bool cvf_isnan(f64 f) { return f != f; }

static inline bool cvf_v_isnone(CvfValue v) { return v.vtype == VT_None; }
static inline bool cvf_v_isundef(CvfValue v) { return v.vtype == VT_Undefined || cvf_isnan(v.value); }
static inline f32 cvf_v_tof(CvfValue v) {
    if (cvf_v_isundef(v) || v.vtype == VT_String) return 0;
    return (f32)v.value;
}
static inline i32 cvf_v_toi(CvfValue v) {
    if (cvf_v_isundef(v) || v.vtype == VT_String) return 0;
    return cvf_f2i32(v.value);
}
static inline i64 cvf_v_toi64(CvfValue v) {
    if (cvf_v_isundef(v) || v.vtype == VT_String) return 0;
    return cvf_f2i64(v.value);
}
static inline bool cvf_v_tob(CvfValue v) {
    if (cvf_v_isundef(v)) return false;
    if (v.vtype == VT_String) return false;
    return v.value != 0;
}

f64 cvf_go_nan(void);  // math.NaN() bit pattern
static inline CvfValue cvf_undef(void) { return (CvfValue){VT_Undefined, cvf_go_nan()}; }
static inline CvfValue cvf_none(void) { return (CvfValue){VT_None, 0}; }
static inline CvfValue cvf_int(i32 i) { return (CvfValue){VT_Int, (f64)i}; }
static inline CvfValue cvf_int64(i64 i) { return (CvfValue){VT_Int, (f64)i}; }
static inline CvfValue cvf_bool(bool b) { return (CvfValue){VT_Bool, b ? 1.0 : 0.0}; }
static inline CvfValue cvf_float(f32 f) {
    if (f != f) return cvf_undef();
    return (CvfValue){VT_Float, (f64)f};
}
static inline void cvf_v_setf(CvfValue *v, f32 f) { *v = cvf_float(f); }
static inline void cvf_v_seti(CvfValue *v, i32 i) { *v = cvf_int(i); }
static inline void cvf_v_setb(CvfValue *v, bool b) { v->vtype = VT_Bool; v->value = b ? 1.0 : 0.0; }

// String values hold an index into the string pool of the player that owns
// the currently running state (sys.workingState.playerNo). The pool lookup
// is supplied by the system layer.
const char *cvf_v_tos(CvfValue v);
const char *cvf_v_tolows(CvfValue v);

// ---------------------------------------------------------------- operators
// v1 = v1 OP v2, exactly as Ikemen's BytecodeExp helpers.
void cvf_op_neg(CvfValue *v);
void cvf_op_bwnot(CvfValue *v);
void cvf_op_blnot(CvfValue *v);
void cvf_op_pow(CvfValue *v1, CvfValue v2, bool old_version);
void cvf_op_mul(CvfValue *v1, CvfValue v2);
void cvf_op_div(CvfValue *v1, CvfValue v2);
void cvf_op_mod(CvfValue *v1, CvfValue v2);
void cvf_op_add(CvfValue *v1, CvfValue v2);
void cvf_op_sub(CvfValue *v1, CvfValue v2);
void cvf_op_gt(CvfValue *v1, CvfValue v2);
void cvf_op_ge(CvfValue *v1, CvfValue v2);
void cvf_op_lt(CvfValue *v1, CvfValue v2);
void cvf_op_le(CvfValue *v1, CvfValue v2);
void cvf_op_range(CvfValue *v1, CvfValue min, CvfValue max, int op);  // op: OC_range_*
void cvf_op_eq(CvfValue *v1, CvfValue v2);
void cvf_op_ne(CvfValue *v1, CvfValue v2);
void cvf_op_bwand(CvfValue *v1, CvfValue v2);
void cvf_op_bwxor(CvfValue *v1, CvfValue v2);
void cvf_op_bwor(CvfValue *v1, CvfValue v2);
void cvf_op_bland(CvfValue *v1, CvfValue v2);
void cvf_op_blxor(CvfValue *v1, CvfValue v2);
void cvf_op_blor(CvfValue *v1, CvfValue v2);
void cvf_op_abs(CvfValue *v);
void cvf_op_exp(CvfValue *v);
void cvf_op_ln(CvfValue *v);
void cvf_op_log(CvfValue *v1, CvfValue v2);
void cvf_op_cos(CvfValue *v);
void cvf_op_sin(CvfValue *v);
void cvf_op_tan(CvfValue *v);
void cvf_op_acos(CvfValue *v);
void cvf_op_asin(CvfValue *v);
void cvf_op_atan(CvfValue *v);
void cvf_op_floor(CvfValue *v);
void cvf_op_ceil(CvfValue *v);
void cvf_op_max(CvfValue *v1, CvfValue v2);
void cvf_op_min(CvfValue *v1, CvfValue v2);
void cvf_op_random(CvfValue *v1, CvfValue v2, i32 *seed);
void cvf_op_round(CvfValue *v1, CvfValue v2);
void cvf_op_clamp(CvfValue *v1, CvfValue v2, CvfValue v3);
void cvf_op_atan2(CvfValue *v1, CvfValue v2);
void cvf_op_sign(CvfValue *v);
void cvf_op_rad(CvfValue *v);
void cvf_op_deg(CvfValue *v);
void cvf_op_lerp(CvfValue *v1, CvfValue v2, CvfValue v3);

// Reported like sys.printBytecodeError (console only, no effect on play).
void cvf_bytecode_error(const char *msg);

// ---------------------------------------------------------------- stack
typedef struct CvfStack {
    CvfValue *v;
    u32 n, cap;
} CvfStack;

void cvf_stack_grow(CvfStack *s);
static inline void cvf_push(CvfStack *s, CvfValue v) {
    if (s->n == s->cap) cvf_stack_grow(s);
    s->v[s->n++] = v;
}
static inline CvfValue cvf_pop(CvfStack *s) { return s->v[--s->n]; }
static inline CvfValue *cvf_top(CvfStack *s) { return &s->v[s->n - 1]; }

#endif
