#include "core/rng.h"

// Ports of common.go Random/Rand/RandI/RandF/RandF32. Arithmetic is done in
// 32-bit with wrapping (the engine is built with -fwrapv, matching Go).

i32 cvf_random(i32 *seed) {
    i32 s = *seed;
    i32 w = s / 127773;
    s = (s - w * 127773) * 16807 - w * 2836;
    if (s <= 0) s += CVF_IMAX - (s == 0 ? 1 : 0);
    *seed = s;
    return s;
}

i32 cvf_rand(i32 *seed, i32 min, i32 max) {
    return min + cvf_random(seed) / (CVF_IMAX / (max - min + 1) + 1);
}

f32 cvf_randf32(i32 *seed, f32 min, f32 max) {
    return min + (f32)cvf_random(seed) / ((f32)CVF_IMAX / (max - min + 1.0f) + 1.0f);
}

i32 cvf_randi(i32 *seed, i32 x, i32 y) {
    if (y < x) {
        if ((u32)(x - y) > (u32)CVF_IMAX)
            return (i32)((i64)y + (i64)cvf_random(seed) * ((i64)x - (i64)y) / (i64)CVF_IMAX);
        return cvf_rand(seed, y, x);
    }
    if ((u32)(y - x) > (u32)CVF_IMAX)
        return (i32)((i64)x + (i64)cvf_random(seed) * ((i64)y - (i64)x) / (i64)CVF_IMAX);
    return cvf_rand(seed, x, y);
}

f32 cvf_randf(i32 *seed, f32 x, f32 y) {
    return x + (f32)cvf_random(seed) * (y - x) / (f32)CVF_IMAX;
}
