// Ported from Go's src/math (exp.go, log.go, pow.go, hypot.go, log10.go,
// sin.go, tan.go, trig_reduce.go, atan.go, asin.go, atan2.go) via Ikemen's
// cvf_detmath.go. Original Go code: Copyright 2009 The Go Authors (BSD
// licence); Cephes parts: Copyright 1984-2000 Stephen L. Moshier.
#include "core/detmath.h"

#include <math.h>

static inline u64 f64bits(f64 f) { u64 u; memcpy(&u, &f, 8); return u; }
static inline f64 f64frombits(u64 u) { f64 f; memcpy(&f, &u, 8); return f; }
static inline bool is_nan(f64 x) { return x != x; }
#define DBLMAX 1.7976931348623157e308
static inline bool isinf_any(f64 x) { return x > DBLMAX || x < -DBLMAX; }
static inline bool isinf_pos(f64 x) { return x > DBLMAX; }
static inline bool isinf_neg(f64 x) { return x < -DBLMAX; }
static inline bool signbit_(f64 x) { return (f64bits(x) >> 63) != 0; }
static inline f64 nan_(void) { return f64frombits(0x7FF8000000000001ull); }  // Go math.NaN()
static inline f64 inf_(int s) { return s >= 0 ? f64frombits(0x7FF0000000000000ull) : f64frombits(0xFFF0000000000000ull); }
static inline f64 copysign_(f64 x, f64 y) { return f64frombits((f64bits(x) & ~(1ull << 63)) | (f64bits(y) & (1ull << 63))); }
static inline f64 fabs_(f64 x) { return f64frombits(f64bits(x) & ~(1ull << 63)); }

// ---------------------------------------------------------------- exp
static f64 expmulti(f64 hi, f64 lo, int k) {
    const f64 P1 = 1.66666666666666657415e-01, P2 = -2.77777777770155933842e-03,
              P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
              P5 = 4.13813679705723846039e-08;
    f64 r = hi - lo;
    f64 t = r * r;
    f64 c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    f64 y = 1 - ((lo - (r * c) / (2 - c)) - hi);
    return ldexp(y, k);
}

f64 cvf_exp(f64 x) {
    const f64 Ln2Hi = 6.93147180369123816490e-01, Ln2Lo = 1.90821492927058770002e-10,
              Log2e = 1.44269504088896338700e+00, Overflow = 7.09782712893383973096e+02,
              Underflow = -7.45133219101941108420e+02, NearZero = 1.0 / (1 << 28);
    if (is_nan(x)) return x;
    if (x > Overflow) return inf_(1);
    if (x < Underflow) return 0;
    if (-NearZero < x && x < NearZero) return 1 + x;
    i64 k = 0;
    if (x < 0) k = (i64)(Log2e * x - 0.5);
    else if (x > 0) k = (i64)(Log2e * x + 0.5);
    f64 hi = x - (f64)k * Ln2Hi;
    f64 lo = (f64)k * Ln2Lo;
    return expmulti(hi, lo, (int)k);
}

// ---------------------------------------------------------------- log
f64 cvf_ln(f64 x) {
    const f64 Ln2Hi = 6.93147180369123816490e-01, Ln2Lo = 1.90821492927058770002e-10,
              L1 = 6.666666666666735130e-01, L2 = 3.999999999940941908e-01,
              L3 = 2.857142874366239149e-01, L4 = 2.222219843214978396e-01,
              L5 = 1.818357216161805012e-01, L6 = 1.531383769920937332e-01,
              L7 = 1.479819860511658591e-01;
    if (is_nan(x) || isinf_pos(x)) return x;
    if (x < 0) return nan_();
    if (x == 0) return inf_(-1);
    int ki;
    f64 f1 = frexp(x, &ki);
    if (f1 < GO_SQRT2_2) {
        f1 *= 2;
        ki--;
    }
    f64 f = f1 - 1;
    f64 k = (f64)ki;
    f64 s = f / (2 + f);
    f64 s2 = s * s;
    f64 s4 = s2 * s2;
    f64 t1 = s2 * (L1 + s4 * (L3 + s4 * (L5 + s4 * L7)));
    f64 t2 = s4 * (L2 + s4 * (L4 + s4 * L6));
    f64 R = t1 + t2;
    f64 hfsq = 0.5 * f * f;
    return k * Ln2Hi - ((hfsq - (s * (hfsq + R) + k * Ln2Lo)) - f);
}

