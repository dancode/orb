/*==============================================================================================

    runtime_service/gui/chrome/popup/gui_menu.c -- Menus: menu bars, menu entries, menu items.

    Menus are a thin coordination layer over the popup stack (gui_popup.c), built the same way
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
                             over s_build.win.menubar_rect, drawn outside the body's scrolling flow).

    Included by gui_chrome.c after gui_combo.c, so the popup internals (popup_open_id,
    popup_is_open_id, popup_set_anchor, popup_begin_common_id, the g_ctx->popup.open stack, the
    GUI_POPUP_* constants), the region push/pop helpers, and every widget / layout primitive are
    all in scope.

==============================================================================================*/
// clang-format off

/* Menu-bar region id salt, so a window's menu-bar strip never shares a record with the window
   body region (which keys off the bare window id). */
#define GUI_MENUBAR_SALT   0x4D454E55u    /* 'MENU' */

/* Persistent per-entry state: the last frame this menu's popup body emitted.  Distinguishes a
   click that should close an open bar menu from one that should open a closed one -- popup_close_check
   has already dropped the popup by the time menu_begin runs, so without it the same click would
   close then immediately reopen (the exact problem combo solves the same way). */
typedef struct { u32 open_frame; } gui_menu_state_t;

/*==============================================================================================
    menu_close_chain -- dismiss the whole open menu stack down to the topmost modal.

    Selecting a command closes every menu and submenu at once.  The popups above a modal are the
    menu chain (a context menu or a bar's menus + their submenus); truncating to just past the
    topmost modal closes them all while leaving a modal that hosts the menu open.  popup_begin
    for those popups already ran this frame and returned true, so the bodies still finish rendering;
    they are simply gone next frame.
==============================================================================================*/

static void
menu_close_chain( void )
{
    g_ctx->popup.open_count = popup_modal_floor();   /* truncate to just past the topmost modal */
}

/*==============================================================================================
    menu_item -- a leaf command row.
==============================================================================================*/

bool
gui_menu_item( const char* label, const char* shortcut, bool* selected )
{
    gui_id_t   id = item_id( label );
    gui_rect_t r  = cell_next( WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    /* Pointing at a leaf row -- by mouse or by the nav cursor -- collapses any submenu open at this
       depth, so moving off a sibling menu_begin onto a plain item closes that submenu: the menu
       reads as one active path under either input. */
    if ( ( st.hover || st.nav ) && g_ctx->popup.open_count > s_popup_begin_count )
        g_ctx->popup.open_count = s_popup_begin_count;

    /* Row highlight on hover / nav (active tint while pressed).  ONE mix for the row and its
       check gutter, gated on the weights rather than the live flags so the highlight fades out
       after the cursor has moved on instead of being cut at its first frame. */
    gui_style_mix_t mix = style_mix( id, st, false );
    if ( mix.hot > 0.0f || mix.act > 0.0f )
        draw_face_mix( r, GUI_ROLE_BG, mix );

    /* A fixed check-mark gutter on the left so checkable and plain items align.  With
       GUI_MENU_CHECK_BOX (default) a bordered idle box is always drawn when the item has a
       *selected pointer, matching the visual weight of a standalone checkbox; with
       GUI_MENU_CHECK_PLAIN only the indicator appears when selected and the gutter is empty
       otherwise. */
    f32 check_w = CHECKBOX_SZ + WIDGET_PAD;
    if ( selected )
    {
        f32 bx = r.x + WIDGET_PAD;
        f32 by = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, GUI_ALIGN_VCENTER ).y;
        bool draw_box = ( style_shape( GUI_VAR_MENU_CHECK ) == GUI_MENU_CHECK_BOX );
        if ( draw_box )
        {
            draw_face_mix( ( gui_rect_t ){ bx, by, CHECKBOX_SZ, CHECKBOX_SZ }, GUI_ROLE_BG, mix );
            draw_push_rect_outline( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, WIN_BORDER, COL_BORDER_IDLE );
        }
        if ( *selected )
            draw_check_indicator( ( gui_rect_t ){ bx, by, CHECKBOX_SZ, CHECKBOX_SZ }, COL_MARK_IDLE );
    }

    f32 lx = r.x + WIDGET_PAD + check_w;
    draw_label( lx, text_center_y( r.y, r.h ), COL_TEXT_IDLE, label );

    /* Shortcut hint, dim and right-aligned in the row. */
    f32 sw = ( shortcut && shortcut[ 0 ] ) ? font_text_w( shortcut ) : 0.0f;
    if ( sw > 0.0f )
        draw_push_text( r.x + r.w - WIDGET_PAD - sw, text_center_y( r.y, r.h ), COL_TEXT_DIM, shortcut );

    /* Natural row width (gutter + label + a gap + shortcut) so the menu popup auto-sizes to its
       widest row over two frames, like the combo dropdown. */
    f32 natural = lx + label_width( label ) + WIDGET_PAD;
    if ( sw > 0.0f ) natural += WIDGET_PAD + sw;
    cell_reach( natural );

    if ( st.clicked )
    {
        if ( selected ) *selected = !( *selected );
        menu_close_chain();
        return true;
    }
    return false;
}

