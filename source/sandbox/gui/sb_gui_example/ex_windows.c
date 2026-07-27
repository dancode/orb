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
            u32  layers    = gui()->debug_get_layers();
            bool show_rect = ( layers & GUI_DBG_CONTENT ) != 0;
            if ( gui()->checkbox( "debug: outline measured content rect", &show_rect ) )
                gui()->debug_set_layers( layers ^ GUI_DBG_CONTENT );
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
        if ( gui()->is_item_hovered() && gui()->tooltip_begin() )
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

/*==============================================================================================
    Raw Pane -- pane_begin / pane_end: the minimal top-level surface occupant (GUI_STACK_PLAN
    section 5), with ALL chrome hand-built from rect cuts + stock_* renders.  The acceptance check:
    the pane competes for hover/z correctly beside stock windows -- drag the control window
    over it and cycle the tier to watch the one z contest resolve both paint order and input.
==============================================================================================*/

static void
ex_windows_pane( void )
{
    static i32  tier   = 0;      /* index into GUI_REGION_MID / BG / FG          */
    static bool input  = true;   /* off = GUI_WIN_NO_INPUT: pure display         */
    static bool check  = false;
    static i32  clicks = 0;

    /* Control window (stock chrome) -- also the occlusion sparring partner. */
    if ( ex_begin( "Raw Pane", 420, 260, GUI_WIN_NONE ))
    {
        gui()->stack();
        gui()->text( "the pane to the right is pane_begin + hand-built chrome:" );
        gui()->text( "no pool record, no layout, no chrome paint -- rect cuts + stock_*." );
        gui()->text( "drag THIS window over it: MID floats above every window," );
        gui()->text( "BG sinks under any raised window, FG tops even popups." );

        static const char* tiers[] = { "MID (over windows)", "BG (window floor)", "FG (topmost)" };
        gui()->combo( "tier", &tier, tiers, 3 );
        gui()->checkbox( "input (hover contest)", &input );
        gui()->textf( "pane clicks: %d", clicks );
    }
    gui()->window_end();

    /* The raw pane, root-level (never inside a window bracket -- a pane IS a root surface).
       Position/size are caller-owned values; a real HUD would compute them from the viewport. */
    gui_region_tier_t t = ( tier == 1 ) ? GUI_REGION_BG
                        : ( tier == 2 ) ? GUI_REGION_FG
                        :                 GUI_REGION_MID;

    gui_rect_t r = { 620.0f, 240.0f, 250.0f, 180.0f };
    gui_pane_t p = gui()->pane_begin( "ex_raw_pane", r, t, 0,
                                      input ? GUI_WIN_NONE : GUI_WIN_NO_INPUT );

    /* Hand-built chrome: backdrop, title band, body rows -- every rect accounted for. */
    gui_rect_t body = p.rect;
    gui()->draw_frame( body, GUI_COLOR( 0x16, 0x1A, 0x22, 0xF0 ),
                             GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF ), 1.0f );

    gui_rect_t title = gui_rect_cut_top( &body, 26.0f );
    gui()->draw_rect( title.x, title.y, title.w, title.h, GUI_COLOR( 0x20, 0x2A, 0x38, 0xFF ) );
    gui()->stock_label( gui_rect_pad( title, 6.0f ), GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                     input ? "raw pane" : "raw pane (display only)" );

    body = gui_rect_pad( body, 10.0f );
    gui_rect_t row = gui_rect_cut_top( &body, 30.0f );
    if ( gui()->stock_button( row, "click me##pane" ) )
        clicks++;

    gui_rect_cut_top( &body, 8.0f );   /* gap */
    row = gui_rect_cut_top( &body, 24.0f );
    gui()->stock_check( gui_rect_cut_left( &row, 24.0f ), "##pane_chk", &check );
    gui()->stock_label( gui_rect_pad( row, 4.0f ), GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                     check ? "checked" : "unchecked" );

    gui_rect_t foot = gui_rect_cut_bottom( &body, 20.0f );
    char zbuf[ 48 ];
    snprintf( zbuf, sizeof( zbuf ), "z 0x%08X  vp %u", p.z, (u32)p.vp );
    gui()->stock_label( foot, GUI_ALIGN_LEFT | GUI_ALIGN_BOTTOM, zbuf );

    gui()->pane_end();
}

/*==============================================================================================
    Feature Kit -- the GUI_STACK_PLAN section-6 acceptance sketch, live: a window assembled
    feature by feature over a pane.  feat_maximize + feat_clamp shape the rect (B-features:
    the work area is passed IN), feat_collapse tweens the height off a caller bool, the title
    band drags through feat_move, the edges resize through feat_resize, and close is nothing
    but a static bool a hand-placed stock_button clears.  Every persistent byte is a demo static;
    gui holds only tween scratch.
==============================================================================================*/

