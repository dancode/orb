/*==============================================================================================

    base/math_simd.h -- SIMD acceleration seam for the vector/matrix math.

    This file is the ONE place the math library reaches for hardware SIMD.  Today it is
    deliberately empty of intrinsics: every vec/mat/quat operation is written as plain scalar
    ORB_INLINE code in its own header, which the compiler auto-vectorizes well under /O2 (and
    /arch:AVX2).  That is fast enough for gizmos, cameras, 2D UI, and per-object transforms --
    the only math this engine does today.

    The point of the seam is future-proofing WITHOUT committing the public types to a register
    layout now:

      - The public vec4_t / mat4_t / quat_t stay ordinary structs (f32 fields + e[] access), so
        they reflect cleanly, cross the hot-reload DLL boundary, and memcpy straight into GPU
        push constants.  They are NOT typedef'd to __m128 -- that trap forces 16-byte alignment
        on every stored vector and fights the vec3 vertex layout.

      - They ARE ORB_ALIGNAS(16) (vec4/mat4/quat), so the day a profiler zone shows a real
        hot batch (skinning, particle transforms, physics broadphase), an SSE/AVX kernel can be
        dropped in behind the SAME function signatures -- no call site changes.

    Rule: SIMD belongs to batch kernels that process many elements (structure-of-arrays, driven
    by the job system), not to the scalar single-value ops a call site uses one at a time.  When
    that day comes, add ORB_MATH_SIMD paths here and branch the hot ops on it.  Until a
    measurement demands it, this stays scalar.

==============================================================================================*/
#ifndef MATH_SIMD_H
#define MATH_SIMD_H

// clang-format off

/* No SIMD backend selected yet -- the math ops compile as scalar.  Flip to 1 and fill in the
   intrinsic paths (guarded per COMPILER_*) only when a measured hot path justifies it. */
#define ORB_MATH_SIMD 0

/*============================================================================================*/
#endif    // MATH_SIMD_H