/*==============================================================================================
    menu_begin / menu_end -- a submenu entry that drives a nested popup.
==============================================================================================*/

bool
gui_menu_begin( const char* label )
{
    gui_id_t id  = item_id( label );
    gui_id_t pid = id_combine( id, GUI_POPUP_SALT );

    /* Orientation from the active layout mode: a bar (pack) renders a horizontal label whose popup
       drops below; a menu (stack) renders a full-width row whose popup opens to the side. */
    bool in_bar = ( lf()->mode == GUI_MODE_PACK );

    gui_rect_t box;
    f32          anchor_x, anchor_y;
    if ( in_bar )
    {
        box      = cell_next_w( label_natural_w( label ), WIDGET_H );
        anchor_x = box.x;
        anchor_y = box.y + box.h;            /* drop below the bar label */
    }
    else
    {
        box      = cell_next( WIDGET_H );
        anchor_x = box.x + box.w;            /* open to the right of the row */
        anchor_y = box.y;
    }

    gui_item_state_t st = item_state( id, box, ITEM_BUTTON );

    gui_menu_state_t* ms = GUI_STATE( gui_menu_state_t, id );
    bool was_open     = ( ms->open_frame + 1u == gui_frame_index() );
    bool this_open    = popup_is_open_id( pid );
    bool sibling_open = ( g_ctx->popup.open_count > s_popup_begin_count );

    /* Open policy.  Bar: a click toggles (a click while open closes -- guarded by was_open since
       popup_close_check already dropped it at frame top), and once any bar menu is open, hovering a
       sibling switches to it.  Menu: hovering or clicking a row opens its submenu.  A popup_open_id
       at this depth replaces whatever sibling was open and truncates anything deeper, so switching
       is automatic. */
    /* Keyboard reflexes layered onto the mouse ones (driven by the menu-bar nav state in g_ctx->nav):

         bar_nav -- in menu-bar mode, the nav-highlighted bar entry drops its menu, so Left/Right
                    traversal of the bar shows each menu in turn (the existing && !this_open guard
                    keeps it from re-opening every frame).
         mnem    -- an Alt+letter mnemonic matched this bar entry's leading letter: select + open it
                    and consume the request (issue: Alt+F opens File).
         nav_right -- inside a menu, a Right move on a submenu row opens it; nav descends next frame
                    as the new popup becomes the top one and captures nav. */
    bool bar_nav = in_bar && st.nav && g_ctx->nav.bar_win == s_build.win.id && !g_ctx->nav.in_menus;

    /* Mnemonic key: this entry's leading letter, upper-cased.  '#' leads a hidden label -- no mnemonic. */
    u8 lead = ( label[ 0 ] == '#' ) ? 0u : (u8)label[ 0 ];
    if ( lead >= 'a' && lead <= 'z' )
        lead -= 32u;
    bool mnem = in_bar && g_ctx->nav.mnemonic != 0 && lead == g_ctx->nav.mnemonic;
    if ( mnem )
    {
        g_ctx->nav.id       = id;                 /* highlight this entry from next frame on */
        g_ctx->nav.bar_win  = s_build.win.id;       /* drive this window's bar */
        g_ctx->nav.in_menus = false;
        g_ctx->nav.mnemonic = 0;                  /* consume */
    }

    bool nav_right = ( st.nav && g_ctx->nav.move_dir == GUI_DIR_RIGHT );

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

    /* Entry visuals: lit while hovered / nav-highlighted or while its submenu is open.  An open
       entry is the CHOSEN one of its bar -- the SELECT plane -- so it keeps its hover step and a
       cursor moving along an open menu bar still reads as moving. */
    gui_style_mix_t bmix = style_mix( id, st, this_open );
    if ( bmix.hot > 0.0f || bmix.act > 0.0f || bmix.sel > 0.0f )
        draw_push_rect_filled( box.x, box.y, box.w, box.h, 0,0,1,1, 0,
                               style_col_mix( GUI_ROLE_BG, bmix ) );

    draw_label( box.x + WIDGET_PAD, text_center_y( box.y, box.h ), COL_TEXT_IDLE, label );

    if ( in_bar )
    {
        cell_reach( box.x + box.w );
    }
    else
    {
        /* Submenu marker: a right-pointing arrow boxed at the row's right edge. */
        gui_rect_t arrow = { box.x + box.w - box.h, box.y, box.h, box.h };
        draw_arrow( arrow, GUI_DIR_RIGHT, COL_TEXT_IDLE );
        cell_reach( box.x + WIDGET_PAD + label_width( label ) + box.h + WIDGET_PAD );
    }

    /* The submenu is an auto-size, stack-bodied popup keyed off this entry's id. */
    bool vis = popup_begin_common_id( pid, NULL, GUI_WIN_NOTITLEBAR | GUI_POPUP_BASE_FLAGS,
                                      false, 0.0f, 0.0f );
    if ( vis )
    {
        ms->open_frame = gui_frame_index();   /* body emitted this frame -> "open" next frame */
        gui_stack();                      /* a menu body is a vertical list */
    }
    return vis;
}

