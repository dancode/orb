/*==============================================================================================

    runtime_service/gui/debug/gui_step_window.c -- Command stepper: control window shell.

    The interactive front end of the command stepper (backend/gui_step_capture.c): Capture /
    Release, a transport row ( |<  <  >  >| ), and a scrub slider over the frozen command
    prefix.  An ORDINARY window drawn through the normal pipeline with the standard widgets --
    what keeps it usable over a frozen scene is GUI_WIN_DEBUG_BAND: the capture never snapshots
    the debug band and live debug-band emission is never suppressed, so the controls stay fully
    interactive while the band-0 replay under them holds still.

    Every control LATCHES its effect (capture at the next build, release / cursor at the next
    frame's restore -- see gui_backend.h), so each mutating branch raises wants_redraw or the
    clean-frame emit skip would sit on the stale request (the deferred-update rule).

    Emitted internally (debug_overlays_emit, gui_frame_overlay.c) at the default context's
    ctx_end while debug_enable is on.  The F8 hotkey freezes/releases and opens this window; the
    X button only hides it -- hiding does NOT release an active freeze (F8 or Release does).
    The , . step hotkeys keep working alongside (they scrub with key repeat; the buttons do not).
    Compiled out unless GUI_CMD_STEPPER (gui_backend.h); gui_step_window stays a no-op stub then.

==============================================================================================*/
// clang-format off

#ifdef GUI_CMD_STEPPER

#define STEP_SHELL_TITLE "Command Stepper"

/* Seek + the wants_redraw the latched cursor needs to reach the next frame's restore. */
static void
step_seek_dirty( u32 cursor )
{
    gui_step_seek( cursor );
    g_ctx->retained.wants_redraw = true;
}

void
gui_step_window( bool* open )
{
    if ( !( open && *open ) )
        return;

    /* The host said open: reopen the pool entry if the X button hid it on an earlier run. */
    gui_window_set_open( STEP_SHELL_TITLE, true );

    gui_window_set_next_size( 340.0f, 130.0f, GUI_COND_ONCE );
    if ( gui_window_begin( STEP_SHELL_TITLE, GUI_WIN_CLOSEABLE | GUI_WIN_DEBUG_BAND ) )
    {
        gui_stack();

        /* State row: the freeze toggle plus the cursor readout. */
        bool frozen = gui_step_frozen();
        if ( gui_button( frozen ? "Release" : "Capture" ) )
        {
            if ( frozen ) gui_step_release();
            else          gui_step_capture();
            g_ctx->retained.wants_redraw = true;
        }
        gui_same_line( -1.0f );
        if ( frozen )
            gui_textf( "frozen   %u / %u", gui_step_cursor(), gui_step_count() );
        else
            gui_text( "live -- Capture freezes this frame's command list" );

        if ( frozen )
        {
            u32 cur = gui_step_cursor();
            u32 cnt = gui_step_count();

            /* Transport: seek to start / one back / one forward / seek to end.  Single steps --
               the , . hotkeys cover held scrubbing (key repeat), the slider covers long jumps. */
            if ( gui_button( "|<" ) )
                step_seek_dirty( 0 );
            gui_same_line( -1.0f );
            if ( gui_button( "<" ) && cur > 0 )
                step_seek_dirty( cur - 1 );
            gui_same_line( -1.0f );
            if ( gui_button( ">" ) )
                step_seek_dirty( cur + 1 );   /* seek clamps to the frozen count */
            gui_same_line( -1.0f );
            if ( gui_button( ">|" ) )
                step_seek_dirty( cnt );

            /* Scrubber over the whole frozen prefix. */
            i32 v = (i32)cur;
            if ( gui_slider_int( "##step_cursor", &v, 0, (i32)cnt ) )
                step_seek_dirty( v < 0 ? 0u : (u32)v );
        }
    }
    gui_window_end();

    /* The X button closed it this frame: report back so the hotkey toggle stays in sync. */
    if ( !gui_window_is_open( STEP_SHELL_TITLE ) )
        *open = false;
}

#else  /* !GUI_CMD_STEPPER */

/* No-op stub so the emit site (debug_overlays_emit) needs no build guard of its own. */
void
gui_step_window( bool* open )
{
    (void)open;
}

#endif /* GUI_CMD_STEPPER */

// clang-format on
/*============================================================================================*/