f64 cvf_log2(f64 x) {
    int e;
    f64 frac = frexp(x, &e);
    if (frac == 0.5) return (f64)(e - 1);
    return cvf_ln(frac) * GO_INV_LN2 + (f64)e;
}

// ---------------------------------------------------------------- pow
static bool is_odd_int(f64 x) {
    if (fabs_(x) >= (f64)(1ull << 53)) return false;
    f64 xi;
    f64 xf = modf(x, &xi);
    return xf == 0 && ((i64)xi & 1) == 1;
}

f64 cvf_pow(f64 x, f64 y) {
    if (y == 0 || x == 1) return 1;
    if (y == 1) return x;
    if (is_nan(x) || is_nan(y)) return nan_();
    if (x == 0) {
        if (y < 0) {
            if (signbit_(x) && is_odd_int(y)) return inf_(-1);
            return inf_(1);
        }
        if (y > 0) {
            if (signbit_(x) && is_odd_int(y)) return x;
            return 0;
        }
    } else if (isinf_any(y)) {
        if (x == -1) return 1;
        if ((fabs_(x) < 1) == isinf_pos(y)) return 0;
        return inf_(1);
    } else if (isinf_any(x)) {
        if (isinf_neg(x)) return cvf_pow(1 / x, -y);
        if (y < 0) return 0;
        if (y > 0) return inf_(1);
    } else if (y == 0.5) {
        return sqrt(x);
    } else if (y == -0.5) {
        return 1 / sqrt(x);
    }

    f64 yi;
    f64 yf = modf(fabs_(y), &yi);
    if (yf != 0 && x < 0) return nan_();
    if (yi >= 9223372036854775808.0) {
        if (x == -1) return 1;
        if ((fabs_(x) < 1) == (y > 0)) return 0;
        return inf_(1);
    }

    f64 a1 = 1.0;
    int ae = 0;
    if (yf != 0) {
        if (yf > 0.5) {
            yf--;
            yi++;
        }
        a1 = cvf_exp(yf * cvf_ln(x));
    }

    int xe;
    f64 x1 = frexp(x, &xe);
    for (i64 i = (i64)yi; i != 0; i >>= 1) {
        if (xe < -(1 << 12) || (1 << 12) < xe) {
            ae += xe;
            break;
        }
        if ((i & 1) == 1) {
            a1 *= x1;
            ae += xe;
        }
        x1 *= x1;
        xe <<= 1;
        if (x1 < .5) {
            x1 += x1;
            xe--;
        }
    }
    if (y < 0) {
        a1 = 1 / a1;
        ae = -ae;
    }
    return ldexp(a1, ae);
}

f64 cvf_hypot(f64 p, f64 q) {
    p = fabs_(p);
    q = fabs_(q);
    if (isinf_pos(p) || isinf_pos(q)) return inf_(1);
    if (is_nan(p) || is_nan(q)) return nan_();
    if (p < q) { f64 t = p; p = q; q = t; }
    if (p == 0) return 0;
    q = q / p;
    return p * sqrt(1 + q * q);
}

