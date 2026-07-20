/*==============================================================================================

    runtime_service/console/console_view.c -- Quake-style developer console drop-down.

    A pure VIEW over the core cvar + console systems: ALL console state (scrollback, commands,
    history) lives in CORE.  This file renders core()->con_line_get() and feeds keystrokes into
    core()->con_submit(); run a core host without gui and the same console still works over
    stdout.  Lifted from the sb_gui_console test bed and wrapped as a host-driven service.

      - The grave key (` / ~) drops the console over the scene, Quake style; Escape closes it.
        The `toggleconsole` command flips it too, so it can be bound or scripted.
      - Enter executes; Up/Down recall history; Tab completes command + cvar names (a single hit
        fills in, several grow the input to their common prefix and list with descriptions); the
        mouse wheel and PageUp/PageDown scroll the scrollback; Ctrl+Home/Ctrl+End jump to the
        oldest line / live tail.  All keys route through gui()->set_edit_key_hook, so the console
        sees them before the text edit and unconsumed keys edit the line as normal.
      - Scrollback lines are coloured by severity (core con_line_level): errors red, warnings
        amber, debug/trace dim, interactive echo and info in the theme text colour.  con_log_level
        sets how much ambient engine logging is pulled in; `condump` copies it to the clipboard.
      - Height is con_height: a fraction of the viewport (0.005..0.5), snapped to whole dense
        rows.  The console runs in a GUI_SCALE_DENSE scope so all sizing is rows_h() row counting.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    View state -- view-side only; the text/data all lives in core
==============================================================================================*/

static bool s_open           = false;
static i32  s_view_offset    = 0;        // scrollback lines above the bottom (0 = live tail)
static i32  s_history_pos    = -1;       // -1 = editing a live line, else index into history
static char s_input[ CONSOLE_INPUT_MAX ];
static i32  s_redraw_frames  = 0;        // force-redraw window after a queued submit
static u32  s_mono_font      = 0;        // monospace font id for column-aligned output (0 = unloaded)
static bool s_mono_tried     = false;    // one-shot: the mono font load was attempted
static bool s_focus_pending  = false;    // one-shot: seat keyboard focus on the input next emit
static i32  s_rows           = CONSOLE_ROWS;  // visible scrollback rows this frame (con_height derived)
static u32  s_tail_total     = 0;        // con_line_total() sampled while parked at the live tail
static i32  s_applied_floor  = -1;       // last log floor pushed to core (con_log_level resync guard)

/* Service-owned cvars: registered in console_mod_init (console_api.c), read here.  cvar_t is
   visible via core_api.h, included by console.c ahead of this translation unit. */
static cvar_t* s_cv_height    = NULL;    // con_height    -- drop-down height, fraction of viewport
static cvar_t* s_cv_log_level = NULL;    // con_log_level -- ambient-log severity floor (string enum)

/*==============================================================================================
    Severity presentation
==============================================================================================*/

/* Scrollback lines carry a log_level_t (core con_line_level): interactive echo/results are
   LOG_LEVEL_CONSOLE, ambient log lines their real severity.  Map the severities that warrant
   attention to a colour; everything else (console I/O, INFO) returns 0 = draw in the theme's
   default text colour.  Colours are 0xAABBGGRR to match the gui abgr convention. */

static u32
console_level_color( log_level_t lvl )
{
    switch ( lvl )
    {
        case LOG_LEVEL_FATAL:
        case LOG_LEVEL_ERROR: return 0xFF5050F0u;    // red
        case LOG_LEVEL_WARN:  return 0xFF40C0F0u;    // amber
        case LOG_LEVEL_DEBUG:
        case LOG_LEVEL_TRACE: return 0xFF8C8C8Cu;    // dim grey
        default:              return 0u;             // CONSOLE / INFO -> theme text colour
    }
}

