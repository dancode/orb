/*==============================================================================================

    sandbox/gui/sb_gui_bench/bench.h -- shared types for the gui benchmark suite.

    A case is one steady-state workload measured over a fixed frame budget.  The runner
    (bench_core.c) applies its configuration between frames, burns settle frames, captures one
    sample per measured frame, and aggregates.  Everything here is data; the behavior lives in
    the sibling .c files, unity-included by sb_gui_bench.c.

==============================================================================================*/
#ifndef SB_GUI_BENCH_H
#define SB_GUI_BENCH_H

// clang-format off

#if OS_WINDOWS
    #define PATH_SEP "\\"
#else
    #define PATH_SEP "/"
#endif

/* Result files are KEPT output, not scratch -- they land in artifacts/ (gitignored, survives a
   temp/ wipe), one date-stamped file per run so old results accumulate. */
#define BENCH_OUT_DIR      "artifacts" PATH_SEP "bench"

#define BENCH_MAX_SAMPLES  1024   // per-case sample cap; measure_frames clamps to it
#define BENCH_MAX_CASES    64     // registry cap across every suite

#define BENCH_SETTLE_DEFAULT   30    // frames burned after a case's config lands (absorbs the
                                     //   one-frame stats lag, the ~2-frame gpu-timestamp
                                     //   latency, and a theme re-bake)
#define BENCH_MEASURE_DEFAULT  120   // frames captured per case
#define BENCH_WARMUP_SECONDS   1.0   // free-run before the first case (cold caches, first-touch
                                     //   pipeline + atlas work; the sb_gui_stress precedent)

typedef struct bench_case_s bench_case_t;

/* A scene emitter runs once per frame inside the default context.  `frame` counts from zero at
   each phase change, so a deliberately-animated scene derives its motion from it and stays
   deterministic (never from wall time). */
typedef void ( *bench_emit_fn )( const bench_case_t* c, u32 frame );

struct bench_case_s
{
    const char*   suite;              // "pipeline" | "text" | "op" | "fill" | "style"
    const char*   name;               // unique key: the report row and the -case substring match
    const char*   note;               // one-liner echoed beside the row
    bool          retained_on;        // DEFAULT FALSE: the retained geometry cache is OFF, so
                                      //   the whole pipeline (emit, diff, tessellate, upload,
                                      //   submit, GPU) runs fresh every measured frame.  true
                                      //   only for a case that deliberately measures the
                                      //   retained replay path itself.
    const char*   theme;              // theme_set at apply; NULL = "dark"
    bench_emit_fn emit_fn;            // the scene; must ALWAYS emit something (an empty frame
                                      //   skips the flush and records no stats at all)
    u32           param;              // case knob: layer count, op config index, row count...
};

/* One measured frame.  emit_ms is the host's own bracket around frame_begin..frame_end (the
   same seam the internal perf overlay times); the rest is gui()->render_stats(), which at any
   steady state describes the same workload despite its one-frame publish lag. */
typedef struct
{
    f64 emit_ms;
    f32 diff_ms, tess_ms, submit_ms, gpu_ms;
    u32 draw_calls, quad_count, prim_count;
    u32 upload_bytes, upload_batches;

} bench_sample_t;

/* med/avg/min/p95 of one metric column.  Median is the primary comparator -- emit and gpu both
   spike on compositor / power-state noise, and a mean alone makes two identical runs look
   different. */
typedef struct
{
    f64 med, avg, mn, p95;

} bench_agg_t;

typedef struct
{
    const bench_case_t* c;
    u32                 frames;         // samples captured
    u32                 gpu_frames;     // samples with a non-zero gpu reading
    bool                theme_missing;  // theme_set failed; row is flagged
    bool                skipped;        // case could not run (e.g. no SDF font bake)
    const char*         skip_reason;
    bool                pool_full;      // a build pool saturated during measurement: content
                                        //   was dropped and this row's numbers are INVALID

    bench_agg_t         emit, diff, tess, submit, gpu;

    /* Counts are deterministic in steady state -- last sample's value, no statistics. */
    u32                 draw_calls, quad_count, prim_count;
    u32                 upload_bytes, upload_batches;

} bench_result_t;

// clang-format on

#endif /* SB_GUI_BENCH_H */
