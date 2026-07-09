/*==============================================================================================

    sandbox/gui_console/sb_gui_console.c -- Quake-style developer console test bed.

    Kicks the tires on the core cvar + console systems through the gui front end:

      - ALL console state (scrollback, commands, history) lives in CORE.  This file is a pure
        VIEW: it renders core()->con_line_get() and feeds keystrokes into core()->con_exec().
        Run any core host without a gui and the same console still works over stdout.
      - The grave key (` / ~) drops the console over the scene, Quake style; Escape closes it.
      - Enter executes; Up/Down recall history; Tab completes command + cvar names;
        the mouse wheel and PageUp/PageDown scroll the scrollback; Ctrl+Home/Ctrl+End jump to
        the oldest line / live tail.  All keys route through gui()->set_edit_key_hook, so the
        console sees them before the text edit and unconsumed keys edit the line as normal.
      - A test-bed window shows live cvar values read through core()-> every frame, so a
        "s_volume 0.25" typed in the console moves the bar the same frame.
      - Layout is authored in the gui grid system: the console runs in a GUI_SCALE_DENSE scope
        with a fixed one-ramp-row template, so all sizing is rows_h() row counting; the test-bed
        window's geometry is in grid units (u( n )).  The original hand-derived font arithmetic
        this replaced is what motivated the grid/scale system.

    Try:  help, cvarlist, s_volume 0.25, toggle cheats, r_quality ultra, cvarinfo r_quality,
          seta com_maxfps 120, writeconfig, reset_all, quit

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

/*==============================================================================================
    Console front-end state -- view-side only; the text/data all lives in core
==============================================================================================*/

#define CONSOLE_ROWS      18      // visible scrollback rows in the drop-down
#define CONSOLE_INPUT_MAX 120     // input line capacity (fits CON_HISTORY_LEN)

static bool s_console_open   = false;
static bool s_focus_pending  = false;    // focus the input line on the next emitted frame
static i32  s_view_offset    = 0;        // scrollback lines above the bottom (0 = live tail)
static i32  s_history_pos    = -1;       // -1 = editing a live line, else index into history
static char s_input[ CONSOLE_INPUT_MAX ];

static bool s_quit           = false;    // set by the host-registered "quit" command
static i32  s_redraw_frames  = 0;        // force-redraw window after a queued submit

/*============================================================================================*/
/* Host command -- proves front ends can extend the core registry. */

static void
cmd_quit( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    s_quit = true;
}

/*============================================================================================*/
/* Demo cvars -- one of each type, registered through the core()-> vtable. */

static void
register_demo_cvars( void )
{
    static const char* quality_options[] = { "low", "medium", "high", "ultra" };

    core()->cvar_register_b( "cheats",      "Enable cheat commands", false, 0 );
    core()->cvar_register_i( "com_maxfps",  "Maximum frames per second", 240, 30, 480, CVAR_ARCHIVE );
    core()->cvar_register_f( "s_volume",    "Sound volume", 0.8f, 0.0f, 1.0f, CVAR_ARCHIVE );
    core()->cvar_register_s( "r_quality",   "Rendering quality", quality_options, 4, 2, CVAR_ARCHIVE );
    core()->cvar_register_w( "player_name", "Player display name", "orb", 32, CVAR_ARCHIVE );

    core()->cmd_register( "quit", cmd_quit, "Exit the application" );
}

/*==============================================================================================
    Console input helpers
==============================================================================================*/

/* The grave keypress that toggles the console also arrives as a text event, so the freshly
   focused input field types the '`' character the same frame.  Strip it after the widget runs. */