/* con_log_level is a string enum {trace,debug,info,warn,error}; map the selected value to the
   log_level_t floor con_set_log_filter wants.  Unknown -> WARN (core's own default). */

static log_level_t
console_parse_floor( const char* v )
{
    if ( v )
    {
        if ( strcmp( v, "trace" ) == 0 ) return LOG_LEVEL_TRACE;
        if ( strcmp( v, "debug" ) == 0 ) return LOG_LEVEL_DEBUG;
        if ( strcmp( v, "info"  ) == 0 ) return LOG_LEVEL_INFO;
        if ( strcmp( v, "error" ) == 0 ) return LOG_LEVEL_ERROR;
    }
    return LOG_LEVEL_WARN;
}

/*==============================================================================================
    Input helpers
==============================================================================================*/

/* The grave keypress that toggles the console also arrives as a text event, so the freshly
   focused input field types the '`' character the same frame.  Strip it after the widget runs. */

static void
console_strip_backticks( char* s )
{
    char* dst = s;
    for ( char* src = s; *src; ++src )
    {
        if ( *src != '`' && *src != '~' )
            *dst++ = *src;
    }
    *dst = '\0';
}

/* Up/Down recall: copy a history entry (or an empty live line) into the input buffer. */

static void
console_history_recall( i32 dir )
{
    const i32 count = ( i32 )core()->con_history_count();
    if ( count == 0 )
        return;

    if ( dir < 0 )    /* older: from the live line jump to the newest entry, then walk back */
    {
        if ( s_history_pos < 0 )
            s_history_pos = count - 1;
        else if ( s_history_pos > 0 )
            s_history_pos--;    /* already at the oldest entry: stay put */
    }
    else              /* newer: walk forward; past the newest returns to a live empty line */
    {
        if ( s_history_pos < 0 )
            return;             /* already editing the live line */
        if ( s_history_pos >= count - 1 )
        {
            s_history_pos = -1;
            s_input[ 0 ]  = '\0';
            gui()->set_edit_cursor_end();
            return;
        }
        s_history_pos++;
    }

    snprintf( s_input, sizeof( s_input ), "%s", core()->con_history_get( ( u32 )s_history_pos ) );
    gui()->set_edit_cursor_end();    /* buffer replaced under the caret: seat it at the end */
}

/* Best-effort one-line description for a completion match: commands carry one in the registry,
   cvars in their metadata.  NULL when neither has text. */

static const char*
console_lookup_desc( const char* name )
{
    const u32 nc = core()->cmd_count();
    for ( u32 i = 0; i < nc; ++i )
    {
        if ( strcmp( core()->cmd_name( i ), name ) == 0 )
            return core()->cmd_desc( i );
    }

    cvar_t* cv = core()->cvar_find( name );
    return cv ? core()->cvar_get_desc( cv ) : NULL;
}

/* Tab completion over command + cvar names: a single match replaces the input, several list. */

static void
console_complete_input( void )
{
    const char* matches[ 32 ];
    const u32   n = core()->con_complete( s_input, matches, 32 );

    if ( n == 0 )
        return;

    if ( n == 1 )
    {
        snprintf( s_input, sizeof( s_input ), "%s ", matches[ 0 ] );
        gui()->set_edit_cursor_end();    /* completion replaced the buffer: caret to the end */
        return;
    }

    /* Several matches: first grow the input to their longest common prefix, so a shared stem
       fills in without forcing a pick (case-insensitive compare; keep matches[0]'s casing).
       Then list the candidates with their descriptions so the disambiguating char is obvious. */
    u32 lcp = ( u32 )strlen( matches[ 0 ] );
    for ( u32 i = 1; i < n; ++i )
    {
        u32 j = 0;
        while ( j < lcp && matches[ i ][ j ] &&
                tolower( ( unsigned char )matches[ 0 ][ j ] ) == tolower( ( unsigned char )matches[ i ][ j ] ) )
            ++j;
        lcp = j;
    }

    if ( lcp > ( u32 )strlen( s_input ) )
    {
        snprintf( s_input, sizeof( s_input ), "%.*s", ( int )lcp, matches[ 0 ] );
        gui()->set_edit_cursor_end();
    }

    core()->con_printf( "\n" );
    for ( u32 i = 0; i < n; ++i )
    {
        const char* desc = console_lookup_desc( matches[ i ] );
        if ( desc && desc[ 0 ] )
            core()->con_printf( "  %-24s %s\n", matches[ i ], desc );
        else
            core()->con_printf( "  %s\n", matches[ i ] );
    }
}

