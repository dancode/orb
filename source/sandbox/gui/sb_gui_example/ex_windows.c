/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_windows.c -- "Windows" category demos.

    Window-level behavior: the flag playground (every gui_win_flags_t bit toggled live against a
    test-subject window), overlap / z-order, auto-sizing, popups + modals, tooltips, and window
    menu bars.  Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/*==============================================================================================
    Window Playground -- a control window drives every window flag on a live test subject.
    This is the one demo whose subject window does NOT default to free resizing: its flags are
    exactly what the checkboxes say.
==============================================================================================*/

static void
ex_windows_playground( void )
{
    static u32  flags       = 0;            /* gui_win_flags_t bits for the subject     */
    static i32  lines       = 12;           /* subject filler rows                      */
    static bool wide_line   = false;        /* emit an extra-wide row (HSCROLL fodder)  */
    static i32  drag_mode   = GUI_WIN_DRAG_TITLEBAR;

    /* --- the control window (this demo's primary; default resizable like all demos) ------- */
    if ( ex_begin( "Window Playground", 420, 720, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Flags below apply to the 'Flag Test Subject'" );
        gui()->text( "window immediately." );

        gui()->separator_text( "Chrome" );
        ex_flag_checkbox( "NOTITLEBAR",  &flags, GUI_WIN_NOTITLEBAR );
        ex_flag_checkbox( "NOCOLLAPSE",  &flags, GUI_WIN_NOCOLLAPSE );
        ex_flag_checkbox( "MENUBAR",     &flags, GUI_WIN_MENUBAR );
        ex_flag_checkbox( "CLOSEABLE",   &flags, GUI_WIN_CLOSEABLE );
        ex_flag_checkbox( "NO_DETACH (no tear-off / pop-out)", &flags, GUI_WIN_NO_DETACH );
        ex_flag_checkbox( "NO_TAB_TARGET (refuses tab drops)", &flags, GUI_WIN_NO_TAB_TARGET );

        gui()->separator_text( "Move / resize" );
        ex_flag_checkbox( "NOMOVE",            &flags, GUI_WIN_NOMOVE );
        ex_flag_checkbox( "NORESIZE",          &flags, GUI_WIN_NORESIZE );
        ex_flag_checkbox( "NO_BOUNDARY_CLAMP", &flags, GUI_WIN_NO_BOUNDARY_CLAMP );
        ex_flag_checkbox( "ALWAYS_AUTOSIZE",   &flags, GUI_WIN_ALWAYS_AUTOSIZE );
        ex_flag_checkbox( "CAN_AUTOSIZE (corner grip)", &flags, GUI_WIN_CAN_AUTOSIZE );

        gui()->separator_text( "Scrolling" );
        ex_flag_checkbox( "NOSCROLL",       &flags, GUI_WIN_NOSCROLL );
        ex_flag_checkbox( "NOMOUSESCROLL",  &flags, GUI_WIN_NOMOUSESCROLL );
        ex_flag_checkbox( "HSCROLL",        &flags, GUI_WIN_HSCROLL );
        ex_flag_checkbox( "ALWAYS_VSCROLL", &flags, GUI_WIN_ALWAYS_VSCROLL );
        ex_flag_checkbox( "ALWAYS_HSCROLL", &flags, GUI_WIN_ALWAYS_HSCROLL );
        {
            bool show_rect = gui()->debug_content_rect_shown();
            if ( gui()->checkbox( "debug: outline measured content rect", &show_rect ) )
                gui()->debug_show_content_rect( show_rect );
        }

        gui()->separator_text( "Input" );
        ex_flag_checkbox( "NO_INPUT (click-through)", &flags, GUI_WIN_NO_INPUT );
        gui()->same_line( -1.0f );
        gui()->help_marker( "The subject becomes purely visual; uncheck here to get it back." );

        gui()->separator_text( "Composites" );
        gui()->row_cols_n( 0, 3 );
        if ( gui()->button( "NONE" ) )         flags = 0;
        if ( gui()->button( "NODECORATION" ) ) flags = GUI_WIN_NODECORATION;
        if ( gui()->button( "OVERLAY" ) )      flags = GUI_WIN_OVERLAY;
        gui()->row( 0 );
        gui()->textf( "flags = 0x%08X", flags );

        /* NATIVE / NO_MINIMIZE / NO_MAXIMIZE act on a host OS window -- out of scope here. */
        gui()->push_item_flag( GUI_ITEM_DISABLED, true );
        gui()->text( "NATIVE / NO_MINIMIZE / NO_MAXIMIZE: OS-window" );
        gui()->text( "chrome flags -- see viewport_shell, not this demo." );
        gui()->pop_item_flag();

        gui()->separator_text( "Subject content" );
        gui()->slider_int( "filler lines", &lines, 0, 60 );
        gui()->checkbox( "one extra-wide line (feeds HSCROLL)", &wide_line );

        gui()->separator_text( "window_set_drag (GLOBAL: every window)" );
        bool drag_changed = false;
        drag_changed |= gui()->radio_button( "None",     &drag_mode, GUI_WIN_DRAG_NONE );     gui()->same_line( -1.0f );
        drag_changed |= gui()->radio_button( "Titlebar", &drag_mode, GUI_WIN_DRAG_TITLEBAR ); gui()->same_line( -1.0f );
        drag_changed |= gui()->radio_button( "Body",     &drag_mode, GUI_WIN_DRAG_BODY );
        if ( drag_changed )
            gui()->window_set_drag( (gui_win_drag_t)drag_mode );

        /* CLOSEABLE escape hatch: if the subject was X-closed, re-open it from here. */
        bool subject_open = gui()->window_is_open( "Flag Test Subject" );
        gui()->disabled_begin( subject_open );
        if ( gui()->button( "Re-open subject window" ) )
            gui()->window_set_open( "Flag Test Subject", true );
        gui()->disabled_end();
    }
    gui()->window_end();

    /* --- the test subject ------------------------------------------------------------------ */
    gui()->window_set_next_size( 320, 300, GUI_COND_ONCE );
    if ( gui()->window_begin( "Flag Test Subject", (gui_win_flags_t)flags ) )
    {
        /* The reserved menu-bar strip only fills when the flag reserved it. */
        if ( ( flags & GUI_WIN_MENUBAR ) && gui()->menu_bar_begin() )
        {
            if ( gui()->menu_begin( "Menu" ) )
            {
                gui()->menu_item( "an item", NULL, NULL );
                gui()->menu_end();
            }
            gui()->menu_bar_end();
        }

        gui()->stack();
        gui()->text( "I am the test subject." );
        gui()->textf( "flags = 0x%08X", flags );
        if ( wide_line )
            gui()->text( "a deliberately very long line of text that runs far past any "
                         "reasonable window width so horizontal scrolling has something to do" );
        for ( i32 i = 0; i < lines; i++ )
            gui()->textf( "content line %02d", i );
    }
    gui()->window_end();
}

/*==============================================================================================
    Multiple Windows -- overlap, z-order raise on click, the closeable + control pair.
==============================================================================================*/

static void
ex_windows_multi( void )
{
    if ( ex_begin( "Default Window", 280, 200, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Default flags (+ the demo X)." );
        gui()->text( "Title bar, collapse, resize." );
        gui()->text( "Click another window to raise it." );
    }
    gui()->window_end();
    gui()->window_set_next_size( 280, 180, GUI_COND_ONCE );
    if ( gui()->window_begin( "No Title Bar", GUI_WIN_NOTITLEBAR ) )
    {
        gui()->stack();
        gui()->text( "GUI_WIN_NOTITLEBAR" );
        static bool t = false;
        gui()->checkbox( "a toggle", &t );
    }
    gui()->window_end();
    gui()->window_set_next_size( 260, 160, GUI_COND_ONCE );
    if ( gui()->window_begin( "Fixed", GUI_WIN_NORESIZE | GUI_WIN_NOMOVE ) )
    {
        gui()->stack();
        gui()->text( "GUI_WIN_NORESIZE | NOMOVE" );
        gui()->text( "cannot be moved or resized." );
    }
    gui()->window_end();

    /* A closeable window + a control window that re-opens it on demand. */
    gui()->window_set_next_size( 260, 150, GUI_COND_ONCE );
    if ( gui()->window_begin( "Closeable", GUI_WIN_CLOSEABLE ) )
    {
        gui()->stack();
        gui()->text( "GUI_WIN_CLOSEABLE" );
        gui()->text( "Click the X to close me," );
        gui()->text( "then re-open from the control" );
        gui()->text( "window below." );
    }
    gui()->window_end();
    gui()->window_set_next_size( 260, 110, GUI_COND_ONCE );
    if ( gui()->window_begin( "Window Control", GUI_WIN_NONE ) )
    {
        bool open = gui()->window_is_open( "Closeable" );

        gui()->stack();
        gui()->textf( "Closeable window is %s.", open ? "open" : "closed" );
        gui()->disabled_begin( open );
        if ( gui()->button( "Open Closeable window" ) )
            gui()->window_set_open( "Closeable", true );
        gui()->disabled_end();
    }
    gui()->window_end();
}

/*==============================================================================================
    Auto-size -- windows and children that hug their content.
==============================================================================================*/

static void
ex_windows_autosize( void )
{
    /* (a) A window that always hugs its content -- the row count drives its height. */
    if ( gui()->window_begin( "Always Auto-size", GUI_WIN_ALWAYS_AUTOSIZE | GUI_WIN_CLOSEABLE ) )
    {
        static i32 rows = 3;

        gui()->stack();
        gui()->text( "ALWAYS_AUTOSIZE: window fits content." );
        gui()->row2( 0.5f, 0.5f );
        if ( gui()->button( "Add row" ) )    rows++;
        if ( gui()->button( "Remove row" ) ) rows = rows > 0 ? rows - 1 : 0;
        gui()->row( 0 );

        for ( i32 i = 0; i < rows; i++ )
            gui()->textf( "content row %d", i );
    }
    gui()->window_end();

    /* (b) A normal window with a corner grip; double-click it to fit. */
    gui()->window_set_next_size( 300, 320, GUI_COND_ONCE );
    if ( gui()->window_begin( "Double-click grip", GUI_WIN_CAN_AUTOSIZE ) )
    {
        gui()->stack();
        gui()->text( "CAN_AUTOSIZE: drag the triangle" );
        gui()->text( "grip in the corner to resize," );
        gui()->text( "or double-click it to snap back" );
        gui()->text( "to fit this content." );
    }
    gui()->window_end();

    /* (c) Auto-height child (h <= 0) + content_avail(). */
    gui()->window_set_next_size( 320, 260, GUI_COND_ONCE );
    if ( gui()->window_begin( "Auto child", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "child_begin with h <= 0 hugs content:" );
        if ( gui()->child_begin( "auto", 0, 0, GUI_WIN_NOSCROLL ) )
        {
            gui()->stack();
            gui()->text( "I am exactly as tall" );
            gui()->text( "as my three lines." );
            gui()->bullet_text( "no fixed height" );
        }
        gui()->child_end();

        gui_vec2_t avail = gui()->content_avail();
        gui()->textf( "content_avail below: %.0f x %.0f", avail.x, avail.y );
    }
    gui()->window_end();
}

/*==============================================================================================
    Popups & Modals -- the popup stack, modal blocking, close policy, context menus.
==============================================================================================*/

static void
ex_windows_popups( void )
{
    static const char* s_last = "(none)";

    if ( ex_begin( "Popups & Modals", 440, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* Plain popup: opens on request, auto-closes on an outside click. */
        gui()->separator_text( "popup_open / popup_begin" );
        if ( gui()->button( "Open popup" ) )
            gui()->popup_open( "ex_popup" );
        gui()->same_line( -1.0f );
        gui()->textf( "open: %s   last pick: %s",
                      gui()->popup_is_open( "ex_popup" ) ? "yes" : "no", s_last );

        static bool keep_open = false;
        gui()->checkbox( "GUI_ITEM_NO_CLOSE_POPUP (picks keep it open)", &keep_open );

        if ( gui()->popup_begin( "ex_popup", GUI_WIN_NONE ) )
        {
            gui()->stack();
            gui()->text( "A selectable normally closes the popup:" );
            gui()->push_item_flag( GUI_ITEM_NO_CLOSE_POPUP, keep_open );
            static const char* picks[] = { "Cut", "Copy", "Paste" };
            for ( i32 i = 0; i < 3; i++ )
            {
                gui()->push_id_int( i );
                if ( gui()->selectable( picks[ i ], NULL ) )
                    s_last = picks[ i ];
                gui()->pop_id();
            }
            gui()->pop_item_flag();
            gui()->separator();
            if ( gui()->button( "popup_close_current" ) )
                gui()->popup_close_current();
            gui()->popup_end();
        }

        /* Modal: blocks + dims everything behind; only close_current dismisses it. */
        gui()->separator_text( "popup_modal_begin" );
        if ( gui()->button( "Open modal" ) )
            gui()->popup_open( "ex_modal" );
        if ( gui()->popup_modal_begin( "ex_modal", "Confirm Action", GUI_WIN_NONE ) )
        {
            gui()->stack();
            gui()->text( "Input behind this modal is blocked" );
            gui()->text( "and the background is dimmed." );

            /* A popup nested from inside the modal stacks on top of it. */
            if ( gui()->button( "Nested popup" ) )
                gui()->popup_open( "ex_nested" );
            if ( gui()->popup_begin( "ex_nested", GUI_WIN_NONE ) )
            {
                gui()->stack();
                gui()->text( "stacked over the modal" );
                gui()->popup_end();
            }

            gui()->separator();
            gui()->row2( 0.5f, 0.5f );
            if ( gui()->button( "OK" ) )     { s_last = "modal OK";     gui()->popup_close_current(); }
            if ( gui()->button( "Cancel" ) ) { s_last = "modal Cancel"; gui()->popup_close_current(); }
            gui()->row( 0 );
            gui()->popup_end();
        }

        /* Context menus: right-click the previous item / empty window space. */
        gui()->separator_text( "context menus" );
        gui()->selectable( "Right-click this row", NULL );
        if ( gui()->popup_context_item_begin( "row_ctx" ) )
        {
            gui()->stack();
            if ( gui()->menu_item( "Rename", NULL, NULL ) ) s_last = "ctx Rename";
            if ( gui()->menu_item( "Delete", NULL, NULL ) ) s_last = "ctx Delete";
            gui()->popup_end();
        }

        gui()->text( "...or right-click empty space in this window." );
        if ( gui()->popup_context_window_begin( "win_ctx" ) )
        {
            gui()->stack();
            if ( gui()->menu_item( "Window action A", NULL, NULL ) ) s_last = "winctx A";
            if ( gui()->menu_item( "Window action B", NULL, NULL ) ) s_last = "winctx B";
            gui()->popup_end();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Tooltips -- the one-liner, the multi-widget body, and the (?) footnote.
==============================================================================================*/

static void
ex_windows_tooltips( void )
{
    if ( ex_begin( "Tooltips", 400, 360, GUI_WIN_NONE ) )
    {
        gui()->stack();

        gui()->separator_text( "set_item_tooltip (one-liner)" );
        gui()->button( "Hover me" );
        gui()->set_item_tooltip( "Does the thing." );

        gui()->separator_text( "help_marker" );
        gui()->text( "A setting that needs a footnote" );
        gui()->same_line( -1.0f );
        gui()->help_marker( "The dim (?) hint: hover pops this text.\n"
                            "Multi-line bodies are fine." );

        gui()->separator_text( "tooltip_begin (rich body)" );
        gui()->button( "Hover for a rich tooltip" );
        if ( gui()->tooltip_begin() )
        {
            gui()->stack();
            gui()->text( "Tooltips lay out like a window body:" );
            gui()->bullet_text( "any widget works" );
            gui()->progress_bar( 0.4f, NULL );
            gui()->text_disabled( "but they are non-interactive" );
            gui()->tooltip_end();
        }

        gui()->separator_text( "on any widget" );
        static f32 v = 3.0f;
        gui()->slider_float( "value", &v, 0.0f, 10.0f );
        gui()->set_item_tooltip( "Tooltips attach to the previous widget,\nwhatever it is." );
    }
    gui()->window_end();
}

/*==============================================================================================
    Menu Bars -- a window-owned menu bar: submenus, checkable items, disabled items.
==============================================================================================*/

/* Shared across the bar + context menu so a chosen command is visible below. */
static const char* s_mb_last  = "(none)";
static bool        s_mb_grid  = true;
static bool        s_mb_stats = false;

/* The File menu body, factored out so the bar and the context menu can both show it. */
static void
ex_menu_file_body( void )
{
    if ( gui()->menu_item( "New",  NULL,     NULL ) ) s_mb_last = "File / New";
    if ( gui()->menu_item( "Open", "Ctrl+O", NULL ) ) s_mb_last = "File / Open";

    if ( gui()->menu_begin( "Open Recent" ) )
    {
        if ( gui()->menu_item( "scene.orb",   NULL, NULL ) ) s_mb_last = "Recent / scene.orb";
        if ( gui()->menu_item( "level_1.orb", NULL, NULL ) ) s_mb_last = "Recent / level_1.orb";
        if ( gui()->menu_begin( "More.." ) )            /* nested submenu */
        {
            if ( gui()->menu_item( "old.orb",   NULL, NULL ) ) s_mb_last = "Recent / old.orb";
            if ( gui()->menu_item( "older.orb", NULL, NULL ) ) s_mb_last = "Recent / older.orb";
            gui()->menu_end();
        }
        gui()->menu_end();
    }

    if ( gui()->menu_item( "Save",      "Ctrl+S", NULL ) ) s_mb_last = "File / Save";
    if ( gui()->menu_item( "Save As..", NULL,     NULL ) ) s_mb_last = "File / Save As";
    gui()->separator();
    if ( gui()->menu_item( "Quit", "Alt+F4", NULL ) ) s_mb_last = "File / Quit";
}

static void
ex_windows_menus( void )
{
    static bool boxed_check = true;     /* GUI_VAR_MENU_CHECK: bordered box vs plain gutter */

    gui()->push_style_var( GUI_VAR_MENU_CHECK, boxed_check ? 1.0f : 0.0f );

    if ( ex_begin( "Menu Bars", 440, 380, GUI_WIN_MENUBAR ) )
    {
        if ( gui()->menu_bar_begin() )
        {
            if ( gui()->menu_begin( "File" ) )
            {
                ex_menu_file_body();
                gui()->menu_end();
            }
            if ( gui()->menu_begin( "Edit" ) )
            {
                if ( gui()->menu_item( "Undo", "Ctrl+Z", NULL ) ) s_mb_last = "Edit / Undo";

                /* A disabled item -- reuse the item-flag stack. */
                gui()->push_item_flag( GUI_ITEM_DISABLED, true );
                gui()->menu_item( "Redo", "Ctrl+Y", NULL );
                gui()->pop_item_flag();

                gui()->separator();
                if ( gui()->menu_item( "Cut",   "Ctrl+X", NULL ) ) s_mb_last = "Edit / Cut";
                if ( gui()->menu_item( "Copy",  "Ctrl+C", NULL ) ) s_mb_last = "Edit / Copy";
                if ( gui()->menu_item( "Paste", "Ctrl+V", NULL ) ) s_mb_last = "Edit / Paste";
                gui()->menu_end();
            }
            if ( gui()->menu_begin( "View" ) )
            {
                /* Checkable items: pass a bool* -- the tick reflects + toggles the flag. */
                gui()->menu_item( "Show grid",  NULL, &s_mb_grid );
                gui()->menu_item( "Show stats", NULL, &s_mb_stats );
                gui()->menu_end();
            }
            gui()->menu_bar_end();
        }

        gui()->stack();
        gui()->text( "GUI_WIN_MENUBAR reserves the strip;" );
        gui()->text( "menu_bar_begin fills it." );
        gui()->textf( "Last command: %s", s_mb_last );
        gui()->textf( "grid: %s   stats: %s",
                      s_mb_grid ? "ON" : "off", s_mb_stats ? "ON" : "off" );
        gui()->separator();
        gui()->checkbox( "Boxed menu checks (GUI_VAR_MENU_CHECK)", &boxed_check );
        gui()->text( "The app's own bar at the top of the screen" );
        gui()->text( "is main_menu_bar_begin -- same API, pinned." );
        gui()->new_line( -1.0f );
        gui()->text( "Right-click empty space: the same menu" );
        gui()->text( "items work inside any popup." );

        if ( gui()->popup_context_window_begin( "menus_ctx" ) )
        {
            gui()->stack();
            if ( gui()->menu_begin( "File" ) )
            {
                ex_menu_file_body();
                gui()->menu_end();
            }
            gui()->separator();
            gui()->menu_item( "Show stats", NULL, &s_mb_stats );
            gui()->popup_end();
        }
    }
    gui()->window_end();

    gui()->pop_style_var( 1 );
}

/*============================================================================================*/
