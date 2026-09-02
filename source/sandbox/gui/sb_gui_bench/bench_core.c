/*==============================================================================================

    sandbox/gui/sb_gui_bench/bench_core.c -- registry, runner, aggregation, report.

    The scripted runner is a state machine ticked once per frame AFTER the present, so the
    frame just scored is fully built and submitted before any decision is made about the next:

        WARMUP   free-run for one second emitting the first case's scene (cold caches, shader
                 and atlas first-touch), then apply the first case
        SETTLE   config landed; burn frames (stats one-frame lag, ~2-frame gpu-timestamp
                 latency, theme re-bake all wash out here)
        MEASURE  capture one sample per frame; aggregate at the budget
        ...next case, until the registry is out; write the report and stop.

    Configuration is applied BETWEEN frames (theme_set clears the style stacks; levers must not
    move mid-build).  gpu samples of exactly zero are excluded -- hardware without graphics
    timestamps reads 0 there, and an all-zero column prints n/a rather than a free-looking 0.

==============================================================================================*/
// clang-format off

#define BENCH_COUNTOF( a ) ( ( u32 )( sizeof( a ) / sizeof( ( a )[ 0 ] ) ) )

/*==============================================================================================
    Registry -- every suite, in report order
==============================================================================================*/

typedef struct
{
    const bench_case_t* arr;
    u32                 count;

} bench_suite_tab_t;

static const bench_suite_tab_t k_bench_suites[] =
{
    { k_pipeline_cases, BENCH_COUNTOF( k_pipeline_cases ) },
    { k_text_cases,     BENCH_COUNTOF( k_text_cases     ) },
    { k_fill_cases,     BENCH_COUNTOF( k_fill_cases     ) },
    { k_op_cases,       BENCH_COUNTOF( k_op_cases       ) },
    { k_style_cases,    BENCH_COUNTOF( k_style_cases    ) },
};

static const bench_case_t* g_reg[ BENCH_MAX_CASES ];
static u32                 g_reg_count = 0;

static void
bench_registry_build( void )
{
    if ( g_reg_count )
        return;
    for ( u32 s = 0; s < BENCH_COUNTOF( k_bench_suites ); ++s )
        for ( u32 i = 0; i < k_bench_suites[ s ].count && g_reg_count < BENCH_MAX_CASES; ++i )
            g_reg[ g_reg_count++ ] = &k_bench_suites[ s ].arr[ i ];
}

/*==============================================================================================
    Command-line state -- set by main() before boot
==============================================================================================*/

static bool        s_arg_run     = false;   // -run: scripted suite, report, exit
static const char* s_arg_case    = NULL;    // -case <substr>: filter (marks the report PARTIAL)
static u32         s_arg_frames  = BENCH_MEASURE_DEFAULT;
static u32         s_arg_settle  = BENCH_SETTLE_DEFAULT;

static bool
bench_case_match( const bench_case_t* c )
{
    return !s_arg_case || strstr( c->name, s_arg_case ) != NULL
                       || strcmp( c->suite, s_arg_case ) == 0;
}

static void
bench_list_print( void )
{
    bench_registry_build();
    for ( u32 i = 0; i < g_reg_count; ++i )
        printf( "%-8s %-18s -- %s\n", g_reg[ i ]->suite, g_reg[ i ]->name, g_reg[ i ]->note );
}

/*==============================================================================================
    Runner state
==============================================================================================*/

typedef enum
{
    BR_OFF = 0,
    BR_WARMUP,
    BR_SETTLE,
    BR_MEASURE,
    BR_DONE,

} bench_phase_t;

static struct
{
    bench_phase_t  phase;
    i32            cur;            // registry index of the running case
    u32            frames;         // frames spent in the current phase
    u32            gframe;         // monotonic frame counter handed to scene emitters
    f64            warm_t0;
    u32            done_count;     // matching cases finished, for the progress line
    u32            match_total;
    bool           theme_missing;  // theme_set failed for the running case

    bench_sample_t samples[ BENCH_MAX_SAMPLES ];
    u32            sample_count;