/* Key passthrough -- the Doom 3 console key map, run by the focused input field BEFORE its
   own editing (gui()->set_edit_key_hook).  Consumed keys never reach the text edit, so
   Ctrl+Home jumps the scrollback while plain Home still moves the caret.  Keys arrive with
   OS auto-repeat, so holding PageUp keeps scrolling. */

static bool
console_key_hook( u32 key, bool ctrl, bool shift, bool repeat, void* user )
{
    UNUSED( shift );
    UNUSED( repeat );
    UNUSED( user );

    switch ( key )
    {
        case APP_KEY_UP:        console_history_recall( -1 );                      return true;
        case APP_KEY_DOWN:      console_history_recall( +1 );                      return true;
        case APP_KEY_TAB:       console_complete_input();                          return true;
        case APP_KEY_PAGE_UP:   s_view_offset += s_rows / 2;                        return true;
        case APP_KEY_PAGE_DOWN: s_view_offset -= s_rows / 2;                        return true;

        case APP_KEY_HOME:      /* Ctrl+Home: oldest line (clamped in console_show) */
            if ( !ctrl ) return false;
            s_view_offset = ( i32 )core()->con_line_count();
            return true;

        case APP_KEY_END:       /* Ctrl+End: back to the live tail */
            if ( !ctrl ) return false;
            s_view_offset = 0;
            return true;

        default:
            return false;
    }
}

/*==============================================================================================
    Drop-down -- one undecorated window pinned across the top of the display
==============================================================================================*/

