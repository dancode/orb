/*==============================================================================================

    runtime_service/imgui/imgui_widget_menu.c -- Menus: menu bars, menu entries, menu items.

    Menus are a thin coordination layer over the popup stack (imgui_popup.c), built the same way
    the combo box was: a widget that drives a popup plus a little local state, no new machinery.

      menu_item    -- a leaf row: a check-mark gutter, the label, and a right-aligned dim shortcut.
                      A click toggles its optional *selected and dismisses the whole menu chain
                      (every open popup down to the topmost modal), the way picking a command should.

      menu_begin   -- a submenu entry.  It renders horizontally when emitted into a bar (pack mode:
                      a label that drops its popup *below*) and as a full-width row with a right
                      arrow inside a menu (stack mode: a popup opening to the *side*).  Each opens a
                      popup keyed off its widget id, so submenus nest directly on the popup stack.
                      The two menu reflexes layered on top of the popup's click-outside close:
                        - click-to-open then hover-to-switch among bar entries (only once one is open)
                        - hover-opens-submenu inside a menu, with sibling rows replacing each other
                      Both fall out of two popup-stack queries (is this one open / is a sibling open)
                      plus the same was-open guard the combo uses to stop a toggle click from
                      closing-then-reopening on the same frame.

      main_menu_bar_begin -- a helper window pinned across the top of the display, then a bar().
      menu_bar_begin      -- the strip a WIN_MENUBAR window reserved below its title bar (a region
                             over s_build.menubar_rect, drawn outside the body's scrolling flow).

    Included by imgui.c after imgui_widget_combo.c, so the popup internals (popup_open_id,
    popup_is_open_id, popup_set_anchor, popup_begin_common_id, the s_popups_open stack, the
    IMGUI_POPUP_* constants), the region push/pop helpers, and every widget / layout primitive are
    all in scope.

==============================================================================================*/
// clang-format off

/* Menu-bar region id salt, so a window's menu-bar strip never shares a record with the window
   body region (which keys off the bare window id). */
#define IMGUI_MENUBAR_SALT   0x4D454E55u    /* 'MENU' */

/* Persistent per-entry state: the last frame this menu's popup body emitted.  Distinguishes a
   click that should close an open bar menu from one that should open a closed one -- popup_close_check
   has already dropped the popup by the time menu_begin runs, so without it the same click would
   close then immediately reopen (the exact problem combo solves the same way). */
typedef struct { u32 open_frame; } imgui_menu_state_t;

/*----------------------------------------------------------------------------------------------
    menu_close_chain -- dismiss the whole open menu stack down to the topmost modal.

    Selecting a command closes every menu and submenu at once.  The popups above a modal are the
    menu chain (a context menu or a bar's menus + their submenus); truncating to just past the
    topmost modal closes them all while leaving a modal that hosts the menu open.  popup_begin
    for those popups already ran this frame and returned true, so the bodies still finish rendering;
    they are simply gone next frame.
----------------------------------------------------------------------------------------------*/

static void
menu_close_chain( void )
{
    u32 floor = 0;
    for ( u32 i = 0; i < s_popup_open_count; ++i )
        if ( s_popups_open[ i ].modal ) floor = i + 1u;
    s_popup_open_count = floor;
}

/*----------------------------------------------------------------------------------------------
    menu_item -- a leaf command row.
----------------------------------------------------------------------------------------------*/

