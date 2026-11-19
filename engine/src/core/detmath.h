// Deterministic math: every platform computes bit-identical results.
// Ports of the pure-Go versions in Go's math package (the reference engine
// uses exactly these, see Ikemen cvf_detmath.go). Never use libm's
// transcendental functions in game logic: their results differ between
// libraries and CPUs. sqrt, floor, ceil, ldexp, frexp, modf are exact by
// IEEE 754 and safe to use directly.
#ifndef CVF_DETMATH_H
#define CVF_DETMATH_H

#include "core/base.h"

f64 cvf_exp(f64 x);
f64 cvf_ln(f64 x);
f64 cvf_log2(f64 x);
f64 cvf_pow(f64 x, f64 y);
f64 cvf_hypot(f64 p, f64 q);
f64 cvf_sin(f64 x);
f64 cvf_cos(f64 x);
f64 cvf_tan(f64 x);
f64 cvf_atan(f64 x);
f64 cvf_asin(f64 x);
f64 cvf_acos(f64 x);
f64 cvf_atan2(f64 y, f64 x);

// Exact Go constant-expression values (Go folds constants at arbitrary
// precision; writing 4/M_PI in C would round twice).
#define GO_4_OVER_PI 0x1.45f306dc9c883p+00
#define GO_PI 0x1.921fb54442d18p+01
#define GO_PI_2 0x1.921fb54442d18p+00
#define GO_PI_4 0x1.921fb54442d18p-01
#define GO_3PI_4 0x1.2d97c7f3321d2p+01
#define GO_SQRT2_2 0x1.6a09e667f3bcdp-01
#define GO_INV_LN2 0x1.71547652b82fep+00
#define GO_PI_180 0x1.1df46a2529d39p-06
#define GO_180_PI 0x1.ca5dc1a63c1f8p+05
#define GO_PI_F32 0x1.921fb6p+01f

// Ikemen common.go helpers (float32 versions used by game logic).
static inline f32 cvf_rad(f32 f) { return f * GO_PI_F32 / 180.0f; }
static inline f32 cvf_deg(f32 f) { return f * 180.0f / GO_PI_F32; }
static inline f32 cvf_cosf(f32 f) { return (f32)cvf_cos((f64)f); }
static inline f32 cvf_sinf(f32 f) { return (f32)cvf_sin((f64)f); }
f32 cvf_powf(f32 x, f32 y);  // Ikemen Pow (fast paths + cvf_pow)

#endif