static void
console_show( f32 display_w, f32 display_h, f32 top_y )
{
    /* The whole console speaks one step of the theme's scale ramp: DENSE, the text-list step.
       The scope makes every metric read inside -- row heights, gaps, the window and child pads,
       and the sz_rows_h counting helper -- resolve to that step, so sizing is row COUNTING, not
       font arithmetic. */
    gui()->scale_push( GUI_SCALE_DENSE );

    /* con_height sets the drop-down as a fraction of the viewport; snap that to a whole number of
       dense rows so every line edge still lands on the grid.  input_reserve is the input line plus
       the window's own top/bottom pad (it must match win_h below); the rest of the target height
       is scrollback.  base = one row + the child's pad; pitch = each row after the first.  Invert
       sz_rows_h through those two so the row count tracks the theme's metrics, not a fixed guess. */
    f32 frac = s_cv_height ? core()->cvar_get_float( s_cv_height ) : 0.4f;
    if ( frac < 0.005f ) frac = 0.005f;
    if ( frac > 0.5f )   frac = 0.5f;

    const f32 input_reserve = gui()->sz_rows_h( 1 ) + gui()->sz_row_gap();
    const f32 base          = gui()->sz_rows_h( 1 );
    const f32 pitch         = gui()->sz_rows_h( 2 ) - base;
    const f32 hist_avail    = frac * display_h - input_reserve;

    i32 rows = 1;
    if ( hist_avail > base && pitch > 0.0f )
        rows += ( i32 )( ( hist_avail - base ) / pitch );
    if ( rows < CONSOLE_ROWS_MIN ) rows = CONSOLE_ROWS_MIN;
    if ( rows > CONSOLE_ROWS_MAX ) rows = CONSOLE_ROWS_MAX;
    s_rows = rows;    /* the key hook reads this for its half-page scroll step */

    /* The child holds `rows` dense rows: rows - 1 text lines plus the trailing separator, each
       exactly one ramp row.  win_h adds the input row + the window's top/bottom pad. */
    const f32 hist_h = gui()->sz_rows_h( ( u32 )rows );
    const f32 win_h  = hist_h + input_reserve;

    /* window_begin instead of region_begin: a normal window's background (translucent per
       theme) and border, just stripped of title bar / resize / move / collapse.  top_y drops
       the console below the viewport chrome (caption band + menu bar) so the title bar stays
       grabbable while the console is down -- at y=0 the full-width console would cover it. */
    gui()->window_set_next_pos( 0.0f, top_y, GUI_COND_ALWAYS );
    gui()->window_set_next_size( display_w, win_h, GUI_COND_ALWAYS );
    /* GUI_WIN_TEXT_SELECT: the scrollback runs are drawn into the window's own segments (a child
       region is layout, not a separate window id), so flagging the top-level window makes every
       line it draws sweep/marquee selectable within the scrollback's scissor -- no multiline edit
       box needed. */
    if ( gui()->window_begin( "##console",
                              GUI_WIN_NODECORATION | GUI_WIN_NOMOVE | GUI_WIN_MODAL | GUI_WIN_TEXT_SELECT ) )
    {
        gui()->stack();

        /* Speak the whole console in a fixed-pitch font when one loaded (console_frame): cvarlist
           and friends lay their output out with printf column padding, which only lines up under a
           monospace face.  Falls through to the theme font until the load lands. */
        if ( s_mono_font )
            gui()->push_font( s_mono_font );

        /* child_begin carves a fixed-height box out of the window's flow: whatever it holds,
           the input line below always lands at the same y, immune to scroll state or content
           inside the child. */
        if ( gui()->child_begin( "##console_scrollback", 0.0f, hist_h, GUI_WIN_NONE ) ) // GUI_WIN_NOSCROLL
        {
            /* Fixed dense rows: every line -- text or separator -- occupies exactly one ramp
               row, so hist_h above is exact and every line edge sits on the grid. */
            gui()->row( gui()->sz_scale_row( GUI_SCALE_DENSE ) );

            /* Mouse wheel scrolls the scrollback -- while the console is open it owns the
               wheel, Quake style.  Wheel up (positive) looks back in history. */
            const f32 wheel = gui()->get_mouse_wheel();
            if ( wheel != 0.0f )
                s_view_offset += ( i32 )wheel * 3;

            /* Scrollback: the last `visible` lines, shifted up by the view offset.  Each line is
               tinted by its severity (console_level_color); 0 means draw in the theme text colour.
               text_colored/text both feed the window's segments, so either stays sweep-selectable. */
            const i32 visible = rows - 1;    /* the last child row is the separator */
            const i32 total   = ( i32 )core()->con_line_count();
            i32 max_offset    = total - visible;
            if ( max_offset < 0 )
                max_offset = 0;
            if ( s_view_offset > max_offset )
                s_view_offset = max_offset;
            if ( s_view_offset < 0 )
                s_view_offset = 0;

            for ( i32 row = 0; row < visible; ++row )
            {
                const i32 idx = total - s_view_offset - ( visible - row );
                if ( idx < 0 )
                {
                    gui()->text( " " );
                    continue;
                }
                const char* line = core()->con_line_get( ( u32 )idx );
                const u32   col  = console_level_color( core()->con_line_level( ( u32 )idx ) );
                if ( col )
                    gui()->text_colored( col, line );
                else
                    gui()->text( line );
            }

            /* Separator row.  con_line_total is a monotonic commit counter (survives ring wrap):
               while parked at the tail it is the baseline; scrolled up, the difference is how many
               lines landed since, surfaced so new output does not go unnoticed off the bottom.
               Guard the subtraction against a con_clear reset (which zeroes the counter). */
            const u32 committed = core()->con_line_total();
            if ( committed < s_tail_total )
                s_tail_total = committed;

            if ( s_view_offset > 0 )
            {
                char      sep[ 96 ];
                const u32 fresh = committed - s_tail_total;
                if ( fresh > 0 )
                    snprintf( sep, sizeof( sep ), "v v v  %u new below  (End for live tail)  v v v", fresh );
                else
                    snprintf( sep, sizeof( sep ), "^ ^ ^  scrolled back  (End for live tail)  ^ ^ ^" );
                gui()->separator_text( sep );
            }
            else
            {
                s_tail_total = committed;    /* parked at the tail: this is the new baseline */
                gui()->separator();
            }
        }
        gui()->child_end();

        /* Input line: stacked directly below the child in the window's own flow.  The child
           above always consumes exactly hist_h regardless of its content, so this never
           shifts.  A two-track row -- a natural-width "]" prompt gutter, then a flex field --
           pins the prompt at the left so it reads like the "] cmd" echo in the scrollback. */
        static const f32 input_tracks[] = { 0.0f, 1.0f, GUI_END };
        gui()->row_cols( gui()->sz_scale_row( GUI_SCALE_DENSE ), input_tracks );
        gui()->text( "]" );

        /* Focus is a one-shot EVENT, armed on open (console_set_open): request it here, right
           before the input emits, so the very next focusable widget -- this input -- consumes it
           the same frame.  The window is GUI_WIN_MODAL, so the gui core confines keyboard focus
           to it (focus_allowed in interact/gui_item.c): no other window can steal focus while the
           console is down, so there is no need to re-steal every frame.  Leaving focus alone the
           rest of the time is what lets a scrollback text selection drop the caret and Ctrl+C
           copy the selection (a focused field would otherwise own the copy). */
        if ( s_focus_pending )
        {
            s_focus_pending = false;
            gui()->set_keyboard_focus();
        }

        /* One-shot: the hook must be re-armed each frame, just before the field it is meant for. */
        gui()->set_edit_key_hook( console_key_hook, NULL );

        const bool entered = gui()->input_text( "##con_input", s_input, sizeof( s_input ) );

        console_strip_backticks( s_input );

        if ( entered )
        {
            core()->con_submit( s_input );    /* queued: runs at the loop's next cmd_pump */
            s_redraw_frames = 2;              /* output lands next frame; keep the emit alive */
            s_input[ 0 ]  = '\0';
            s_history_pos = -1;
            s_view_offset = 0;         /* executing snaps the view back to the live tail */
        }
        if ( s_mono_font )
            gui()->pop_font();
    }
    gui()->window_end();
    gui()->scale_pop();   /* close the DENSE scope opened above the sizing math */
}