    bench_result_t results[ BENCH_MAX_CASES ];
    u32            result_count;

} s_run;

/* Frame-time / emit-time EMAs for the interactive readout (main() feeds them every frame; emit
   lags one frame behind dt since it isn't known until after bench_frame runs). */
static f32 s_dt_avg   = 0.0f;
static f32 s_emit_avg = 0.0f;

static u32
bench_measure_frames( void )
{
    return s_arg_frames < BENCH_MAX_SAMPLES ? s_arg_frames : BENCH_MAX_SAMPLES;
}

static i32
bench_next_match( i32 from )
{
    for ( i32 i = from + 1; i < ( i32 )g_reg_count; ++i )
        if ( bench_case_match( g_reg[ i ] ) )
            return i;
    return -1;
}

/* Between-frames configuration: the theme swap clears the style stacks and re-bakes the color
   grid, and the retained lever must hold still across a whole build. */
static void
bench_apply_case( const bench_case_t* c )
{
    s_run.theme_missing = !gui()->theme_set( c->theme ? c->theme : "dark" );
    gui()->set_retained_skip( c->retained_on );
}

/*==============================================================================================
    Aggregation
==============================================================================================*/

static int
bench_cmp_f64( const void* a, const void* b )
{
    f64 d = *( const f64* )a - *( const f64* )b;
    return d < 0.0 ? -1 : ( d > 0.0 ? 1 : 0 );
}

static bench_agg_t
bench_aggregate( f64* v, u32 n )
{
    bench_agg_t a = { 0 };
    if ( n == 0 )
        return a;

    qsort( v, n, sizeof( f64 ), bench_cmp_f64 );

    f64 sum = 0.0;
    for ( u32 i = 0; i < n; ++i )
        sum += v[ i ];

    a.med = v[ n / 2 ];
    a.avg = sum / ( f64 )n;
    a.mn  = v[ 0 ];
    a.p95 = v[ ( u32 )( 0.95 * ( f64 )( n - 1 ) ) ];
    return a;
}

static void
bench_finish_case( const bench_case_t* c )
{
    static f64 col[ BENCH_MAX_SAMPLES ];

    bench_result_t* r = &s_run.results[ s_run.result_count++ ];
    memset( r, 0, sizeof( *r ) );
    r->c             = c;
    r->frames        = s_run.sample_count;
    r->theme_missing = s_run.theme_missing;

    u32 n = s_run.sample_count;

    for ( u32 i = 0; i < n; ++i ) col[ i ] = s_run.samples[ i ].emit_ms;
    r->emit = bench_aggregate( col, n );
    for ( u32 i = 0; i < n; ++i ) col[ i ] = ( f64 )s_run.samples[ i ].diff_ms;
    r->diff = bench_aggregate( col, n );
    for ( u32 i = 0; i < n; ++i ) col[ i ] = ( f64 )s_run.samples[ i ].tess_ms;
    r->tess = bench_aggregate( col, n );
    for ( u32 i = 0; i < n; ++i ) col[ i ] = ( f64 )s_run.samples[ i ].submit_ms;
    r->submit = bench_aggregate( col, n );

    /* gpu: only frames that produced a reading; none at all = unsupported hardware. */
    u32 gn = 0;
    for ( u32 i = 0; i < n; ++i )
        if ( s_run.samples[ i ].gpu_ms > 0.0f )
            col[ gn++ ] = ( f64 )s_run.samples[ i ].gpu_ms;
    r->gpu        = bench_aggregate( col, gn );
    r->gpu_frames = gn;

    if ( n )
    {
        const bench_sample_t* last = &s_run.samples[ n - 1 ];
        r->draw_calls     = last->draw_calls;
        r->quad_count     = last->quad_count;
        r->prim_count     = last->prim_count;
        r->upload_bytes   = last->upload_bytes;
        r->upload_batches = last->upload_batches;
    }

    if ( c->emit_fn == scene_text_wall && c->param == BENCH_TEXT_OUTLINE && !s_bench_font_sdf )
    {
        r->skipped     = true;
        r->skip_reason = "no SDF font bake found; ran plain";
    }

    /* A saturated build pool means content was DROPPED and this case measured less than it
       emitted.  The gui's own overflow warning latches once per run, so a second offender
       would pass silently -- check the physical fills against the caps here, every case. */
    {
        gui_render_stats_t rs = gui()->render_stats();
        r->pool_full = ( rs.cmd_count_all  >= GUI_MAX_CMDS  )
                    || ( rs.quad_count_all >= GUI_MAX_QUADS )
                    || ( rs.prim_count_all >= GUI_MAX_PRIMS );
        if ( r->pool_full )
            printf( "        ^ POOL FULL (cmds %u/%u quads %u/%u styles %u/%u) -- row invalid\n",
                    rs.cmd_count_all, ( u32 )GUI_MAX_CMDS, rs.quad_count_all,
                    ( u32 )GUI_MAX_QUADS, rs.prim_count_all, ( u32 )GUI_MAX_PRIMS );
    }

    ++s_run.done_count;
    char gbuf[ 32 ];
    if ( gn ) snprintf( gbuf, sizeof( gbuf ), "%.3f", r->gpu.med );
    else      snprintf( gbuf, sizeof( gbuf ), "n/a" );
    printf( "[%2u/%2u] %-18s emit %6.3f ms  gpu %s ms\n",
            s_run.done_count, s_run.match_total, c->name, r->emit.med, gbuf );
    fflush( stdout );
}

