/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_census.c -- scripted style-record census sweep.

    Drives the whole demo registry with no hands on it.  For each DPI scale, for each theme: open
    one demo, hold it a fixed number of frames, close it, move to the next; when the registry is
    exhausted, dump the census under that combination's name, clear, and start the next.  The
    process exits when the last run is out.

    Why a driver rather than clicking through the explorer:

      - REPEATABILITY.  The palette bake is written from these dumps, so a dump has to be the same
        artifact every time it is taken or the table cannot be re-derived when the theme changes.
      - COMPARABILITY.  The census counts arena entries per record, and a hand-driven run counts
        however long a human happened to leave each window open.  A fixed frame budget per demo
        makes the ranking mean something.
      - The DIFFERENTIALS, which are the whole point.  Match records across runs by the hash column
        the census prints:

          across THEMES  a record present in every theme is one no style var reaches -- a literal,
                         a constant, or content.  One that appears in only some themes is on a
                         bakeable path, and which themes it appears in name the var.
          across DPI     a record present at both scales did not scale with em.  GUI_CLASS_SKIN and
                         GUI_CLASS_STROKE are both specified em-scaled, so a metric-bearing record
                         that survives a 2x DPI change unchanged is a raw-pixel literal -- a bug,
                         and one palette entry that should have collapsed into another.

        The two scales are 1.0 and 2.0 rather than something realistic on purpose: an exact factor
        of two leaves no doubt about whether a lane moved.

    Demos are opened ONE AT A TIME.  All 52 at once overruns the window pool and the geometry
    arena, and the census accumulates across the whole session anyway, so a sequential walk sees
    the same distinct set with none of the overflow.

    Not a substitute for the interactive explorer: this walks the DEFAULT state of every demo, so
    anything reachable only by driving a widget (an open combo popup, a dragged slider) is absent.
    Those are per-instance shapes rather than theme vocabulary, which is why the default state is
    the right sample for a palette table.

==============================================================================================*/
// clang-format off

/* Frames each demo is held open.  Enough for a window to land its open animation and for the
   retained cache to settle into a steady pass. */
#define EX_CENSUS_FRAMES   12u

/* Frames of nothing between demos, so a closing window's geometry retires before the next opens
   and one demo's records cannot be attributed to the next demo's window. */
#define EX_CENSUS_SETTLE    4u

/* Frames after a DPI or theme change before the run starts counting.  Generous because a scale
   change re-lands the managed font, which goes out to the runtime baker -- and the census is
   CLEARED at the end of this window, so everything the transition itself emitted is discarded
   rather than attributed to the run. */
#define EX_CENSUS_LAND     90u

/* The DPI scales swept, applied through GUI_DPI_MANUAL. */
static const f32 k_census_dpi[] = { 1.0f, 2.0f };

#define EX_CENSUS_DPI_COUNT  ( (u32)( sizeof( k_census_dpi ) / sizeof( k_census_dpi[ 0 ] ) ) )

typedef enum
{
    EX_CENSUS_OFF = 0,
    EX_CENSUS_LANDING,    /* DPI / theme just changed; let the font and metrics settle */
    EX_CENSUS_HOLD,       /* a demo is open, burning its frame budget                  */
    EX_CENSUS_SETTLING,   /* it just closed; let the frame drain                       */
    EX_CENSUS_DONE,

} ex_census_phase_t;

static struct
{
    ex_census_phase_t phase;
    i32               demo;         // index into s_demos
    u32               theme;        // index into the built-in theme list
    u32               theme_count;
    u32               dpi;          // index into k_census_dpi
    u32               frames;       // frames spent in the current phase
    char              tag[ 48 ];    // "<theme>@<scale>" -- the dump label and the run's join key

} s_census_run;

/*  Apply the current (DPI, theme) pair and enter the landing window.  Both are set together
    because both re-land the style metrics, and settling once for the pair is cheaper and no less
    correct than settling for each. */

static void
ex_census_enter_run( void )
{
    u32                n    = 0;
    const gui_theme_t* list = gui()->theme_list( &n );
    const char*        name = ( list && s_census_run.theme < n ) ? list[ s_census_run.theme ].name
                                                                 : "?";
    f32                scale = k_census_dpi[ s_census_run.dpi ];

    gui()->dpi_set( GUI_DPI_MANUAL, scale );
    gui()->theme_set( name );

    snprintf( s_census_run.tag, sizeof( s_census_run.tag ), "%s@%.2f", name, scale );

    s_census_run.phase  = EX_CENSUS_LANDING;
    s_census_run.frames = 0;

    printf( "[census] run %u/%u: %s\n",
            s_census_run.dpi * s_census_run.theme_count + s_census_run.theme + 1u,
            EX_CENSUS_DPI_COUNT * s_census_run.theme_count, s_census_run.tag );
}