/*==============================================================================================
    Service entry points -- driven by the host loop
==============================================================================================*/

static void
console_set_open( bool open )
{
    if ( s_open == open )
        return;

    s_open = open;
    if ( s_open )
    {
        s_view_offset   = 0;       /* open on the live tail */
        s_focus_pending = true;    /* one-shot: seat focus on the input as the console appears */
    }
}

static void
console_toggle( void )
{
    console_set_open( !s_open );
}

static bool
console_is_open( void )
{
    return s_open;
}

/* Per-frame housekeeping -- the host calls this once per frame, near input()->frame().
   Polls the toggle and ticks the post-submit redraw pin.  Emits no widgets. */

static void
console_frame( f32 dt )
{
    UNUSED( dt );

    /* Keep core's ambient-log floor in sync with con_log_level (runs even while closed so the
       setting takes hold immediately; applied only on change, so it is near-free otherwise). */
    if ( s_cv_log_level )
    {
        const log_level_t floor = console_parse_floor( core()->cvar_get_string( s_cv_log_level ) );
        if ( ( i32 )floor != s_applied_floor )
        {
            core()->con_set_log_filter( floor );
            s_applied_floor = ( i32 )floor;
        }
    }

    /* Quake toggle: grave opens/closes, Escape closes. */
    if ( app()->key_pressed( APP_KEY_GRAVE ) )
        console_toggle();
    if ( s_open && app()->key_pressed( APP_KEY_ESCAPE ) )
        console_set_open( false );

    /* One-shot monospace load, deferred to the first open: the host boots a proportional theme
       font, but the console's printf-padded output (cvarlist, cvarinfo) only aligns in a
       fixed-pitch face.  Loaded here -- console_frame runs before gui()->frame_begin, the safe
       between-frames point font_load requires -- and pushed around the drop-down in console_show.
       font_load activates the new font, so save/restore keeps the theme font active for the host. */
    if ( s_open && !s_mono_tried )
    {
        s_mono_tried = true;
        char path[ 512 ];
        gui()->asset_path( "assets/font/JetBrainsMonoNL-Regular_16px.orb_font", path, sizeof( path ) );
        const u32 prev = gui()->font_active_id();
        s_mono_font = gui()->font_load( path );   /* 0 on failure: console falls back to the theme font */
        gui()->font_use( prev );
    }

    /* Queued commands print the frame AFTER Enter; hold the emit open briefly so the
       retained-cache clean-frame skip doesn't keep the new scrollback lines offscreen. */
    if ( s_redraw_frames > 0 )
    {
        gui()->set_force_redraw( true );
        if ( --s_redraw_frames == 0 )
            s_redraw_frames = -1;    /* submit settled: clear the pin next frame */
    }
    else if ( s_redraw_frames < 0 )
    {
        gui()->set_force_redraw( false );
        s_redraw_frames = 0;
    }
}