static void
ex_windows_features( void )
{
    static gui_rect_t rect      = { 620.0f, 300.0f, 300.0f, 220.0f };
    static gui_rect_t restore;             /* feat_maximize's save slot (caller-owned too) */
    static bool       open      = true;
    static bool       folded    = false;
    static bool       maximized = false;
    static i32        clicks    = 0;
    static bool       check     = false;

    const gui_id_t feat_id = 0x0FEA0001u;  /* mechanisms key on any caller-owned id */
    const f32      title_h = 26.0f;

    /* Control window: the open latch's re-open side + state readout. */
    if ( ex_begin( "Feature Kit", 400, 220, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "a window assembled feature by feature over a pane:" );
        gui()->text( "feat_maximize/clamp shape the rect, feat_collapse" );
        gui()->text( "tweens the height, the title band drags (feat_move)," );
        gui()->text( "edges resize (feat_resize), close is a plain bool." );
        gui()->disabled_begin( open );
        if ( gui()->button( "re-open the feature window" ) )
            open = true;
        gui()->disabled_end();
        gui()->textf( "clicks %d   folded %d   maximized %d", clicks, folded, maximized );
    }
    gui()->window_end();

    if ( !open )
        return;

    /* Work area of the main viewport -- B-features take it as a PARAMETER. */
    i32 vw = 0, vh = 0;
    gui()->viewport_size( 0, &vw, &vh );
    f32        top  = gui()->viewport_content_y( 0 );
    gui_rect_t work = { 0.0f, top, (f32)vw, (f32)vh - top };

    gui()->feat_maximize( feat_id, maximized, &rect, &restore, work );
    if ( !maximized )
        gui()->feat_clamp( &rect, work, title_h );

    f32 disp_h = gui()->feat_collapse( feat_id, !folded, title_h, rect.h );

    gui_pane_t p = gui()->pane_begin( "ex_feat_pane",
                                      ( gui_rect_t ){ rect.x, rect.y, rect.w, disp_h },
                                      GUI_REGION_MID, 0, GUI_WIN_NONE );

    /* Edge resize FIRST, all four edges, so an edge grab pre-empts the title-band move arm
       (stock chrome resolves resize before its drag for the same reason -- the T band and
       the titlebar overlap by a few px). */
    if ( !maximized && !folded )
        gui()->feat_resize( feat_id, &rect,
                            GUI_RESIZE_L | GUI_RESIZE_R | GUI_RESIZE_T | GUI_RESIZE_B,
                            160.0f, 120.0f );

    /* Hand-built chrome over the pane rect. */
    gui_rect_t body = p.rect;
    gui()->draw_frame( body, GUI_COLOR( 0x16, 0x1A, 0x22, 0xF4 ),
                             GUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF ), 1.0f );

    gui_rect_t title = gui_rect_cut_top( &body, title_h );
    gui()->draw_rect( title.x, title.y, title.w, title.h, GUI_COLOR( 0x2C, 0x24, 0x18, 0xFF ) );

    /* Title buttons BEFORE feat_move, cut off the band so the drag handle excludes them (and
       a press they claim blocks the move arm -- the same order stock chrome resolves). */
    if ( gui()->stock_button( gui_rect_cut_right( &title, title_h ), "x##feat_close" ) )
        open = false;                                       /* the open latch: just a bool */
    if ( gui()->stock_button( gui_rect_cut_right( &title, title_h ), maximized ? "v##feat_max"
                                                                            : "^##feat_max" ) )
        maximized = !maximized;
    if ( gui()->stock_button( gui_rect_cut_right( &title, title_h ), folded ? ">##feat_fold"
                                                                         : "-##feat_fold" ) )
        folded = !folded;

    if ( !maximized )
        gui()->feat_move( feat_id, title, &rect.x, &rect.y );
    gui()->stock_label( gui_rect_pad( title, 6.0f ), GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                     "feature window" );

    /* Body -- only when the collapse tween has left room. */
    if ( disp_h > title_h + 8.0f )
    {
        body = gui_rect_pad( body, 10.0f );
        gui_rect_t row = gui_rect_cut_top( &body, 30.0f );
        if ( gui()->stock_button( row, "click me##feat" ) )
            clicks++;
        gui_rect_cut_top( &body, 8.0f );
        row = gui_rect_cut_top( &body, 24.0f );
        gui()->stock_check( gui_rect_cut_left( &row, 24.0f ), "##feat_chk", &check );
        gui()->stock_label( gui_rect_pad( row, 4.0f ), GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                         "assembled from mechanisms" );
    }

    gui()->pane_end();
}

/*============================================================================================*/