/*==============================================================================================
    Report
==============================================================================================*/

static char s_report[ 96 * 1024 ];
static u32  s_report_len = 0;

static void
rep( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    int n = vsnprintf( s_report + s_report_len, sizeof( s_report ) - s_report_len, fmt, args );
    va_end( args );
    if ( n > 0 && ( u32 )n < sizeof( s_report ) - s_report_len )
        s_report_len += ( u32 )n;
}

/* gpu columns print n/a on hardware with no graphics-queue timestamps.  Sixteen rotating
   buffers because one rep() line uses up to eight results at once. */
static const char*
rep_ms( f64 v, bool na )
{
    static char buf[ 16 ][ 16 ];
    static u32  slot = 0;
    char* b = buf[ slot++ & 15 ];
    if ( na ) snprintf( b, 16, "    n/a" );
    else      snprintf( b, 16, "%7.3f", v );
    return b;
}

static const bench_result_t*
rep_find( const char* name )
{
    for ( u32 i = 0; i < s_run.result_count; ++i )
        if ( strcmp( s_run.results[ i ].c->name, name ) == 0 )
            return &s_run.results[ i ];
    return NULL;
}

static void
bench_report_build( void )
{
    SysDateTime dt;
    sys_datetime_local( &dt );

#if defined( _DEBUG )
    const char* cfg = "Debug";
#elif defined( NDEBUG )
    const char* cfg = "Release";
#else
    const char* cfg = "Unknown";
#endif

    rep( "gui_bench  %04u-%02u-%02u %02u:%02u:%02u  build=%s  %ux%u  settle=%u  measure=%u\n",
         dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, cfg,
         ( u32 )BENCH_HOST_W, ( u32 )BENCH_HOST_H, s_arg_settle, bench_measure_frames() );
    if ( s_arg_case )
        rep( "PARTIAL run: -case %s\n", s_arg_case );
    rep( "medians below; spread (med/avg/min/p95) at the bottom.  gpu trails ~2 frames and is\n"
         "GPU execution time, valid at any present cadence.  Only same-config runs compare.\n" );

    const char* suite = "";
    for ( u32 i = 0; i < s_run.result_count; ++i )
    {
        const bench_result_t* r = &s_run.results[ i ];
        if ( strcmp( suite, r->c->suite ) != 0 )
        {
            suite = r->c->suite;
            rep( "\n== %s %.*s\n", suite,
                 ( int )( 86 - strlen( suite ) ),
                 "======================================================================================" );
            rep( "%-18s %7s %7s %7s %7s %7s %7s %6s %6s %6s  %s\n",
                 "case", "emit", "diff", "tess", "submit", "gpu",
                 "quads", "prims", "draws", "upKB", "note" );
        }

        char tail[ 160 ] = "";
        if ( r->theme_missing )
            snprintf( tail, sizeof( tail ), "  [THEME MISSING]" );
        if ( r->skipped )
        {
            size_t off = strlen( tail );
            snprintf( tail + off, sizeof( tail ) - off, "  [SKIP: %s]", r->skip_reason );
        }
        if ( r->pool_full )
        {
            size_t off = strlen( tail );
            snprintf( tail + off, sizeof( tail ) - off, "  [POOL FULL -- INVALID]" );
        }

        bool gna = ( r->gpu_frames == 0 );
        rep( "%-18s %s %s %s %s %s %7u %6u %6u %6u  %s%s\n",
             r->c->name,
             rep_ms( r->emit.med,   false ),
             rep_ms( r->diff.med,   false ),
             rep_ms( r->tess.med,   false ),
             rep_ms( r->submit.med, false ),
             rep_ms( r->gpu.med,    gna ),
             r->quad_count, r->prim_count, r->draw_calls, r->upload_bytes / 1024u,
             r->c->note, tail );
    }

    /* Derived: the retained cache's ROI, straight off the static-scene pair. */
    {
        const bench_result_t* d = rep_find( "diff_static_scene" );
        const bench_result_t* t = rep_find( "tess_static_scene" );
        if ( d && t )
            rep( "\nretained cache ROI: tess %0.3f ms -> %0.3f ms on the identical scene "
                 "(cache saves %0.3f ms/frame)\n",
                 t->tess.med, d->tess.med, t->tess.med - d->tess.med );
    }

    /* Derived: per-op fragment price against the flat fill. */
    {
        const bench_result_t* base = rep_find( "op_self" );
        if ( base && base->gpu_frames )
        {
            rep( "\n== op delta vs op_self (gpu med) =====================================================\n" );
            for ( u32 i = 0; i < s_run.result_count; ++i )
            {
                const bench_result_t* r = &s_run.results[ i ];
                if ( strcmp( r->c->suite, "op" ) != 0 )
                    continue;
                rep( "%-18s %s  %+0.3f\n", r->c->name,
                     rep_ms( r->gpu.med, r->gpu_frames == 0 ),
                     r->gpu.med - base->gpu.med );
            }
            rep( "(equal cell coverage per config; a field shades its whole quad footprint.\n"
                 " A NEGATIVE delta is real: sparse ops (ring, dash, repeat, seg) leave most of\n"
                 " the cell empty and the zero-coverage discard kills those pixels early)\n" );
        }
    }

    rep( "\n== spread ============================================================================\n" );
    rep( "%-18s  %-31s  %-31s\n", "case", "emit med/avg/min/p95", "gpu med/avg/min/p95" );
    for ( u32 i = 0; i < s_run.result_count; ++i )
    {
        const bench_result_t* r   = &s_run.results[ i ];
        bool                  gna = ( r->gpu_frames == 0 );
        rep( "%-18s  %s %s %s %s  %s %s %s %s\n", r->c->name,
             rep_ms( r->emit.med, false ), rep_ms( r->emit.avg, false ),
             rep_ms( r->emit.mn,  false ), rep_ms( r->emit.p95, false ),
             rep_ms( r->gpu.med, gna ), rep_ms( r->gpu.avg, gna ),
             rep_ms( r->gpu.mn,  gna ), rep_ms( r->gpu.p95, gna ) );
    }
}