bool
imgui_menu_item( const char* label, const char* shortcut, bool* selected )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Pointing at a leaf row -- by mouse or by the nav cursor -- collapses any submenu open at this
       depth, so moving off a sibling menu_begin onto a plain item closes that submenu: the menu
       reads as one active path under either input. */
    if ( ( st.hover || st.nav ) && s_popup_open_count > s_popup_begin_count )
        s_popup_open_count = s_popup_begin_count;

    /* Row highlight on hover / nav (active tint while pressed). */
    if ( st.hover || st.nav )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    /* A fixed check-mark gutter on the left so checkable and plain items align; the indicator
       (a 'v' tick, or a disc per IMGUI_VAR_CHECK_STYLE -- matching the checkbox) is drawn only
       when *selected. */
    f32 check_w = CHECKBOX_SZ + WIDGET_PAD;
    if ( selected && *selected )
    {
        f32 bx = r.x + WIDGET_PAD;
        f32 by = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, IMGUI_ALIGN_VCENTER ).y;
        draw_check_indicator( ( imgui_rect_t ){ bx, by, CHECKBOX_SZ, CHECKBOX_SZ }, COL_CHECK_MARK );
    }

    f32 lx = r.x + WIDGET_PAD + check_w;
    draw_label( lx, text_center_y( r.y, r.h ), COL_TEXT, label );

    /* Shortcut hint, dim and right-aligned in the row. */
    f32 sw = ( shortcut && shortcut[ 0 ] ) ? font_text_w( shortcut ) : 0.0f;
    if ( sw > 0.0f )
        draw_push_text( r.x + r.w - WIDGET_PAD - sw, text_center_y( r.y, r.h ), COL_TEXT_DIM, shortcut );

    /* Natural row width (gutter + label + a gap + shortcut) so the menu popup auto-sizes to its
       widest row over two frames, like the combo dropdown. */
    f32 natural = lx + label_width( label );
    if ( sw > 0.0f ) natural += WIDGET_PAD * 2.0f + sw;
    widget_track_width( natural );

    if ( st.clicked )
    {
        if ( selected ) *selected = !( *selected );
        menu_close_chain();
        return true;
    }
    return false;
}

/*----------------------------------------------------------------------------------------------
    menu_begin / menu_end -- a submenu entry that drives a nested popup.
----------------------------------------------------------------------------------------------*/

