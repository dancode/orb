/*==============================================================================================

    math.h -- Integer and floating-point math utilities.

    Explicit, type-safe API (no C11 _Generic).
    Naming scheme: math_<type>_<operation>

    Example:
        math_i32_min(a, b)
        math_f32_abs(x)

==============================================================================================*/
#ifndef MATH_H
#define MATH_H

// clang-format off
/*==============================================================================================
     Min / Max
==============================================================================================*/

// i32
static inline i32 math_i32_min(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 math_i32_max(i32 a, i32 b) { return a > b ? a : b; }

// i64
static inline i64 math_i64_min(i64 a, i64 b) { return a < b ? a : b; }
static inline i64 math_i64_max(i64 a, i64 b) { return a > b ? a : b; }

// u32
static inline u32 math_u32_min(u32 a, u32 b) { return a < b ? a : b; }
static inline u32 math_u32_max(u32 a, u32 b) { return a > b ? a : b; }

// u64
static inline u64 math_u64_min(u64 a, u64 b) { return a < b ? a : b; }
static inline u64 math_u64_max(u64 a, u64 b) { return a > b ? a : b; }

// f32
static inline f32 math_f32_min(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 math_f32_max(f32 a, f32 b) { return a > b ? a : b; }

// f64
static inline f64 math_f64_min(f64 a, f64 b) { return a < b ? a : b; }
static inline f64 math_f64_max(f64 a, f64 b) { return a > b ? a : b; }

/*==============================================================================================
    Clamp
==============================================================================================*/

static inline i32 math_i32_clamp(i32 v, i32 lo, i32 hi) { return math_i32_min(math_i32_max(v, lo), hi); }
static inline i64 math_i64_clamp(i64 v, i64 lo, i64 hi) { return math_i64_min(math_i64_max(v, lo), hi); }
static inline u32 math_u32_clamp(u32 v, u32 lo, u32 hi) { return math_u32_min(math_u32_max(v, lo), hi); }
static inline u64 math_u64_clamp(u64 v, u64 lo, u64 hi) { return math_u64_min(math_u64_max(v, lo), hi); }
static inline f32 math_f32_clamp(f32 v, f32 lo, f32 hi) { return math_f32_min(math_f32_max(v, lo), hi); }
static inline f64 math_f64_clamp(f64 v, f64 lo, f64 hi) { return math_f64_min(math_f64_max(v, lo), hi); }

/*==============================================================================================
    Absolute value
==============================================================================================*/

static inline i32 math_i32_abs(i32 a) { return a < 0 ? -a : a; }
static inline i64 math_i64_abs(i64 a) { return a < 0 ? -a : a; }
static inline f32 math_f32_abs(f32 a) { return a < 0.0f ? -a : a; }
static inline f64 math_f64_abs(f64 a) { return a < 0.0  ? -a : a; }

/*==============================================================================================
    Alignment (power-of-two)
==============================================================================================*/

// Align v up to next multiple of align (align must be power of two).
#define math_align_up(v, align)   ( ((v) + (align) - 1) & ~((align) - 1) )

// Align v down to nearest multiple of align (align must be power of two).
#define math_align_down(v, align) ( (v) & ~((align) - 1) )

/*==============================================================================================
    Interpolation
==============================================================================================*/

// Linear interpolation
static inline f32 math_f32_lerp(f32 a, f32 b, f32 t)
{
    return a + t * (b - a);
}

static inline f64 math_f64_lerp(f64 a, f64 b, f64 t)
{
    return a + t * (b - a);
}

// Inverse lerp
static inline f32 math_f32_unlerp(f32 a, f32 b, f32 v)
{
    /* Returns the t (time) value such that math_f32_lerp(a, b, t) == v. */
    return (v - a) / (b - a);
}

// Remap
static inline f32 math_f32_remap(f32 in_lo, f32 in_hi, f32 out_lo, f32 out_hi, f32 v)
{
    /* Maps v from range [in_lo, in_hi] to range [out_lo, out_hi]. */
    return math_f32_lerp(out_lo, out_hi, math_f32_unlerp(in_lo, in_hi, v));
}

/*==============================================================================================
    Sign
==============================================================================================*/

static inline i32 math_i32_sign(i32 x)
{
    return (x > 0) - (x < 0);
}

static inline f32 math_f32_sign(f32 x)
{
    return (f32)((x > 0.0f) - (x < 0.0f));
}
/*==============================================================================================
    Float comparison
==============================================================================================*/

#define F32_EPSILON 1e-6f /* A small value used as tolerance for floating-point comparisons */

/* Returns 1 if a and b are within eps of each other, 0 otherwise. */
static inline int math_f32_nearly_equal(f32 a, f32 b, f32 eps)
{
    f32 d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

// ============================================================
// Compile-time integer log2 (for constants only)
// ============================================================

#define MATH_LOG2_CONST(n) \
    ((n) <= (1<<0)  ? 0  : (n) <= (1<<1)  ? 1  : (n) <= (1<<2)  ? 2  : \
     (n) <= (1<<3)  ? 3  : (n) <= (1<<4)  ? 4  : (n) <= (1<<5)  ? 5  : \
     (n) <= (1<<6)  ? 6  : (n) <= (1<<7)  ? 7  : (n) <= (1<<8)  ? 8  : \
     (n) <= (1<<9)  ? 9  : (n) <= (1<<10) ? 10 : (n) <= (1<<11) ? 11 : \
     (n) <= (1<<12) ? 12 : (n) <= (1<<13) ? 13 : (n) <= (1<<14) ? 14 : \
     (n) <= (1<<15) ? 15 : (n) <= (1<<16) ? 16 : 31)

// clang-format on
/*============================================================================================*/
#endif    // MATH_H