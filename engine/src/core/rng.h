// Deterministic random numbers, identical to Ikemen GO (Park-Miller
// minimal standard generator, Schrage's method) so reference fights replay
// exactly. The seed is part of match state.
#ifndef CVF_RNG_H
#define CVF_RNG_H

#include "core/base.h"

i32 cvf_random(i32 *seed);
i32 cvf_rand(i32 *seed, i32 min, i32 max);       // Rand(min, max)
i32 cvf_randi(i32 *seed, i32 x, i32 y);          // RandI(x, y)
f32 cvf_randf(i32 *seed, f32 x, f32 y);          // RandF(x, y)
f32 cvf_randf32(i32 *seed, f32 min, f32 max);    // RandF32(min, max)

#endif