/*  Arm the sweep.  Returns false when the gui reports no themes, which would make every run
    identical and the theme differential meaningless. */

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

    /* Pin the rebuild.  With the retained cache live, how often a window re-tessellates depends on
       what changed around it, so a demo's contribution to the histogram would vary run to run --
       and comparing runs is the entire point.  Forced, every demo contributes exactly
       EX_CENSUS_FRAMES passes and the weights are the vocabulary's, not the cache's.  This makes
       the ABSOLUTE entry counts larger than a real session's; read them as relative. */
    gui()->set_force_redraw( true );

    s_census_run.demo        = 0;
    s_census_run.theme       = 0;
    s_census_run.dpi         = 0;
    s_census_run.theme_count = n;

    printf( "[census] sweep: %d demos x %u themes x %u dpi scales, %u frames each\n",
            EX_DEMO_COUNT, n, EX_CENSUS_DPI_COUNT, (u32)EX_CENSUS_FRAMES );

    ex_census_enter_run();
    return true;
}

/*  The registry is exhausted for this run: dump it, then advance theme-then-DPI.  Sets the phase
    to DONE when both axes are spent. */

static void
ex_census_finish_run( void )
{
    gui()->debug_style_census( s_census_run.tag, true );

    if ( ++s_census_run.theme >= s_census_run.theme_count )
    {
        s_census_run.theme = 0;
        if ( ++s_census_run.dpi >= EX_CENSUS_DPI_COUNT )
        {
            s_census_run.phase = EX_CENSUS_DONE;
            gui()->set_force_redraw( false );
            gui()->dpi_set( GUI_DPI_AUTO, 0.0f );
            printf( "[census] sweep complete -- %u runs dumped\n",
                    EX_CENSUS_DPI_COUNT * s_census_run.theme_count );
            return;
        }
    }

    s_census_run.demo = 0;
    ex_census_enter_run();
}

/*  One frame of the sweep, called after the present so the frame just scored is fully tessellated
    and counted.  Returns false once the last run is out, which ends the host loop. */

static bool
ex_census_step( void )
{
    if ( s_census_run.phase == EX_CENSUS_DONE )
        return false;

    ++s_census_run.frames;

    switch ( s_census_run.phase )
    {
    case EX_CENSUS_LANDING:
        if ( s_census_run.frames < EX_CENSUS_LAND )
            break;

        /* The scale that actually landed, which is NOT the factor the metric lanes carry.
           dpi_scale() is measured against the BOOT font size; the themes are authored at em=12,
           so a style metric ends up scaled by ( boot_font_px / 12 ) * dpi_scale.  With the boot
           font at 16 that is 4/3 at dpi_scale 1.0 -- which is why GUI_VAR_SHADOW of 16 shows up
           in the dump as feather 21.33 rather than 16.  Only the RATIO between two runs is clean,
           and that is all the differential needs. */
        printf( "[census]   %s landed at dpi_scale %.4f\n",
                s_census_run.tag, (double)gui()->dpi_scale() );

        /* Discard everything the transition emitted -- the old scale's chrome, the font re-bake,
           the closing windows -- so the run counts only what it opened itself. */
        gui()->debug_style_census( NULL, true );
        ex_set_open( &s_demos[ 0 ], true );
        s_census_run.phase  = EX_CENSUS_HOLD;
        s_census_run.frames = 0;
        break;

    case EX_CENSUS_HOLD:
        if ( s_census_run.frames < EX_CENSUS_FRAMES )
            break;

        ex_set_open( &s_demos[ s_census_run.demo ], false );
        gui()->window_set_open( s_demos[ s_census_run.demo ].title, false );
        s_census_run.phase  = EX_CENSUS_SETTLING;
        s_census_run.frames = 0;
        break;

    case EX_CENSUS_SETTLING:
        if ( s_census_run.frames < EX_CENSUS_SETTLE )
            break;

        if ( ++s_census_run.demo < EX_DEMO_COUNT )
        {
            ex_set_open( &s_demos[ s_census_run.demo ], true );
            s_census_run.phase  = EX_CENSUS_HOLD;
            s_census_run.frames = 0;
            break;
        }
        ex_census_finish_run();
        break;

    default:
        break;
    }

    return s_census_run.phase != EX_CENSUS_DONE;
}

// clang-format on
/*============================================================================================*/
