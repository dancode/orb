/*==============================================================================================

    runtime_service/gui/chrome/popup/gui_toolbar.c -- Icon toolbar strip.

    A UE-style icon toolbar built entirely out of the existing toolkit, the same way combo and
    menu were: bar() lays the row out, the caller's scale_push picks the density, and the
    dropdown variant reuses the popup internals combo already drives.  No new machinery.

        toolbar_begin / toolbar_end -- id-scopes the strip (so two toolbars' buttons never
            collide) and brackets a bar() run.  Emit inside any window / child -- it owns no
            window of its own, matching bar() itself.  Does NOT push a scale -- callers wrap
            toolbar_begin/end in their own scale_push/scale_pop (GUI_SCALE_BAR for a normal
            strip, GUI_SCALE_ROOMY for a large main-panel toolbar, etc.) so a single app can
            mix toolbar sizes side by side.

        toolbar_button   -- a square icon button; press semantics like arrow_button.
        toolbar_toggle    -- the same cell, latched on/off (the active tint marks "on"), for a
            state-holding tool (wireframe, grid snap).
        toolbar_dropdown_begin/end -- a split-style button: the icon plus an adjacent down-arrow
            column marking it as a dropdown.  Opens an arbitrary-widget popup below the button,
            anchored and dismissed exactly like a combo box -- put any widgets in the body,
            including menu_item rows for the icon/label/shortcut three-column layout menus already give you.
        toolbar_separator -- a thin vertical rule between button groups.

            gui()->scale_push( GUI_SCALE_BAR );
            gui()->toolbar_begin( "main" );
                if ( gui()->toolbar_button( "##save", icon_save, "Save (Ctrl+S)" ) ) save();
                gui()->toolbar_toggle( "##wire", icon_wire, &wireframe, "Wireframe" );
                gui()->toolbar_separator();
                if ( gui()->toolbar_dropdown_begin( "##view", icon_eye, "View Mode" ) ) {
                    gui()->menu_item( "Lit",         NULL, NULL );
                    gui()->menu_item( "Unlit",       NULL, NULL );
                    gui()->menu_item( "Wireframe",   NULL, NULL );
                    gui()->toolbar_dropdown_end();
                }
            gui()->toolbar_end();
            gui()->scale_pop();

    Included by gui_chrome.c after gui_menu.c, so the popup internals (popup_open_id, popup_is_open_id,
    popup_set_anchor, popup_begin_common_id, GUI_POPUP_SALT, GUI_POPUP_BASE_FLAGS) are in scope.

==============================================================================================*/
// clang-format off

bool
gui_toolbar_begin( const char* id_str )
{
    gui_push_id( id_str );
    gui_push_layout_state();   // hand the caller's shape back verbatim at toolbar_end
    gui_bar();
    return true;
}

void
gui_toolbar_end( void )
{
    gui_pop_layout_state();   // restores whatever the caller had before toolbar_begin (stack, grid, ...)
    gui_pop_id();
}

/* toolbar_icon_rect -- the icon's draw rect, twice the linear size of the plain WIDGET_PAD inset and
   centered in the cell (clamped to the cell so an oversized icon never spills past the button's
   own background).  Bigger glyph, same cell footprint -- the button itself does not grow. */
static gui_rect_t
toolbar_icon_rect( gui_rect_t r )
{
    // f32 w = ( r.w - 2.0f * WIDGET_PAD ) * 2.0f;
    // f32 h = ( r.h - 2.0f * WIDGET_PAD ) * 2.0f;

    f32 widget_h = WIDGET_H;
    f32 widget_p = WIDGET_PAD;

    f32 w = widget_h - ( widget_p );
    f32 h = widget_h * ( widget_p );

    if ( w > r.w ) w = r.w;
    if ( h > r.h ) h = r.h;
    return ( gui_rect_t ){ r.x + ( r.w - w ) * 0.5f, r.y + ( r.h - h ) * 0.5f, w, h };
}

/*==============================================================================================
    toolbar_button -- a square icon cell, pressed like arrow_button.
==============================================================================================*/

