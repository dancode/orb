/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_scrollbar.c -- Scrollbar: track + knob over a scroll offset.

    A stock widget with a special placement policy: it is never emitted from the layout pen.
    The region engine (flow/gui_scroll.c) reserves the gutter, computes the track
    rect from it, and invokes this at layout_pop_region once the frame's content is measured.
    From there it is the standard widget recipe on a handed rect: item_state for the grab
    (ITEM_DRAG), the drag mapped back into *scroll, then the track + knob paint.
    Compose hands the rect and owns the scroll state; this file owns the feel and the look.

    Mouse-only by design: keyboard scrolling is the nav cursor's scroll chase
    (core/gui_item.c), so the bar never lists as a keyboard target (neither Tab nor the
    chrome lane).

    Included by gui_chrome.c in the widgets/ group; the region engine (a LOWER unit) reaches it
    across the unit boundary through the forward
    declaration in flow/gui_flow.h (the same cross-order seam size_animate uses for gui_anim_f32).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Scrollbar ids -- distinct salts so a region's vertical and horizontal bars never share an
    id, nor collide with a label-hashed widget in the same window.  Applied to the region id.
==============================================================================================*/

#define GUI_SCROLLBAR_SALT  0x5C011B01u
#define GUI_HSCROLLBAR_SALT 0x5C011B02u

/* Grab offset within the knob at the moment of press.  Single-slot: only one scrollbar can be
   active (own active_id) at a time, so this covers every bar on every region. */
static f32 s_sb_grab_off = 0.0f;

/*==============================================================================================
    scrollbar_widget -- one scrollbar track + knob along an axis; folds a knob drag into *scroll.

    `vertical` picks the axis; `track` is the full track rect, `content`/`view` the measured and
    visible extents along that axis.  The knob length tracks the visible fraction (min-clamped
    so it stays grabbable) and the drag maps the cursor back into scroll, mirroring
    slider_float.  Shared by every region's bars.
==============================================================================================*/

void
scrollbar_widget( gui_id_t region_id, gui_rect_t track, bool vertical,
                  f32 content, f32 view, f32* scroll )
{
    gui_id_t id          = id_combine( region_id, vertical ? GUI_SCROLLBAR_SALT
                                                             : GUI_HSCROLLBAR_SALT );
    f32        mouse_along = vertical ? s_io.mouse_y : s_io.mouse_x;

    f32 max_scroll = content - view;
    if ( max_scroll < 0.0f ) max_scroll = 0.0f;

    f32 track_len = vertical ? track.h : track.w;
    f32 track_org = vertical ? track.y : track.x;

    /* Knob length is the visible fraction of the track, clamped to a grabbable minimum
       and never longer than the track itself (content <= view => full-length knob).  The
       min-then-cap order matters: a track shorter than the minimum collapses to track_len,
       so it is not folded into one clampf (whose bounds would invert). */
    f32 knob_len = ( content > 0.0f ) ? track_len * ( view / content ) : track_len;
    f32 min_len  = SLIDER_KNOB_W;
    if ( knob_len < min_len )   knob_len = min_len;
    if ( knob_len > track_len ) knob_len = track_len;
    f32 travel = track_len - knob_len;

    /* Derive the current knob position before any interaction this frame -- used for
       press-hit-detection and (after possible update) for drawing. */
    f32 t_cur    = ( max_scroll > 0.0f ) ? *scroll / max_scroll : 0.0f;
    f32 knob_off = track_org + t_cur * travel;

    /* Mouse-only: opt out of nav registration for this item (see the file banner). */
    s_scope.nav.skip = true;
    gui_item_state_t st = item_state( id, track, ITEM_DRAG );

    /* On the press frame, decide whether the cursor landed on the knob (drag from the grabbed
       point) or in the gutter (jump: center the knob under the cursor).  s_sb_grab_off is the
       offset from the knob's leading edge to the cursor and stays fixed for the whole drag. */
    if ( st.pressed )
    {
        if ( mouse_along >= knob_off && mouse_along <= knob_off + knob_len )
            s_sb_grab_off = mouse_along - knob_off;   /* preserve the grab point within handle */
        else
            s_sb_grab_off = knob_len * 0.5f;          /* gutter click: center knob on cursor  */
    }

    /* Drag maps the cursor back into the scroll offset via the grab offset. */
    if ( st.active && travel > 0.0f )
    {
        f32 t = saturate( ( mouse_along - track_org - s_sb_grab_off ) / travel );
        *scroll = t * max_scroll;
        t_cur    = t;
        knob_off = track_org + t_cur * travel;
    }

    /* Track fill draws SQUARE, same reasoning as the title bar (gui_window_end.c): the track
       spans the full gutter and must fully cover anything that scrolled under it (a long
       unwrapped line can paint past the content column into the gutter), and the window's outer
       clip already carries the panel's corner radius where the gutter meets the panel's rounded
       corner.  Rounding the track's own rect here would round BOTH its ends -- including the
       near end at the body seam, away from any window corner -- opening the same coverage gap
       the title bar's own rounding used to.  The grab keeps its radius (a pill knob is a widget
       style choice, not a coverage requirement).  Saved/restored because the scrollbar draws in
       the chrome context. */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );
    draw_face( track, GUI_ROLE_ACCENT, GUI_PHASE_INERT );
    draw_set_rounding( ROUND_WIDGET );
    if ( vertical )
        draw_face_grab( ( gui_rect_t ){ track.x, knob_off, track.w, knob_len }, id, st, 0u, 0.0f );
    else
        draw_face_grab( ( gui_rect_t ){ knob_off, track.y, knob_len, track.h }, id, st, 0u, 0.0f );
    draw_set_rounding( save_round );
}

// clang-format on
/*============================================================================================*/