// ---------------------------------------------------------------- trig
static const f64 sin_c[] = {
    1.58962301576546568060e-10, -2.50507477628578072866e-8, 2.75573136213857245213e-6,
    -1.98412698295895385996e-4, 8.33333333332211858878e-3, -1.66666666666666307295e-1,
};
static const f64 cos_c[] = {
    -1.13585365213876817300e-11, 2.08757008419747316778e-9, -2.75573141792967388112e-7,
    2.48015872888517045348e-5, -1.38888888888730564116e-3, 4.16666666666665929218e-2,
};
#define PI4A 7.85398125648498535156e-1
#define PI4B 3.77489470793079817668e-8
#define PI4C 2.69515142907905952645e-15
#define REDUCE_THRESHOLD ((f64)(1 << 29))
#define FLOAT_MASK 0x7FF
#define FLOAT_SHIFT (64 - 11 - 1)
#define FLOAT_BIAS 1023

static const u64 mpi4[] = {
    0x0000000000000001ull, 0x45f306dc9c882a53ull, 0xf84eafa3ea69bb81ull, 0xb6c52b3278872083ull,
    0xfca2c757bd778ac3ull, 0x6e48dc74849ba5c0ull, 0x0c925dd413a32439ull, 0xfc3bd63962534e7dull,
    0xd1046bea5d768909ull, 0xd338e04d68befc82ull, 0x7323ac7306a673e9ull, 0x3908bf177bf25076ull,
    0x3ff12fffbc0b301full, 0xde5e2316b414da3eull, 0xda6cfd9e4f96136eull, 0x9e8c7ecd3cbfd45aull,
    0xea4f758fd7cbe2f6ull, 0x7a0e73ef14a525d4ull, 0xd7f6bf623f1aba10ull, 0xac06608df8f6d757ull,
};

static void mul64(u64 x, u64 y, u64 *hi, u64 *lo) {
#if defined(__SIZEOF_INT128__)
    unsigned __int128 p = (unsigned __int128)x * y;
    *hi = (u64)(p >> 64);
    *lo = (u64)p;
#else
    const u64 mask32 = 0xFFFFFFFFull;
    u64 x0 = x & mask32, x1 = x >> 32, y0 = y & mask32, y1 = y >> 32;
    u64 w0 = x0 * y0;
    u64 t = x1 * y0 + (w0 >> 32);
    u64 w1 = t & mask32, w2 = t >> 32;
    w1 += x0 * y1;
    *hi = x1 * y1 + w2 + (w1 >> 32);
    *lo = x * y;
#endif
}

static int clz64(u64 x) {
    if (x == 0) return 64;
    return __builtin_clzll(x);
}

static void trig_reduce(f64 x, u64 *jout, f64 *zout) {
    if (x < GO_PI_4) { *jout = 0; *zout = x; return; }
    u64 ix = f64bits(x);
    int exp = (int)((ix >> FLOAT_SHIFT) & FLOAT_MASK) - FLOAT_BIAS - FLOAT_SHIFT;
    ix &= ~((u64)FLOAT_MASK << FLOAT_SHIFT);
    ix |= 1ull << FLOAT_SHIFT;
    unsigned digit = (unsigned)(exp + 61) / 64, bitshift = (unsigned)(exp + 61) % 64;
    // Go's shifts by >= 64 yield 0; C's are undefined, so guard bitshift == 0.
#define SHL(a, s) ((s) >= 64 ? 0 : (a) << (s))
#define SHR(a, s) ((s) >= 64 ? 0 : (a) >> (s))
    u64 z0 = SHL(mpi4[digit], bitshift) | SHR(mpi4[digit + 1], 64 - bitshift);
    u64 z1 = SHL(mpi4[digit + 1], bitshift) | SHR(mpi4[digit + 2], 64 - bitshift);
    u64 z2 = SHL(mpi4[digit + 2], bitshift) | SHR(mpi4[digit + 3], 64 - bitshift);
    u64 z2hi, z2lo, z1hi, z1lo;
    mul64(z2, ix, &z2hi, &z2lo);
    mul64(z1, ix, &z1hi, &z1lo);
    u64 z0lo = z0 * ix;
    u64 lo = z1lo + z2hi;
    u64 c = lo < z1lo ? 1 : 0;
    u64 hi = z0lo + z1hi + c;
    u64 j = hi >> 61;
    hi = hi << 3 | lo >> 61;
    unsigned lz = (unsigned)clz64(hi);
    u64 e = (u64)(FLOAT_BIAS - (lz + 1));
    hi = SHL(hi, lz + 1) | SHR(lo, 64 - (lz + 1));
    hi >>= 64 - FLOAT_SHIFT;
    hi |= e << FLOAT_SHIFT;
    f64 z = f64frombits(hi);
    if ((j & 1) == 1) {
        j++;
        j &= 7;
        z--;
    }
    *jout = j;
    *zout = z * GO_PI_4;
#undef SHL
#undef SHR
}