void
gui_menu_end( void )
{
    gui_popup_end();
}

/*==============================================================================================
    main_menu_bar_begin / main_menu_bar_end -- a helper window pinned across the top of the display.
==============================================================================================*/

/* The bar's band height, published so hosts can stack their own strips (toolbar, dockspace
   inset) below it without re-deriving the sum from font metrics.  Must match the size
   main_menu_bar_begin requests below. */
f32
gui_main_menu_bar_h( void )
{
    return WIDGET_H + WIDGET_GAP;
}

bool
gui_main_menu_bar_begin( void )
{
    f32 h = gui_main_menu_bar_h();

    /* Sit just below the host's native caption band (caption_inset), not at the very top: a
       borderless shell owns the caption strip for the OS move/resize gesture, and a bar painted
       over it would swallow the clicks that drag the window.  Inset is 0 with no native shell, so
       the bar stays pinned to the top edge as before. */
    f32 top = s_vp_pool[ 0 ].caption_inset;

    /* Publish the bar band to the surface, frame-stamped (emit-gated like a dockspace): the
       work area for maximized windows and the drag clamp starts below it -- window_work_top,
       gui_window_free.c. */
    s_vp_pool[ 0 ].bar_inset      = h;
    s_vp_pool[ 0 ].bar_seen_frame = gui_frame_index();

    /* Chrome band: the bar paints with the root regions -- above every normal window, below its
       own dropdown popups -- so a window that still overlaps it (NO_BOUNDARY_CLAMP, a gesture
       overshooting the clamp) slides under, matching the caption band and the viewport border.
       Stamped before begin so this frame's sort key reads it; no record exists on the very
       first frame, where the appearing raise already lands the bar above all current windows
       (as does the raise-on-press for a click frame -- both self-correct here next frame). */
    gui_window_t* bar = window_find( id_hash( "##MainMenuBar" ) );
    if ( bar )
        bar->z = GUI_REGION_Z;

    gui_window_set_next_pos ( 0.0f, top, GUI_COND_ALWAYS );
    gui_window_set_next_size( (f32)s_io.display_w, h, GUI_COND_ALWAYS );

    /* Bar sits flush across the display edge: pin the window radius to 0 only for the body-background
       fill window_begin draws inline below, so it comes out square the first time rather than reading
       the ambient GUI_VAR_PANEL_ROUND theme value.  Popped immediately after -- narrow on purpose, so
       it does not also flatten a dropdown/submenu popup a caller opens between begin and end. */
    style_push_var( GUI_VAR_PANEL_ROUND, 0.0f );

    bool vis = gui_window_begin( "##MainMenuBar",
                                   GUI_WIN_NOTITLEBAR | GUI_WIN_NOMOVE | GUI_WIN_NORESIZE
                                   | GUI_WIN_NOCOLLAPSE | GUI_WIN_NOSCROLL );
    style_pop_var( 1 );

    if ( vis )
    {
        /* Center the entry row in the band.  The window body opens with REGION_PAD_DEFAULT (top pad
           WIDGET_GAP), which seats the WIDGET_H row flush against the bottom of the WIDGET_H +
           WIDGET_GAP band -- 8px above, 0 below, so the labels sit visibly low.  The band carries
           exactly one gap of slack over the row height; lift the pen by half of it so the slack
           splits evenly above and below.  Must run BEFORE gui_bar(): the pack line latches its y
           (line.cross) when it opens, so a lift after the fact moves only the pen and the entries
           still place at the stale, flush-bottom line. */
        layout_pen_jump( lf(), lf()->pen_y - WIDGET_GAP * 0.5f );

        gui_bar();        /* the menu labels pack horizontally */
    }
    return vis;
}