static void
bench_report_write( void )
{
    bench_report_build();

    /* Mirror to stdout first -- the file may still fail, and the console copy is the fallback. */
    fwrite( s_report, 1, s_report_len, stdout );
    fflush( stdout );

    char dir[ 512 ];
    snprintf( dir, sizeof( dir ), "%s" PATH_SEP BENCH_OUT_DIR, sys_root_dir() );
    if ( !sys_dir_make( dir ) )
    {
        fprintf( stderr, "[sb_gui_bench] could not create %s -- report not saved\n", dir );
        return;
    }

    SysDateTime dt;
    sys_datetime_local( &dt );
    char path[ 640 ];
    snprintf( path, sizeof( path ), "%s" PATH_SEP "gui_bench_%04u%02u%02u_%02u%02u%02u.txt",
              dir, dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second );

    if ( sys_file_write_entire( path, s_report, s_report_len ) )
        printf( "[sb_gui_bench] wrote %s\n", path );
    else
        fprintf( stderr, "[sb_gui_bench] write failed: %s\n", path );
    fflush( stdout );
}

/*==============================================================================================
    Scripted runner
==============================================================================================*/

/* Arm the sweep.  Returns false when the filter matches nothing. */
static bool
bench_run_start( void )
{
    bench_registry_build();

    s_run.match_total = 0;
    for ( u32 i = 0; i < g_reg_count; ++i )
        if ( bench_case_match( g_reg[ i ] ) )
            ++s_run.match_total;
    if ( s_run.match_total == 0 )
    {
        fprintf( stderr, "[sb_gui_bench] -case '%s' matches nothing (see -list)\n",
                 s_arg_case ? s_arg_case : "" );
        return false;
    }

    s_run.cur     = bench_next_match( -1 );
    s_run.phase   = BR_WARMUP;
    s_run.warm_t0 = sys_tick_seconds();
    s_run.frames  = 0;

    printf( "[sb_gui_bench] %u case%s, settle %u + measure %u frames each\n",
            s_run.match_total, s_run.match_total == 1 ? "" : "s",
            s_arg_settle, bench_measure_frames() );
    fflush( stdout );
    return true;
}

