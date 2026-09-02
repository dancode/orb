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
      - Height is con_height: a fraction of the space below the viewport chrome (0.1..1.0, 1.0 =
        full).  The body is a 2-row grid (flex scrollback cell + fixed input row); the scrollback
        is a GUI_WIN_ANCHOR_BOTTOM child of real text widgets (one per line, oldest first) --
        the region bottom-justifies the block so the newest line hugs the input and any leftover
        space falls at the top, and pins the scroll to the live tail, so the view never does its
        own row math.  rows_clip virtualizes the run; scroll_by carries the key-driven scrolling.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    View state -- view-side only; the text/data all lives in core
==============================================================================================*/

static bool s_open           = false;
static i32  s_history_pos    = -1;       // -1 = editing a live line, else index into history
static char s_input[ CONSOLE_INPUT_MAX ];
static i32  s_redraw_frames  = 0;        // force-redraw window after a queued submit
static u32  s_mono_font      = 0;        // monospace font id for column-aligned output (0 = unloaded)
static bool s_mono_tried     = false;    // one-shot: the mono font load was attempted
static bool s_focus_pending  = false;    // one-shot: seat keyboard focus on the input next emit
static i32  s_applied_floor  = -1;       // last log floor pushed to core (con_log_level resync guard)

/* Scrollback scrolling is the gui region's job now (GUI_WIN_ANCHOR_BOTTOM: the block bottom-justifies
   and the scroll pins to the live tail on its own).  The key map only needs to hand the region a
   pending nudge, applied inside the scrollback child via gui()->scroll_by; s_page_px is half the
   scrollback view height, sampled there each frame so PageUp/Down step by a screenful. */
