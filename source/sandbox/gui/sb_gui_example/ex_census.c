/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_census.c -- scripted style-record census sweep.

    Drives the whole demo registry with no hands on it: for each theme, open one demo, hold it for
    a fixed number of frames, close it, move to the next; when the registry is exhausted, dump the
    census under that theme's name, clear, and start the next theme.  The process exits when the
    last theme finishes.

    Why a driver rather than clicking through the explorer:

      - REPEATABILITY.  The palette bake is written from this dump, so the dump has to be the same
        artifact every time it is taken or the table cannot be re-derived when the theme changes.
      - COMPARABILITY.  The census counts arena entries per record, and a hand-driven run counts
        however long a human happened to leave each window open.  A fixed frame budget per demo
        makes the ranking mean something.
      - The DIFFERENTIAL.  One record set per theme is what separates a record whose lanes come
        from a style var (it moves when the var moves, so it is on a bakeable path) from a record
        built out of literals or content (it is identical in every run).  Match rows across runs by
        the hash column: present in both = nothing in the theme reaches it.

    Demos are opened ONE AT A TIME on purpose.  All 52 at once overruns the window pool and the
    geometry arena, and the census accumulates across the whole session anyway, so a sequential
    walk sees exactly the same distinct set with none of the overflow.

    Not a substitute for the interactive explorer: this walks the DEFAULT state of every demo, so
    anything reachable only by driving a widget (an open combo popup, a dragged slider) is absent
    from the dump.  Those are per-instance shapes rather than theme vocabulary, which is why the
    default state is the right sample for a palette table.

==============================================================================================*/
// clang-format off

/* Frames each demo is held open.  Enough for a window to land its open animation and for the
   retained cache to settle into a steady pass -- the tail of a demo's frames re-tessellates
   nothing, which is exactly the steady state the census should be counting. */
#define EX_CENSUS_FRAMES   12u

/* Frames of nothing between demos, so a closing window's geometry retires before the next opens
   and one demo's records cannot be attributed to the next demo's window. */
#define EX_CENSUS_SETTLE    4u

typedef enum
{
    EX_CENSUS_OFF = 0,
    EX_CENSUS_HOLD,       /* a demo is open, burning its frame budget */
    EX_CENSUS_SETTLING,   /* it just closed; let the frame drain      */
    EX_CENSUS_DONE,

} ex_census_phase_t;

static struct
{
    ex_census_phase_t phase;
    i32               demo;        // index into s_demos
    u32               theme;       // index into the built-in theme list
    u32               theme_count;
    u32               frames;      // frames spent in the current phase
    const char*       theme_name;  // the active theme, for the dump tag

} s_census_run;

/*  Arm the sweep.  Returns false when the gui reports no themes, which would make every run
    identical and the differential meaningless. */

static bool
ex_census_begin( void )
{
    u32                n    = 0;
    const gui_theme_t* list = gui()->theme_list( &n );
    if ( !list || n == 0 )
    {
        fprintf( stderr, "[sb_gui_example] -census: no themes registered\n" );
        return false;
    }

    s_census_run.phase       = EX_CENSUS_HOLD;
    s_census_run.demo        = 0;
    s_census_run.theme       = 0;
    s_census_run.theme_count = n;
    s_census_run.frames      = 0;
    s_census_run.theme_name  = list[ 0 ].name;

    /* Pin the rebuild.  With the retained cache live, how often a window re-tessellates depends on
       what changed around it, so a demo's contribution to the histogram would vary run to run --
       and comparing runs is the entire point.  Forced, every demo contributes exactly
       EX_CENSUS_FRAMES passes and the weights are the vocabulary's, not the cache's.  This makes
       the ABSOLUTE entry counts larger than a real session's; read them as relative. */
    gui()->set_force_redraw( true );

    gui()->theme_set( list[ 0 ].name );
    gui()->debug_style_census( NULL, true );      /* drop the boot chrome's own records */
    ex_set_open( &s_demos[ 0 ], true );

    printf( "[census] sweep: %d demos x %u themes, %u frames each\n",
            EX_DEMO_COUNT, n, (u32)EX_CENSUS_FRAMES );
    return true;
}

/*  Advance to the next demo, or -- when the registry is exhausted -- dump this theme's run and
    move to the next theme.  Called with the current demo already closed. */

static void
ex_census_next( void )
{
    if ( ++s_census_run.demo < EX_DEMO_COUNT )
    {
        ex_set_open( &s_demos[ s_census_run.demo ], true );
        return;
    }

    /* Theme finished: dump under its name and clear for the next.  The tag is the join key
       between runs, and the per-record hash is what pairs the rows inside them. */
    gui()->debug_style_census( s_census_run.theme_name, true );

    u32                n    = 0;
    const gui_theme_t* list = gui()->theme_list( &n );
    if ( ++s_census_run.theme >= n )
    {
        s_census_run.phase = EX_CENSUS_DONE;
        gui()->set_force_redraw( false );
        printf( "[census] sweep complete -- %u runs dumped\n", n );
        return;
    }

    s_census_run.theme_name = list[ s_census_run.theme ].name;
    gui()->theme_set( s_census_run.theme_name );
    printf( "[census] theme %u/%u: %s\n", s_census_run.theme + 1u, n, s_census_run.theme_name );

    s_census_run.demo = 0;
    ex_set_open( &s_demos[ 0 ], true );
}

/*  One frame of the sweep, called right after ex_frame() so this frame's demo has already been
    emitted and counted.  Returns false once the last theme is done, which ends the host loop. */

static bool
ex_census_step( void )
{
    if ( s_census_run.phase == EX_CENSUS_DONE )
        return false;

    ++s_census_run.frames;

    if ( s_census_run.phase == EX_CENSUS_HOLD )
    {
        if ( s_census_run.frames < EX_CENSUS_FRAMES )
            return true;

        ex_set_open( &s_demos[ s_census_run.demo ], false );
        gui()->window_set_open( s_demos[ s_census_run.demo ].title, false );
        s_census_run.phase  = EX_CENSUS_SETTLING;
        s_census_run.frames = 0;
        return true;
    }

    if ( s_census_run.frames < EX_CENSUS_SETTLE )
        return true;

    ex_census_next();
    s_census_run.frames = 0;
    if ( s_census_run.phase == EX_CENSUS_DONE )
        return false;

    s_census_run.phase = EX_CENSUS_HOLD;
    return true;
}

// clang-format on
/*============================================================================================*/
