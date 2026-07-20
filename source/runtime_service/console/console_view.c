/*==============================================================================================

    runtime_service/console/console_view.c -- Quake-style developer console drop-down.

    A pure VIEW over the core cvar + console systems: ALL console state (scrollback, commands,
    history) lives in CORE.  This file renders core()->con_line_get() and feeds keystrokes into
    core()->con_submit(); run a core host without gui and the same console still works over
    stdout.  Lifted from the sb_gui_console test bed and wrapped as a host-driven service.

      - The grave key (` / ~) drops the console over the scene, Quake style; Escape closes it.
      - Enter executes; Up/Down recall history; Tab completes command + cvar names; the mouse
        wheel and PageUp/PageDown scroll the scrollback; Ctrl+Home/Ctrl+End jump to the oldest
        line / live tail.  All keys route through gui()->set_edit_key_hook, so the console sees
        them before the text edit and unconsumed keys edit the line as normal.
      - Layout is authored in the gui grid system: the console runs in a GUI_SCALE_DENSE scope
        with a fixed one-ramp-row template, so all sizing is rows_h() row counting.

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

    if ( s_history_pos < 0 )
        s_history_pos = ( dir < 0 ) ? count - 1 : -1;
    else
        s_history_pos += dir;

    if ( s_history_pos >= count )
        s_history_pos = -1;    /* walked past the newest entry: back to a live empty line */

    if ( s_history_pos < 0 )
    {
        s_history_pos = ( dir < 0 ) ? 0 : -1;
        if ( s_history_pos < 0 )
        {
            s_input[ 0 ] = '\0';
            return;
        }
    }

    snprintf( s_input, sizeof( s_input ), "%s", core()->con_history_get( ( u32 )s_history_pos ) );
    gui()->set_edit_cursor_end();    /* buffer replaced under the caret: seat it at the end */
}

/* Tab completion over command + cvar names: a single match replaces the input, several list. */

static void
console_complete_input( void )
{
    const char* matches[ 32 ];
    const u32   n = core()->con_complete( s_input, matches, 32 );

    if ( n == 1 )
    {
        snprintf( s_input, sizeof( s_input ), "%s ", matches[ 0 ] );
        gui()->set_edit_cursor_end();    /* completion replaced the buffer: caret to the end */
    }
    else if ( n > 1 )
    {
        core()->con_printf( "\n" );
        for ( u32 i = 0; i < n; ++i )
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
        case APP_KEY_PAGE_UP:   s_view_offset += CONSOLE_ROWS / 2;                 return true;
        case APP_KEY_PAGE_DOWN: s_view_offset -= CONSOLE_ROWS / 2;                 return true;

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
console_show( f32 display_w, f32 top_y )
{
    /* The whole console speaks one step of the theme's scale ramp: DENSE, the text-list step.
       The scope makes every metric read inside -- row heights, gaps, the window and child pads,
       and the sz_rows_h counting helper -- resolve to that step, so sizing is row COUNTING, not
       font arithmetic.

       hist_h is the scrollback child: CONSOLE_ROWS text rows plus the trailing separator row,
       each exactly one dense row (the fixed row template below), with sz_rows_h carrying the gaps
       between and the child's own top/bottom pad.  win_h adds the input row -- sz_rows_h( 1 )
       carries the gap above it and the window's bottom pad -- plus the window's top pad. */
    gui()->scale_push( GUI_SCALE_DENSE );

    const f32 hist_h = gui()->sz_rows_h( CONSOLE_ROWS + 1 );                   /* text rows + separator */
    const f32 win_h  = hist_h + gui()->sz_rows_h( 1 ) + gui()->sz_row_gap();   /* + input row + top pad */

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
        if ( gui()->child_begin( "##console_scrollback", 0.0f, hist_h, GUI_WIN_NOSCROLL ) )
        {
            /* Fixed dense rows: every line -- text or separator -- occupies exactly one ramp
               row, so hist_h above is exact and every line edge sits on the grid. */
            gui()->row( gui()->sz_scale_row( GUI_SCALE_DENSE ) );

            /* Mouse wheel scrolls the scrollback -- while the console is open it owns the
               wheel, Quake style.  Wheel up (positive) looks back in history. */
            const f32 wheel = gui()->get_mouse_wheel();
            if ( wheel != 0.0f )
                s_view_offset += ( i32 )wheel * 3;

            /* Scrollback: the last CONSOLE_ROWS lines, shifted up by the view offset. */
            const i32 total = ( i32 )core()->con_line_count();
            i32 max_offset  = total - CONSOLE_ROWS;
            if ( max_offset < 0 )
                max_offset = 0;
            if ( s_view_offset > max_offset )
                s_view_offset = max_offset;
            if ( s_view_offset < 0 )
                s_view_offset = 0;

            for ( i32 row = 0; row < CONSOLE_ROWS; ++row )
            {
                const i32 idx = total - s_view_offset - ( CONSOLE_ROWS - row );
                gui()->text( ( idx >= 0 ) ? core()->con_line_get( ( u32 )idx ) : " " );
            }

            /* The fixed row template hands both separator kinds the same one-row cell, so
               switching between them keeps this row's height constant across scroll states. */
            if ( s_view_offset > 0 )
                gui()->separator_text( "^ ^ ^  (PageDown for live tail)  ^ ^ ^" );
            else
                gui()->separator();
        }
        gui()->child_end();

        /* Input line: stacked directly below the child in the window's own flow.  The child
           above always consumes exactly hist_h regardless of its content, so this never
           shifts. */

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

    console_show( ( f32 )disp_w, top_y );
}

// clang-format on
/*============================================================================================*/