f64 cvf_cos(f64 x) {
    if (is_nan(x) || isinf_any(x)) return nan_();
    bool sign = false;
    x = fabs_(x);
    u64 j;
    f64 y = 0, z;
    if (x >= REDUCE_THRESHOLD) {
        trig_reduce(x, &j, &z);
    } else {
        j = (u64)(x * GO_4_OVER_PI);
        y = (f64)j;
        if ((j & 1) == 1) { j++; y++; }
        j &= 7;
        z = ((x - y * PI4A) - y * PI4B) - y * PI4C;
    }
    if (j > 3) { j -= 4; sign = !sign; }
    if (j > 1) sign = !sign;
    f64 zz = z * z;
    if (j == 1 || j == 2)
        y = z + z * zz * ((((((sin_c[0] * zz) + sin_c[1]) * zz + sin_c[2]) * zz + sin_c[3]) * zz + sin_c[4]) * zz + sin_c[5]);
    else
        y = 1.0 - 0.5 * zz + zz * zz * ((((((cos_c[0] * zz) + cos_c[1]) * zz + cos_c[2]) * zz + cos_c[3]) * zz + cos_c[4]) * zz + cos_c[5]);
    if (sign) y = -y;
    return y;
}

f64 cvf_sin(f64 x) {
    if (x == 0 || is_nan(x)) return x;
    if (isinf_any(x)) return nan_();
    bool sign = false;
    if (x < 0) { x = -x; sign = true; }
    u64 j;
    f64 y = 0, z;
    if (x >= REDUCE_THRESHOLD) {
        trig_reduce(x, &j, &z);
    } else {
        j = (u64)(x * GO_4_OVER_PI);
        y = (f64)j;
        if ((j & 1) == 1) { j++; y++; }
        j &= 7;
        z = ((x - y * PI4A) - y * PI4B) - y * PI4C;
    }
    if (j > 3) { sign = !sign; j -= 4; }
    f64 zz = z * z;
    if (j == 1 || j == 2)
        y = 1.0 - 0.5 * zz + zz * zz * ((((((cos_c[0] * zz) + cos_c[1]) * zz + cos_c[2]) * zz + cos_c[3]) * zz + cos_c[4]) * zz + cos_c[5]);
    else
        y = z + z * zz * ((((((sin_c[0] * zz) + sin_c[1]) * zz + sin_c[2]) * zz + sin_c[3]) * zz + sin_c[4]) * zz + sin_c[5]);
    if (sign) y = -y;
    return y;
}

static const f64 tanP[] = {-1.30936939181383777646e4, 1.15351664838587416140e6, -1.79565251976484877988e7};
static const f64 tanQ[] = {1.00000000000000000000e0, 1.36812963470692954678e4, -1.32089234440210967447e6,
                           2.50083801823357915839e7, -5.38695755929454629881e7};