static f32  s_scroll_dy      = 0.0f;     // pending scroll nudge (px, +down); consumed next scrollback emit
static f32  s_page_px        = 0.0f;     // PageUp/Down step -- half the scrollback view height

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
        case APP_KEY_PAGE_UP:   s_scroll_dy -= s_page_px;                           return true;  /* look back (up) */
        case APP_KEY_PAGE_DOWN: s_scroll_dy += s_page_px;                           return true;

        case APP_KEY_HOME:      /* Ctrl+Home: jump to the oldest line (top) */
            if ( !ctrl ) return false;
            s_scroll_dy = -1.0e6f;   /* clamped to 0 at the region's next push */
            return true;

        case APP_KEY_END:       /* Ctrl+End: back to the live tail (bottom) */
            if ( !ctrl ) return false;
            s_scroll_dy = 1.0e6f;    /* clamped to the tail; re-arms tail-follow */
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
       Every metric read inside -- row heights, gaps, region pads -- resolves to that step. */
    gui()->scale_push( GUI_SCALE_DENSE );

    /* Window height is just a fraction of the space below the caption band (top_y = the title-bar
       band the console drops under; it covers the menu bar).  No row snapping: the layout below
       fills whatever this is, and reads back the resolved geometry to decide how many text rows
       fit -- never the other way round.  The cvar's own min/max bounds the value; the clamp only
       guards a stray 0. */
    f32 frac = s_cv_height ? core()->cvar_get_float( s_cv_height ) : 0.4f;
    if ( frac <= 0.0f ) frac = 0.1f;
    if ( frac > 1.0f )  frac = 1.0f;

    const f32 win_h = frac * ( display_h - top_y );

    gui()->window_set_next_pos( 0.0f, top_y, GUI_COND_ALWAYS );
    gui()->window_set_next_size( display_w, win_h, GUI_COND_ALWAYS );
    /* The scrollback is a real flow of text widgets over a bottom-anchored region (below); the
       runs are drawn by widgets but GUI_WIN_TEXT_SELECT stays off, so `condump` copies the whole
       buffer to the clipboard when a copy is needed. */
    if ( gui()->window_begin( "##console",
                              GUI_WIN_NODECORATION | GUI_WIN_NOMOVE | GUI_WIN_MODAL |
                              GUI_WIN_NOMOUSESCROLL | GUI_WIN_TEXT_SELECT ) )
    {
        /* Fixed-pitch font when one loaded (console_frame): cvarlist and friends align their output
           with printf column padding, which only lines up under a monospace face. */
        if ( s_mono_font )
            gui()->push_font( s_mono_font );

        const f32 line_h = gui()->sz_scale_row( GUI_SCALE_DENSE );   /* one dense row */

        /* Split the window body into two vertical tracks: a FLEX scrollback that fills the top and
           a FIXED one-line input pinned at the bottom.  The grid resolves both across the body
           interior (pen to floor), gaps and pad included -- so the scrollback cell's height is the
           layout's answer, not ours, and the input can never be squeezed off the bottom. */
        gui()->grid( ( gui_grid_t ){
            .cols = { 1.0f, GUI_END },
            .rows = { 1.0f, line_h, GUI_END },
        } );

        /* Scrollback cell.  A GUI_WIN_ANCHOR_BOTTOM child: the region bottom-justifies the block so
           the newest line hugs the input and any slack falls at the top, and pins its scroll to the
           live tail as new output lands (until the user scrolls up).  NOSCROLL keeps it bar-less,
           Quake style; NOMOUSESCROLL disables the region's own (pop-time, one-frame-lagged) wheel so
           the console can drive ALL scrolling through scroll_by, which applies same-frame -- wheel
           and keys alike land with no lag.  Rows emit in NATURAL order (oldest first) as real text
           widgets; rows_clip virtualizes the run so a deep scrollback costs only its visible slice. */
        if ( gui()->child_begin( "##console_scrollback", 0.0f, 0.0f,
            GUI_WIN_NOSCROLL | GUI_WIN_NOMOUSESCROLL | GUI_WIN_ANCHOR_BOTTOM ) )
        {
            /* Fold the mouse wheel into the pending key-driven scroll (PageUp/Down a screenful,
               Ctrl+Home/End an edge), then apply it all in one same-frame nudge -- no lag.  Wheel up
               looks back (toward the top), 3 lines a notch, matching the old console feel. */
            const f32 wheel = gui()->get_mouse_wheel();
            if ( wheel != 0.0f )
                s_scroll_dy -= wheel * line_h * 3.0f;
            if ( s_scroll_dy != 0.0f )
            {
                gui()->scroll_by( 0.0f, s_scroll_dy );
                s_scroll_dy = 0.0f;
            }
            s_page_px = gui()->view_avail().y * 0.5f;

            const u32 text_col = gui()->style_peek()->col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_IDLE ];
            const i32 total    = ( i32 )core()->con_line_count();

            /* One fixed-height column; rows_clip reserves the whole run's extent (so the scroll range
               stays honest) and returns only the visible [first,last) slice to emit. */
            gui()->row_cols_n( line_h, 1 );
            const gui_span_t vis = gui()->rows_clip( total, line_h );
            for ( i32 idx = vis.first; idx < vis.last; ++idx )
            {
                const char* line = core()->con_line_get( ( u32 )idx );
                const u32   sev  = console_level_color( core()->con_line_level( ( u32 )idx ) );
                gui()->text_colored( sev ? sev : text_col, line );
            }
        }
        gui()->child_end();

        /* Input line: fills the fixed bottom grid cell.  No prompt gutter -- the field's text lands
           at the window content left, the same x as the "] " the core echoes at the start of every
           scrollback line, so the live input reads as a continuation of the log. */

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
            s_scroll_dy   = 1.0e6f;    /* executing snaps the view back to the live tail */
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
        s_scroll_dy     = 1.0e6f;  /* open on the live tail (re-arms follow if last closed scrolled up) */
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
        const u32 prev = gui()->font_active_id();
        s_mono_font = gui()->font_load( RID( "font/jetbrains/16" ) );   /* 0 on failure: console falls back to the theme font */
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
console_emit( f32 dt, i32 vp )
{
    UNUSED( dt );

    if ( !s_open )
        return;

    i32 disp_w = 0, disp_h = 0;
    gui()->viewport_size( vp, &disp_w, &disp_h );

    /* Drop below the caption band ONLY -- the console covers the main menu bar rather than
       sitting under it (it is GUI_WIN_MODAL, so a visible-but-dead menu bar reads wrong; hiding
       it under the drop-down is the Quake feel).  caption_h excludes the menu bar band, so only
       the title bar stays exposed and draggable.  0 on an OS-chrome window. */
    const f32 top_y = gui()->viewport_caption_h( vp );

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