/* One frame of the sweep, called after the present.  emit_ms is the host's bracket around this
   frame's frame_begin..frame_end.  Returns false once the report is out. */
static bool
bench_tick( f64 emit_ms )
{
    if ( s_run.phase == BR_DONE || s_run.phase == BR_OFF )
        return false;

    ++s_run.frames;

    switch ( s_run.phase )
    {
        case BR_WARMUP:
            if ( sys_tick_seconds() - s_run.warm_t0 < BENCH_WARMUP_SECONDS )
                break;
            bench_apply_case( g_reg[ s_run.cur ] );
            s_run.phase  = BR_SETTLE;
            s_run.frames = 0;
            break;

        case BR_SETTLE:
            if ( s_run.frames < s_arg_settle )
                break;
            s_run.phase        = BR_MEASURE;
            s_run.frames       = 0;
            s_run.sample_count = 0;
            break;

        case BR_MEASURE:
        {
            gui_render_stats_t rs = gui()->render_stats();
            bench_sample_t*    s  = &s_run.samples[ s_run.sample_count++ ];
            s->emit_ms        = emit_ms;
            s->diff_ms        = rs.diff_ms;
            s->tess_ms        = rs.tess_ms;
            s->submit_ms      = rs.submit_ms;
            s->gpu_ms         = rs.gpu_ms;
            s->draw_calls     = rs.draw_calls;
            s->quad_count     = rs.quad_count;
            s->prim_count     = rs.prim_unique;   /* the per-slot arena; the stored pool is a separate axis */
            s->upload_bytes   = rs.upload_bytes;
            s->upload_batches = rs.upload_batches;

            if ( s_run.sample_count < bench_measure_frames() )
                break;

            bench_finish_case( g_reg[ s_run.cur ] );

            s_run.cur = bench_next_match( s_run.cur );
            if ( s_run.cur < 0 )
            {
                gui()->set_retained_skip( true );
                bench_report_write();
                s_run.phase = BR_DONE;
                return false;
            }
            bench_apply_case( g_reg[ s_run.cur ] );
            s_run.phase  = BR_SETTLE;
            s_run.frames = 0;
            break;
        }

        default:
            break;
    }

    return true;
}

/*==============================================================================================
    Per-frame build -- scripted emits the active case; interactive shows a picker
==============================================================================================*/