f64 cvf_tan(f64 x) {
    if (x == 0 || is_nan(x)) return x;
    if (isinf_any(x)) return nan_();
    bool sign = false;
    if (x < 0) { x = -x; sign = true; }
    u64 j;
    f64 y = 0, z;
    if (x >= REDUCE_THRESHOLD) {
        trig_reduce(x, &j, &z);
    } else {
        j = (u64)(x * GO_4_OVER_PI);
        y = (f64)j;
        if ((j & 1) == 1) { j++; y++; }
        z = ((x - y * PI4A) - y * PI4B) - y * PI4C;
    }
    f64 zz = z * z;
    if (zz > 1e-14)
        y = z + z * (zz * (((tanP[0] * zz) + tanP[1]) * zz + tanP[2]) / ((((zz + tanQ[1]) * zz + tanQ[2]) * zz + tanQ[3]) * zz + tanQ[4]));
    else
        y = z;
    if ((j & 2) == 2) y = -1 / y;
    if (sign) y = -y;
    return y;
}

static f64 xatan(f64 x) {
    const f64 P0 = -8.750608600031904122785e-01, P1 = -1.615753718733365076637e+01,
              P2 = -7.500855792314704667340e+01, P3 = -1.228866684490136173410e+02,
              P4 = -6.485021904942025371773e+01, Q0 = +2.485846490142306297962e+01,
              Q1 = +1.650270098316988542046e+02, Q2 = +4.328810604912902668951e+02,
              Q3 = +4.853903996359136964868e+02, Q4 = +1.945506571482613964425e+02;
    f64 z = x * x;
    z = z * ((((P0 * z + P1) * z + P2) * z + P3) * z + P4) / (((((z + Q0) * z + Q1) * z + Q2) * z + Q3) * z + Q4);
    z = x * z + x;
    return z;
}

static f64 satan(f64 x) {
    const f64 Morebits = 6.123233995736765886130e-17, Tan3pio8 = 2.41421356237309504880;
    if (x <= 0.66) return xatan(x);
    if (x > Tan3pio8) return GO_PI_2 - xatan(1 / x) + Morebits;
    return GO_PI_4 + xatan((x - 1) / (x + 1)) + 0.5 * Morebits;
}

f64 cvf_atan(f64 x) {
    if (x == 0) return x;
    if (x > 0) return satan(x);
    return -satan(-x);
}

f64 cvf_asin(f64 x) {
    if (x == 0) return x;
    bool sign = false;
    if (x < 0) { x = -x; sign = true; }
    if (x > 1) return nan_();
    f64 temp = sqrt(1 - x * x);
    if (x > 0.7) temp = GO_PI_2 - satan(temp / x);
    else temp = satan(x / temp);
    if (sign) temp = -temp;
    return temp;
}

f64 cvf_acos(f64 x) { return GO_PI_2 - cvf_asin(x); }

f64 cvf_atan2(f64 y, f64 x) {
    if (is_nan(y) || is_nan(x)) return nan_();
    if (y == 0) {
        if (x >= 0 && !signbit_(x)) return copysign_(0, y);
        return copysign_(GO_PI, y);
    }
    if (x == 0) return copysign_(GO_PI_2, y);
    if (isinf_any(x)) {
        if (isinf_pos(x)) {
            if (isinf_any(y)) return copysign_(GO_PI_4, y);
            return copysign_(0, y);
        }
        if (isinf_any(y)) return copysign_(GO_3PI_4, y);
        return copysign_(GO_PI, y);
    }
    if (isinf_any(y)) return copysign_(GO_PI_2, y);
    f64 q = cvf_atan(y / x);
    if (x < 0) {
        if (q <= 0) return q + GO_PI;
        return q - GO_PI;
    }
    return q;
}

f32 cvf_powf(f32 x, f32 y) {
    if (y == 0) return 1;
    if (y == 1) return x;
    if (y == 2) return x * x;
    if (y == 3) { f64 p = (f64)x; return (f32)(p * p * p); }
    if (y == 4) { f64 p = (f64)x; p *= p; return (f32)(p * p); }
    return (f32)cvf_pow((f64)x, (f64)y);
}