bool
imgui_menu_begin( const char* label )
{
    imgui_id_t id  = widget_id( label );
    imgui_id_t pid = id_combine( id, IMGUI_POPUP_SALT );

    /* Orientation from the active layout mode: a bar (pack) renders a horizontal label whose popup
       drops below; a menu (stack) renders a full-width row whose popup opens to the side. */
    bool in_bar = ( lf()->mode == IMGUI_MODE_PACK );

    imgui_rect_t box;
    f32          anchor_x, anchor_y;
    if ( in_bar )
    {
        box      = widget_next_rect_w( label_width( label ) + 2.0f * WIDGET_PAD, WIDGET_H );
        anchor_x = box.x;
        anchor_y = box.y + box.h;            /* drop below the bar label */
    }
    else
    {
        box      = widget_next_rect( WIDGET_H );
        anchor_x = box.x + box.w;            /* open to the right of the row */
        anchor_y = box.y;
    }

    widget_state_t st = widget_behavior( id, box, WIDGET_KIND_BUTTON );

    imgui_menu_state_t* ms = IMGUI_STATE( imgui_menu_state_t, id );
    bool was_open     = ( ms->open_frame + 1u == s_retained.frame );
    bool this_open    = popup_is_open_id( pid );
    bool sibling_open = ( s_popup_open_count > s_popup_begin_count );

    /* Open policy.  Bar: a click toggles (a click while open closes -- guarded by was_open since
       popup_close_check already dropped it at frame top), and once any bar menu is open, hovering a
       sibling switches to it.  Menu: hovering or clicking a row opens its submenu.  A popup_open_id
       at this depth replaces whatever sibling was open and truncates anything deeper, so switching
       is automatic. */
    /* Keyboard reflexes layered onto the mouse ones (driven by the menu-bar nav state in s_nav):

         bar_nav -- in menu-bar mode, the nav-highlighted bar entry drops its menu, so Left/Right
                    traversal of the bar shows each menu in turn (the existing && !this_open guard
                    keeps it from re-opening every frame).
         mnem    -- an Alt+letter mnemonic matched this bar entry's leading letter: select + open it
                    and consume the request (issue: Alt+F opens File).
         nav_right -- inside a menu, a Right move on a submenu row opens it; nav descends next frame
                    as the new popup becomes the top one and captures nav. */
    bool bar_nav = in_bar && st.nav && s_nav.bar_win == s_build.win_id && !s_nav.in_menus;

    u8   lead = ( label[ 0 ] != '#' )
              ? (u8)( ( label[ 0 ] >= 'a' && label[ 0 ] <= 'z' ) ? label[ 0 ] - 32 : label[ 0 ] )
              : 0u;
    bool mnem = in_bar && s_nav.mnemonic != 0 && lead == s_nav.mnemonic;
    if ( mnem )
    {
        s_nav.id       = id;                 /* highlight this entry from next frame on */
        s_nav.bar_win  = s_build.win_id;       /* drive this window's bar */
        s_nav.in_menus = false;
        s_nav.mnemonic = 0;                  /* consume */
    }

    bool nav_right = ( st.nav && s_nav.move_dir == IMGUI_DIR_RIGHT );

    bool do_open;
    if ( in_bar )
        do_open = st.clicked ? !was_open : ( ( sibling_open && st.hover ) || bar_nav || mnem );
    else
        do_open = st.clicked || st.hover || nav_right;

    if ( do_open && !this_open )
        popup_open_id( pid, anchor_x, anchor_y );

    /* Keep the popup pinned under / beside the entry every frame it is open (so it tracks a dragged
       parent window), exactly as the combo dropdown re-anchors its box. */
    if ( popup_is_open_id( pid ) )
        popup_set_anchor( pid, anchor_x, anchor_y );

    /* Entry visuals: lit while hovered / nav-highlighted or while its submenu is open. */
    if ( st.hover || st.nav || this_open )
        draw_push_rect_filled( box.x, box.y, box.w, box.h, 0,0,1,1, 0,
                               this_open ? COL_WIDGET_ACT : COL_WIDGET_HOT );

    draw_label( box.x + WIDGET_PAD, text_center_y( box.y, box.h ), COL_TEXT, label );

    if ( in_bar )
    {
        widget_track_width( box.x + box.w );
    }
    else
    {
        /* Submenu marker: a right-pointing arrow boxed at the row's right edge. */
        imgui_rect_t arrow = { box.x + box.w - box.h, box.y, box.h, box.h };
        draw_arrow( arrow, IMGUI_DIR_RIGHT, COL_TEXT );
        widget_track_width( box.x + WIDGET_PAD + label_width( label ) + box.h + WIDGET_PAD );
    }

    /* The submenu is an auto-size, stack-bodied popup keyed off this entry's id. */
    bool vis = popup_begin_common_id( pid, NULL, IMGUI_WIN_NOTITLEBAR | IMGUI_POPUP_BASE_FLAGS,
                                      false, 0.0f, 0.0f );
    if ( vis )
    {
        ms->open_frame = s_retained.frame;   /* body emitted this frame -> "open" next frame */
        imgui_stack();                      /* a menu body is a vertical list */
    }
    return vis;
}

void
imgui_menu_end( void )
{
    imgui_popup_end();
}

/*----------------------------------------------------------------------------------------------
    main_menu_bar_begin / main_menu_bar_end -- a helper window pinned across the top of the display.
----------------------------------------------------------------------------------------------*/

bool
imgui_main_menu_bar_begin( void )
{
    f32 h = WIDGET_H + WIDGET_GAP;

    /* Sit just below the host's native caption band (caption_inset), not at the very top: a
       borderless shell owns the caption strip for the OS move/resize gesture, and a bar painted
       over it would swallow the clicks that drag the window.  Inset is 0 with no native shell, so
       the bar stays pinned to the top edge as before. */
    f32 top = g_ctx->viewports[ 0 ].caption_inset;

    imgui_window_set_next_pos ( 0.0f, top, IMGUI_COND_ALWAYS );
    imgui_window_set_next_size( (f32)s_io.display_w, h, IMGUI_COND_ALWAYS );

    bool vis = imgui_window_begin( "##MainMenuBar",
                                   IMGUI_WIN_NOTITLEBAR | IMGUI_WIN_NOMOVE | IMGUI_WIN_NORESIZE
                                   | IMGUI_WIN_NOCOLLAPSE | IMGUI_WIN_NOSCROLL );
    if ( vis )
        imgui_bar();        /* the menu labels pack horizontally */
    return vis;
}