static void
strip_backticks( char* s )
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
history_recall( i32 dir )
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
complete_input( void )
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
        case APP_KEY_UP:        history_recall( -1 );                              return true;
        case APP_KEY_DOWN:      history_recall( +1 );                              return true;
        case APP_KEY_TAB:       complete_input();                                  return true;
        case APP_KEY_PAGE_UP:   s_view_offset += CONSOLE_ROWS / 2;                 return true;
        case APP_KEY_PAGE_DOWN: s_view_offset -= CONSOLE_ROWS / 2;                 return true;

        case APP_KEY_HOME:      /* Ctrl+Home: oldest line (clamped in show_console) */
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
    Console drop-down -- one undecorated window pinned across the top of the display
==============================================================================================*/

static void
show_console( f32 display_w )
{
    /* The whole console speaks one step of the theme's scale ramp: DENSE, the text-list step.
       The scope makes every metric read inside -- row heights, gaps, the window and child pads,
       and the rows_h counting helper -- resolve to that step, so sizing is row COUNTING, not
       font arithmetic (the old text_h/calc_row/gap derivation lived here; the grid quantizes
       auto text rows, so counting fixed ramp rows is now both simpler and the only exact form).

       hist_h is the scrollback child: CONSOLE_ROWS text rows plus the trailing separator row,
       each exactly one dense row (the fixed row template below), with rows_h carrying the gaps
       between and the child's own top/bottom pad.  win_h adds the input row -- rows_h( 1 )
       carries the gap above it and the window's bottom pad -- plus the window's top pad. */
    gui()->scale_push( GUI_SCALE_DENSE );

    const f32 hist_h = gui()->rows_h( CONSOLE_ROWS + 1 );                /* text rows + separator */
    const f32 win_h  = hist_h + gui()->rows_h( 1 ) + gui()->row_gap();   /* + input row + top pad */

    /* window_begin instead of region_begin: a normal window's background (translucent per
       theme) and border, just stripped of title bar / resize / move / collapse -- looks like
       any other window rather than a chrome-less HUD overlay. */
    gui()->window_set_next_pos( 0.0f, 0.0f, GUI_COND_ALWAYS );
    gui()->window_set_next_size( display_w, win_h, GUI_COND_ALWAYS );
    if ( gui()->window_begin( "##console", GUI_WIN_NODECORATION | GUI_WIN_NOMOVE ) )
    {
        gui()->stack();

        /* child_begin carves a fixed-height box out of the window's flow: whatever it holds,
           the input line below always lands at the same y, immune to scroll state or content
           inside the child -- the same guarantee the two-region split gave, without a second
           window. */
        if ( gui()->child_begin( "##console_scrollback", 0.0f, hist_h, GUI_WIN_NOSCROLL ) )
        {
            /* Fixed dense rows: every line -- text or separator -- occupies exactly one ramp
               row, so hist_h above is exact and every line edge sits on the grid.  (A plain
               stack() would auto-size text rows to the font instead; fixed rows are what make
               the count-based sizing a guarantee rather than a coincidence.) */
            gui()->row( gui()->scale_row( GUI_SCALE_DENSE ) );

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

        /* set_keyboard_focus queues focus for the next focusable widget: on open it was
           requested last frame, after Enter it is requested below for the coming frame. */
        if ( s_focus_pending )
        {
            gui()->set_keyboard_focus();
            s_focus_pending = false;
        }

        /* One-shot: the hook must be re-armed each frame, just before the field it is meant for. */
        gui()->set_edit_key_hook( console_key_hook, NULL );

        const bool entered = gui()->input_text( "##con_input", s_input, sizeof( s_input ) );

        strip_backticks( s_input );

        if ( entered )
        {
            core()->con_submit( s_input );    /* queued: runs at the loop's next cmd_pump */
            s_redraw_frames = 2;              /* output lands next frame; keep the emit alive */
            s_input[ 0 ]  = '\0';
            s_history_pos = -1;
            s_view_offset = 0;         /* executing snaps the view back to the live tail */
            s_focus_pending = true;    /* Enter dropped focus; take it back next frame */
        }
    }
    gui()->window_end();
    gui()->scale_pop();   /* close the DENSE scope opened above the sizing math */
}

/*==============================================================================================
    Test-bed window -- live cvar readout, all through core()->
==============================================================================================*/

static void
show_test_bed( void )
{
    /* Authored in grid units (u( n ) = n quanta), not raw px: the same geometry as before at
       q=4, but it stays on the lattice if the theme's quantum retunes. */
    gui()->window_set_next_pos ( gui()->u( 15 ),  gui()->u( 120 ), GUI_COND_ONCE );
    gui()->window_set_next_size( gui()->u( 115 ), gui()->u( 55 ),  GUI_COND_ONCE );
    if ( gui()->window_begin( "Console Test Bed", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( "Press ` (grave) to toggle the developer console.  Values below are "
                             "read through core()-> every frame -- set them from the console and "
                             "watch them move." );
        gui()->separator();

        gui()->textf( "version      %s", core()->cvar_get_value( "version" ) );
        gui()->textf( "r_quality    %s", core()->cvar_get_value( "r_quality" ) );
        gui()->textf( "com_maxfps   %s", core()->cvar_get_value( "com_maxfps" ) );
        gui()->textf( "player_name  %s", core()->cvar_get_value( "player_name" ) );
        gui()->textf( "cheats       %s   log_level %s",
                      core()->cvar_get_value( "cheats" ),
                      core()->cvar_get_value( "log_level" ) );

        cvar_t* vol = core()->cvar_find( "s_volume" );
        gui()->progress_bar( vol ? core()->cvar_get_float( vol ) : 0.0f, "s_volume" );
    }
    gui()->window_end();
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_console] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    register_demo_cvars();

    core()->con_printf( "ORB developer console -- type \"help\" for commands.\n" );

    int      ret_code = 1;

    /* keyboard_nav off: the console owns Tab (completion) and Up/Down (history). */
    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "sb_gui_console",
        .w         = 1280, .h = 720,
        .os_chrome = true,
        .font      = GUI_FONT_ROBOTO_16, // GUI_FONT_JETBRAINS_16,
        .caps      = &( gui_forward_caps_t ){ .keyboard_nav = true, .tables = false, .docking = false },
        .clock     = sys_tick_seconds,
        .sleep     = sys_sleep_milliseconds,
        .wait      = sys_wait_for_os_events_ms,
        .clear     = { 0.10f, 0.12f, 0.15f, 1.00f },
        .debug     = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_console] gui->boot failed\n" );
        goto shutdown;
    }

    f32 dt = 0.0f;
    while ( gui()->frame_poll( &dt ) && !s_quit )
    {
        /* Drain queued command text (console submits, exec files). */
        core()->cmd_pump();

        /* Queued commands print the frame AFTER Enter; hold the emit open briefly so the
           retained-cache clean-frame skip doesn't keep the new scrollback lines offscreen. */
        if ( s_redraw_frames > 0 )
        {
            gui()->set_force_redraw( true );
            if ( --s_redraw_frames == 0 )
                s_redraw_frames = -1;    /* window just closed: clear the pin next frame */
        }
        else if ( s_redraw_frames < 0 )
        {
            gui()->set_force_redraw( false );
            s_redraw_frames = 0;
        }

        /* Quake toggle: grave opens/closes, Escape closes.  Focus is queued on open so the
           input line captures the keyboard immediately. */
        if ( app()->key_pressed( APP_KEY_GRAVE ) )
        {
            s_console_open = !s_console_open;
            if ( s_console_open )
            {
                s_focus_pending = true;
                s_view_offset   = 0;
            }
        }
        if ( s_console_open && app()->key_pressed( APP_KEY_ESCAPE ) )
            s_console_open = false;

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            show_test_bed();

            if ( s_console_open )
            {
                i32 win_w = 0, win_h = 0;
                app()->window_get_size( ( i32 )vp0, &win_w, &win_h );
                show_console( ( f32 )win_w );
            }

            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->present_begin( NULL );
        gui()->present_end();
        gui()->frame_pace( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