bool
gui_toolbar_button( const char* id_str, gui_icon_id_t icon, const char* tooltip )
{
    gui_id_t   id = item_id( id_str );
    gui_rect_t r  = cell_next_w( WIDGET_H, WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    draw_face_item( r, id, st, false );
    gui_draw_icon_in( toolbar_icon_rect( r ), icon, 0xFFFFFFFFu );

    if ( tooltip && tooltip[ 0 ] )
        gui_set_item_tooltip( tooltip );

    return st.clicked;
}

/*==============================================================================================
    toolbar_toggle -- the same cell, latched on *v.  On draws the active tint (+ a border, so it
    still reads as "on" once the hover/active animation settles back to idle).
==============================================================================================*/

bool
gui_toolbar_toggle( const char* id_str, gui_icon_id_t icon, bool* v, const char* tooltip )
{
    gui_id_t   id = item_id( id_str );
    gui_rect_t r  = cell_next_w( WIDGET_H, WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
    bool               on = ( v && *v );

    /* A toggle that is ON is a CHOSEN item, not a held one: the SELECT plane, so it still lights
       under the cursor.  The flip between planes is the mix's third channel, at its own rate --
       it used to snap, because there was no coordinate between the two planes to travel along. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, col_item_bg_mix( id, st, on ) );
    if ( on )
        draw_push_rect_outline( r.x, r.y, r.w, r.h, WIN_BORDER, 0, COL_BORDER_IDLE );

    gui_draw_icon_in( toolbar_icon_rect( r ), icon, 0xFFFFFFFFu );

    if ( tooltip && tooltip[ 0 ] )
        gui_set_item_tooltip( tooltip );

    bool changed = false;
    if ( st.clicked && v )
    {
        *v      = !( *v );
        changed = true;
        /* Same one-frame-late fix checkbox uses: the tint above drew the OLD state. */
        redraw_request();
    }
    return changed;
}

/*==============================================================================================
    toolbar_dropdown_begin / toolbar_dropdown_end -- a split-style button driving a popup.

    Same open/anchor/dismiss recipe as gui_combo_begin (popup keyed off the button's widget id,
    box-anchored, was-open guard against the same click closing then reopening it), swapping the
    combo's preview-text-plus-arrow face for an icon plus a flush adjacent arrow column.  The body
    is an ordinary stack popup -- any widgets go in it, not just selectable rows.
==============================================================================================*/

typedef struct { u32 open_frame; } gui_toolbar_dd_state_t;

/* Split-button footprint: the square icon cell plus a narrower arrow column beside it, the same
   two-column shape combo's box-plus-arrow uses.  One widget id spans both -- there is only one
   click target -- but the columns draw and read as [icon][down arrow], not a corner overlay. */
#define TB_DD_ARROW_W ( WIDGET_H * 0.45f )

bool
gui_toolbar_dropdown_begin( const char* id_str, gui_icon_id_t icon, const char* tooltip )
{
    gui_id_t   id = item_id( id_str );
    gui_rect_t r  = cell_next_w( WIDGET_H + TB_DD_ARROW_W, WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    gui_id_t                 pid = id_combine( id, GUI_POPUP_SALT );
    gui_toolbar_dd_state_t* ds  = GUI_STATE( gui_toolbar_dd_state_t, id );

    // The was_open guard prevents the same click from closing then
    // immediately reopening the popup.
    bool was_open = ( ds->open_frame + 1u == gui_frame_index() );
    if ( st.clicked && !was_open )
         popup_open_id( pid, r.x, r.y + r.h );
    if ( popup_is_open_id( pid ) )
         popup_set_anchor( pid, r.x, r.y + r.h );

    /* An open dropdown is the chosen entry of the bar, same as a menu bar's open title. */
    bool this_open = popup_is_open_id( pid );
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, col_item_bg_mix( id, st, this_open ) );

    gui_rect_t icon_r  = { r.x, r.y, WIDGET_H, r.h };
    gui_rect_t arrow_r = { r.x + WIDGET_H, r.y, TB_DD_ARROW_W, r.h };
    gui_draw_icon_in( toolbar_icon_rect( icon_r ), icon, 0xFFFFFFFFu );
    draw_dropdown_arrow( arrow_r, COL_TEXT_IDLE );

    if ( tooltip && tooltip[ 0 ] && !this_open )
        gui_set_item_tooltip( tooltip );

    /* Auto-size stack popup, opening below the button -- the arbitrary-widget menu. */
    bool vis = popup_begin_common_id( pid, NULL, GUI_WIN_NOTITLEBAR | GUI_POPUP_BASE_FLAGS,
                                      false, 0.0f, 0.0f );
    if ( vis )
    {
        ds->open_frame = gui_frame_index();   /* body emitted this frame -> "open" next frame */
        gui_stack();                          /* the dropdown body is a vertical list */
    }
    return vis;
}

void
gui_toolbar_dropdown_end( void )
{
    gui_popup_end();
}

/*==============================================================================================
    toolbar_separator -- a thin vertical rule between button groups (the horizontal-bar mirror
    of separator()'s horizontal rule).
==============================================================================================*/

void
gui_toolbar_separator( void )
{
    gui_rect_t r = cell_next_w( WIDGET_PAD * 2.0f + WIN_BORDER, WIDGET_H );
    f32        x = r.x + ( r.w - WIN_BORDER ) * 0.5f;
    draw_push_rect_filled( x, r.y + WIDGET_PAD, WIN_BORDER, r.h - 2.0f * WIDGET_PAD, 0,0,1,1, 0, COL_BORDER_IDLE );
}

// clang-format on
/*============================================================================================*/