void
imgui_main_menu_bar_end( void )
{
    imgui_window_end();
}

/*----------------------------------------------------------------------------------------------
    menu_bar_begin / menu_bar_end -- the strip a WIN_MENUBAR window reserved below its title bar.

    The strip rect was carved off the top of the body in window_begin_ex and stashed in
    s_build.menubar_rect.  We open a transient pack region over it (outside the body's scrolling
    flow) and restore the body pen on pop, so the body content lays out from its own origin
    regardless of the strip region's parent-pen advance.
----------------------------------------------------------------------------------------------*/

static f32          s_menubar_sink[ 4 ];     /* scroll / content-measure sink: the strip never scrolls */
static f32          s_menubar_saved_cursor;  /* body pen to restore after the strip region pops        */
static imgui_rect_t s_menubar_saved_clip;    /* body hit-test clip to restore after the strip region pops */

bool
imgui_menu_bar_begin( void )
{
    if ( !( s_build.win_flags & IMGUI_WIN_MENUBAR ) )
        return false;

    imgui_rect_t bar = s_build.menubar_rect;

    /* Strip background, a touch distinct from the body. */
    draw_push_rect_filled( bar.x, bar.y, bar.w, bar.h, 0,0,1,1, 0, COL_TITLE_BG );

    /* Save the body pen: the strip is drawn outside the body flow, so the body resumes from here. */
    s_menubar_saved_cursor = lf()->cursor_y;

    /* The strip sits ABOVE the body region that is currently on the stack, so the live clip_rect
       (the body's, which starts below the strip) excludes it entirely.  layout_push_region with
       own_clip false narrows the new region's hit-test clip to parent_clip & outer; left as-is the
       intersection with the body clip would be empty and every entry would fail rect_hit -- the
       mouse could never hover the bar (only keyboard nav, which skips the clip test, reached it).
       Point the parent clip at the whole window rect for the push so the strip's hit-test clip
       resolves to the strip itself, then restore the body clip in menu_bar_end. */
    s_menubar_saved_clip = s_build.clip_rect;
    s_build.clip_rect = ( imgui_rect_t ){ s_build.win_x, s_build.win_y, s_build.win_w, s_build.win_h };

    s_menubar_sink[ 0 ] = s_menubar_sink[ 1 ] = s_menubar_sink[ 2 ] = s_menubar_sink[ 3 ] = 0.0f;
    layout_push_region( id_combine( s_build.win_id, IMGUI_MENUBAR_SALT ), bar,
                        ( imgui_pad_t ){ WIDGET_PAD, WIDGET_PAD, WIN_BORDER, 0.0f },
                        IMGUI_WIN_NOSCROLL,
                        &s_menubar_sink[ 0 ], &s_menubar_sink[ 1 ],
                        &s_menubar_sink[ 2 ], &s_menubar_sink[ 3 ],
                        /* own_clip */ false );
    imgui_bar();            /* the menu labels pack horizontally */
    return true;
}

void
imgui_menu_bar_end( void )
{
    if ( !( s_build.win_flags & IMGUI_WIN_MENUBAR ) )
        return;

    layout_pop_region();
    s_build.clip_rect = s_menubar_saved_clip;   /* restore the body hit-test clip (pop left it at the window rect) */
    lf()->cursor_y = s_menubar_saved_cursor;     /* undo the strip pop's body-pen advance */
}

// clang-format on
/*============================================================================================*/