/* Emit the drop-down over viewport vp.  Called inside the gui frame build (after on_gui,
   before ctx_end); a no-op while the console is closed. */

static void
console_emit( f32 dt, gui_vp_t vp )
{
    UNUSED( dt );

    if ( !s_open )
        return;

    i32 disp_w = 0, disp_h = 0;
    gui()->viewport_size( vp, &disp_w, &disp_h );

    /* Start below the viewport chrome (caption + main menu bar) so the title bar stays
       draggable.  0 on an OS-chrome window; the host's menu bar, if any, emitted before this. */
    const f32 top_y = gui()->viewport_content_y( vp );

    console_show( ( f32 )disp_w, ( f32 )disp_h, top_y );
}

/*==============================================================================================
    Console commands -- registered into the core backend in console_mod_init (console_api.c)
==============================================================================================*/

/* toggleconsole -- flip the drop-down from a command, so the open gesture can be bound/scripted
   rather than living only in the hardcoded grave poll. */

static void
console_cmd_toggle( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    console_toggle();
}

/* condump -- copy the whole scrollback to the OS clipboard, one line per row.  Assembles into a
   single core-allocated buffer (the sweep-select Ctrl+C only reaches on-screen lines). */

static void
console_cmd_dump( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    const u32 count = core()->con_line_count();

    size_t cap = 1;    /* trailing NUL */
    for ( u32 i = 0; i < count; ++i )
        cap += strlen( core()->con_line_get( i ) ) + 1;    /* + newline */

    char* buf = ( char* )core()->alloc( cap );
    if ( !buf )
    {
        core()->con_printf( "condump: out of memory\n" );
        return;
    }

    size_t off = 0;
    for ( u32 i = 0; i < count; ++i )
    {
        const char*  line = core()->con_line_get( i );
        const size_t len  = strlen( line );
        memcpy( buf + off, line, len );
        off += len;
        buf[ off++ ] = '\n';
    }
    buf[ off ] = '\0';

    app()->clipboard_set( buf );
    core()->free( buf );
    core()->con_printf( "condump: copied %u lines to the clipboard\n", count );
}

// clang-format on
/*============================================================================================*/