void
gui_main_menu_bar_end( void )
{
    /* Same narrow bracket as begin, this time around the border outline window_end draws inline. */
    style_push_var( GUI_VAR_PANEL_ROUND, 0.0f );
    gui_window_end();
    style_pop_var( 1 );
}

/*==============================================================================================
    menu_bar_begin / menu_bar_end -- the strip a WIN_MENUBAR window reserved below its title bar.

    The strip rect was carved off the top of the body in window_begin_ex and stashed in
    s_build.win.menubar_rect.  We open a transient pack region over it (outside the body's scrolling
    flow) and restore the body pen on pop, so the body content lays out from its own origin
    regardless of the strip region's parent-pen advance.
==============================================================================================*/

static gui_scroll_link_t s_menubar_sink;     /* scroll / content-measure sink: the strip never scrolls */
static f32          s_menubar_saved_cursor;  /* body pen to restore after the strip region pops        */
static gui_rect_t s_menubar_saved_clip;    /* body hit-test clip to restore after the strip region pops */

bool
gui_menu_bar_begin( void )
{
    if ( !( s_build.win.flags & GUI_WIN_MENUBAR ) )
        return false;

    gui_rect_t bar = s_build.win.menubar_rect;

    /* Strip background, a touch distinct from the body.  Always square: it sits flush against the
       body's top and side edges, where a rounded corner would visibly clip against them. */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );
    draw_face( bar, GUI_ROLE_TITLE, GUI_PHASE_IDLE );
    draw_set_rounding( save_round );

    /* Save the body pen: the strip is drawn outside the body flow, so the body resumes from here. */
    s_menubar_saved_cursor = lf()->pen_y;

    /* The strip sits ABOVE the body region that is currently on the stack, so the live interaction clip (s_scope.clip)
       (the body's, which starts below the strip) excludes it entirely.  layout_push_region with
       own_clip false narrows the new region's hit-test clip to parent_clip & outer; left as-is the
       intersection with the body clip would be empty and every entry would fail rect_hit -- the
       mouse could never hover the bar (only keyboard nav, which skips the clip test, reached it).
       Point the parent clip at the whole window rect for the push so the strip's hit-test clip
       resolves to the strip itself, then restore the body clip in menu_bar_end. */
    s_menubar_saved_clip = s_scope.clip;
    s_scope.clip = ( gui_rect_t ){ s_build.win.x, s_build.win.y, s_build.win.w, s_build.win.h };

    s_menubar_sink = ( gui_scroll_link_t ){ 0 };
    /* Split the band's one-gap slack evenly above and below the WIDGET_H entry row, so the strip's
       labels sit centered -- matching the main menu bar (which centers via a pen lift). */
    layout_push_region( id_combine( s_build.win.id, GUI_MENUBAR_SALT ), bar,
                        ( gui_pad_t ){ WIDGET_PAD, WIDGET_PAD, WIDGET_GAP * 0.5f, WIDGET_GAP * 0.5f },
                        GUI_WIN_NOSCROLL, &s_menubar_sink,
                        /* own_clip */ false );
    gui_bar();            /* the menu labels pack horizontally */
    return true;
}

void
gui_menu_bar_end( void )
{
    if ( !( s_build.win.flags & GUI_WIN_MENUBAR ) )
        return;

    layout_pop_region();
    s_scope.clip = s_menubar_saved_clip;   /* restore the body hit-test clip (pop left it at the window rect) */

    /* Undo the strip pop's body-pen advance: the strip lives outside the body flow, so the body
       resumes exactly where it stood -- pen authoritative (no gap owed), and the strip box must
       not linger as a same_line anchor. */
    layout_pen_jump( lf(), s_menubar_saved_cursor );
    lf()->line.prev_item = ( gui_rect_t ){ 0 };
}

// clang-format on
/*============================================================================================*/
