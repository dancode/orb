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

/* Strength of the gutter's dim end, as a fraction of the THEME's own shadow alpha (GUI_EXT_SHADOW,
   the same colour window_draw_elevation casts) -- not an independent colour.  A theme's shadow
   hue or strength change reaches the gutter for free this way, the same reasoning WIN_SHADOW_
   FLOAT_ALPHA in gui_window_free.c already rides for the elevation shadow's window-band cut.
   Two levels, damped between them: REST (0.15) is the quiet default so an unhovered gutter reads
   as a soft hint next to the panel, HOVER (0.35) deepens it while the cursor sits anywhere in the
   track -- a legible "you are over the gutter" cue that costs no extra draw call, just a stronger
   mix of the same two colours already blended for the fill.  A first pass hover-REVEALED the
   whole gutter this way (0 alpha to full) and that read fine on a wide VS-sized window where the
   cursor rarely crosses the gutter, but a small floating window's gutter sits right where the
   cursor travels, so it kept popping in and out on every pass -- distracting in a way none of
   this file's other widgets are, because they react to being TARGETED, not merely overflown.
   Darkening an already-opaque, always-present fill sidesteps that: the gutter never disappears or
   reappears, it only deepens, so there is no coverage flicker to chase. */
#define SCROLLBAR_GUTTER_SHADOW_REST   0.15f
#define SCROLLBAR_GUTTER_SHADOW_HOVER  0.35f
#define SCROLLBAR_GUTTER_SHADOW_RATE   15.0f   /* Hz-like damper speed, gui_anim_f32 */

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
                  f32 content, f32 view, f32* scroll, bool nested, u8 backdrop_phase )
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
       so it is not folded into one clampf (whose bounds would invert).  Floored at the thumb's
       own thickness (SCROLLBAR_KNOB_W), not the slider's knob width -- a scrollbar is its own
       widget now, not a slider wearing a track. */
    f32 knob_len = ( content > 0.0f ) ? track_len * ( view / content ) : track_len;
    f32 min_len  = SCROLLBAR_KNOB_W;
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

    /* Soft gutter: a gradient from the backdrop washed toward the THEME's own shadow colour
       (GUI_EXT_SHADOW, at some fraction of its authored alpha) at the content-facing edge, back
       to the plain backdrop colour at the outer edge -- the shadow IS the gutter fill now, not a
       separate layer over a flat one, and it inherits whatever hue or strength a theme gives its
       shadow instead of carrying its own fixed tint.  Both ends otherwise read off the same
       backdrop -- the window body's PANEL cell, or a nested child's PANEL_CHILD ground when
       `nested` (the region engine hands us its own pushed_clip, the same signal that already
       tells it whether it owns a clip of its own) -- at `backdrop_phase`, the SAME phase that
       backdrop's own fill painted this frame (layout_frame_t.backdrop_phase, flow/gui_flow.h), so
       a focused window's lift matches exactly instead of guessing IDLE.  The fraction itself
       damps between SCROLLBAR_GUTTER_SHADOW_REST and _HOVER off st.hover (see that constant's
       comment for why the fill deepens rather than reveals) -- BOTH gradient stops MUST stay
       fully opaque regardless of that weight: this track spans the full gutter over content that
       can scroll under it (a long unwrapped line can paint past the content column into the
       gutter), so any transparent stop would let it show through instead of being clipped by the
       fill, the same coverage rule the square (non-rounded) track shape below exists for --
       col_lerp keeps both stops opaque here since backdrop_col and theme_shadow are each already
       opaque going in, regardless of the mix weight.  draw_gradient's `horizontal` flag rides
       `vertical` directly: a vertical bar's shadow ramps across its WIDTH (the x axis, i.e.
       horizontal=true), a horizontal bar's ramps across its HEIGHT (the y axis) -- track.x/y is
       already the content-facing edge either way (gui_scroll.c seats the gutter flush past the
       view), so col_a (the dim end) belongs at the box origin and col_b (backdrop) at the far end
       regardless of axis. */
    f32 shadow_strength = gui_anim_f32( id_combine( id, 1u ),
        st.hover ? SCROLLBAR_GUTTER_SHADOW_HOVER : SCROLLBAR_GUTTER_SHADOW_REST, SCROLLBAR_GUTTER_SHADOW_RATE );

    u32 backdrop_col = style_col( nested ? GUI_ROLE_PANEL_CHILD : GUI_ROLE_PANEL, backdrop_phase );
    u32 theme_shadow = style_ext( GUI_EXT_SHADOW ) | 0xFF000000u;
    f32 shadow_amt   = ( (f32)( style_ext( GUI_EXT_SHADOW ) >> 24 ) / 255.0f ) * shadow_strength;
    u32 dim_col      = col_lerp( backdrop_col, theme_shadow, shadow_amt );
    draw_gradient( track, dim_col, backdrop_col, vertical );

    /* The thumb's cross-axis thickness is its own var, independent of the gutter it sits in
       (SCROLLBAR_KNOB_W vs SLIDER_KNOB_W/GUI_VAR_GUTTER), and centered in the gutter's thickness
       -- a thin pill riding a wider hit region, rather than a knob that fills the whole track. */
    f32 track_th = vertical ? track.w : track.h;
    f32 knob_th  = SCROLLBAR_KNOB_W;
    if ( knob_th > track_th ) knob_th = track_th;
    f32 knob_cross = ( vertical ? track.x : track.y ) + ( track_th - knob_th ) * 0.5f;

    /* The thumb is the ONE reactive part of the widget now: the gutter fill above is flat/shadow
       and never changes, so hovering anywhere in the gutter (the whole track is the hit region,
       item_state above) has to read on the thumb instead, or the control gives no feedback at
       all.  draw_face_grab reads its own mix off (id, st) -- the same probe the drag/press logic
       already computed -- so the thumb lifts through the ordinary GUI_ROLE_GRAB HOT/ACTIVE cells,
       just like a slider knob does. */
    draw_set_rounding( ROUND_WIDGET );
    if ( vertical )
        draw_face_grab( ( gui_rect_t ){ knob_cross, knob_off, knob_th, knob_len }, id, st, 0u, 0.0f );
    else
        draw_face_grab( ( gui_rect_t ){ knob_off, knob_cross, knob_len, knob_th }, id, st, 0u, 0.0f );
    draw_set_rounding( save_round );
}

// clang-format on
/*============================================================================================*/