static i32 s_ui_case = -1;   // interactive: registry index of the looping case, -1 = none

static void
bench_interactive_frame( void )
{
    bench_registry_build();

    gui()->window_set_next_pos ( BENCH_PICKER_X, BENCH_PICKER_Y, GUI_COND_ONCE );
    gui()->window_set_next_size( BENCH_PICKER_W, BENCH_PICKER_H, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Picker", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Click a case to loop it live.  Run the suite" );
        gui()->text( "unattended with: sb_gui_bench -run" );
        f32 ms = s_dt_avg * 1000.0f;
        gui()->textf( "frame %.2f ms (%.0f fps)", ms, ms > 0.001f ? 1000.0f / ms : 0.0f );

        gui_render_stats_t rs = gui()->render_stats();
        gui()->textf( "emit-side stats (last frame):" );
        gui()->textf( "  emit %.3f", s_emit_avg * 1000.0f );
        gui()->textf( "  diff %.3f  tess %.3f  submit %.3f ms",
                      rs.diff_ms, rs.tess_ms, rs.submit_ms );
        gui()->textf( "  gpu %.3f ms  quads %u  draws %u", rs.gpu_ms, rs.quad_count,
                      rs.draw_calls );
        gui()->separator();

        if ( s_ui_case >= 0 )
        {
            gui()->textf( "looping: %s", g_reg[ s_ui_case ]->name );
            if ( gui()->button( "stop" ) )
            {
                s_ui_case = -1;
                if ( !gui()->debug_hotkeys_armed() )
                {
                    gui()->set_force_redraw( false );
                    gui()->set_retained_skip( true );
                }
            }
            gui()->separator();
        }

        const char* suite = "";
        for ( u32 i = 0; i < g_reg_count; ++i )
        {
            if ( strcmp( suite, g_reg[ i ]->suite ) != 0 )
            {
                suite = g_reg[ i ]->suite;
                gui()->stack();
                gui()->separator_text( suite );
                gui()->cols_n( 2 );
            }
            gui()->push_id_int( ( i32 )i );
            if ( gui()->button( g_reg[ i ]->name ) && ( i32 )i != s_ui_case )
            {
                s_ui_case = ( i32 )i;
                /* Edge-triggered lever writes, and never while the debug selector menu owns
                   them -- a per-frame write would clobber its checkboxes. */
                if ( !gui()->debug_hotkeys_armed() )
                {
                    gui()->set_force_redraw( true );
                    gui()->set_retained_skip( g_reg[ i ]->retained_on );
                    gui()->theme_set( g_reg[ i ]->theme ? g_reg[ i ]->theme : "dark" );
                }
            }
            gui()->pop_id();
        }
    }
    gui()->window_end();

    if ( s_ui_case >= 0 )
        g_reg[ s_ui_case ]->emit_fn( g_reg[ s_ui_case ], s_run.gframe );
}

/* The one call main() makes inside the context each frame. */
static void
bench_frame( void )
{
    ++s_run.gframe;

    if ( s_arg_run )
    {
        if ( s_run.cur >= 0 )
            g_reg[ s_run.cur ]->emit_fn( g_reg[ s_run.cur ], s_run.gframe );
        return;
    }
    bench_interactive_frame();
}

/*==============================================================================================
    Post-boot assets -- the SDF face for text_outline, the baked disc for op_tex_shape
==============================================================================================*/

/* font_load ACTIVATES what it loads, so the caller's font is put back (sb_gui_sdf pattern). */
static u32
bench_font_try( const char* name )
{
    u32 prev = gui()->font_active_id();
    u32 id   = gui()->font_load( name );
    gui()->font_use( prev );
    return id;
}

static void
bench_assets_init( void )
{
    s_bench_font_sdf = bench_font_try( RID( "font/cascadiamono/32.sdf" ) );
    if ( !s_bench_font_sdf )
        printf( "[sb_gui_bench] no SDF font bake -- text_outline will run plain and be marked\n" );
    bench_shape_init();
}

// clang-format on
/*============================================================================================*/
