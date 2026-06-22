/*==============================================================================================

    runtime_service/imgui/imgui_widget_slider.c -- Slider and drag widgets.

    All widgets in this file share the drag-value interaction pattern: widget_behavior claims
    active_id on mouse press (WIDGET_KIND_DRAG), keeping the drag bound to the widget while
    the cursor sweeps off it.  The displayed value changes live while dragging.

        slider_float / slider_float_step / slider_int  -- a horizontal track with a knob;
            the cursor's fraction along the track maps directly to the value range.
        drag_int  -- no track travel; value changes by cursor displacement from the press
            anchor (s_drag_anchor_v), so there is no range cap and no knob.

    slider_render is the shared visual: track frame, fill bar, knob, and centered value text.

    Included by imgui.c after imgui_widget.c (shares widget_behavior, widget_split_label,
    widget_next_rect, the COL_* palette, and the WIDGET_/WIN_ layout macros from
    imgui_widget_core.c).

==============================================================================================*/
// clang-format off

/* Draw a slider's track, the fill bar up to t (0..1), the knob, and -- unless IMGUI_ITEM_NO_VALUE_TEXT
   is set -- value_text centered on top, fitted to the inner width. */
static void
slider_render( imgui_rect_t track_r, widget_state_t st, f32 t, const char* value_text )
{
    t = saturate( t );

    /* Track frame.  Unlike a button, a slider has a handle on top, so the frame must not take the
       same hover/active colour the knob does (widget_bg_color below) or the highlight would swallow
       the handle.  It lifts to a subtler tint -- distinct from the knob in every state -- so the
       hover still reads as a fill while the knob stays clearly the brighter element. */
    u32 track_col = ( st.hover || st.nav || st.active ) ? COL_INPUT_FOCUS : COL_SLIDER_TRACK;
    draw_push_rect_filled( track_r.x, track_r.y, track_r.w, track_r.h,
                           0,0,1,1, 0, track_col );
    draw_push_rect_outline( track_r.x, track_r.y, track_r.w, track_r.h,
                            WIN_BORDER, 0, COL_BORDER );

    /* Fill bar up to t.  Round only the start (left) corners to match the track frame; keep the
       leading (right) edge facing the knob square, so a rounded leading edge never leaves a gap
       between the fill and the handle.  Per-corner via draw_round_rect_ex (imgui_draw_symbol.c). */
    f32 fill_w = t * ( track_r.w - SLIDER_KNOB_W );
    if ( fill_w > 0.0f )
        draw_round_rect_ex( ( imgui_rect_t ){ track_r.x, track_r.y + 1.0f, fill_w, track_r.h - 2.0f },
                            ROUND_WIDGET, 0.0f, 0.0f, ROUND_WIDGET, true, 0.0f, COL_WIDGET_FG );

    /* Knob (grab): the brighter hover/active element, outlined so its edge stays crisp against the
       track and the fill bar regardless of how close their colours get.  A bar grab by default
       (grab radius -- raise IMGUI_VAR_GRAB_ROUNDING for a pill), or a circular handle when
       IMGUI_VAR_SLIDER_KNOB selects it. */
    f32 knob_x = track_r.x + t * ( track_r.w - SLIDER_KNOB_W );
    if ( style_var( IMGUI_VAR_SLIDER_KNOB ) >= 0.5f )
    {
        f32 kcx = knob_x + SLIDER_KNOB_W * 0.5f;
        f32 kcy = track_r.y + track_r.h * 0.5f;
        f32 kr  = track_r.h * 0.5f;
        draw_circle( kcx, kcy, kr, true,  0.0f,      widget_bg_color( st ) );
        draw_circle( kcx, kcy, kr, false, WIN_BORDER, COL_BORDER );
    }
    else
    {
        f32 save_round = draw_rounding();
        draw_set_rounding( ROUND_GRAB );
        draw_push_rect_filled ( knob_x, track_r.y, SLIDER_KNOB_W, track_r.h,
                                0,0,1,1, 0, widget_bg_color( st ) );
        draw_push_rect_outline( knob_x, track_r.y, SLIDER_KNOB_W, track_r.h,
                                WIN_BORDER, 0, COL_BORDER );
        draw_set_rounding( save_round );
    }

    if ( value_text && !( s_build.cur_item_flags & IMGUI_ITEM_NO_VALUE_TEXT ) )
    {
        f32 inner = track_r.w - 2.0f * WIDGET_PAD;
        f32 tw    = font_text_w_n( value_text, 0xFFFFFFFFu );
        f32 tx    = track_r.x + ( track_r.w - tw ) * 0.5f;
        if ( tx < track_r.x + WIDGET_PAD ) tx = track_r.x + WIDGET_PAD;
        draw_text_fit_n( tx, text_center_y( track_r.y, track_r.h ), COL_TEXT, value_text, 0xFFFFFFFFu, inner );
    }
}

/* slider_float_step -- slider_float quantized to `step` (e.g. 0.25 lands the value on 1/4 marks);
   step <= 0 leaves it continuous, so plain slider_float just forwards with step 0. */
bool
imgui_slider_float_step( const char* label, f32* v, f32 lo, f32 hi, f32 step )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Track takes the left portion; the label sits at the right.  The min track width keeps the
       knob travel usable when the label is long. */
    imgui_rect_t track_r = widget_split_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT );
    widget_state_t st = widget_behavior( id, track_r, WIDGET_KIND_DRAG );

    /* Drag: map the cursor's track fraction to a value, snapping to the step grid when asked. */
    bool changed = false;
    if ( st.active )
    {
        f32 t  = saturate( ( s_io.mouse_x - track_r.x ) / track_r.w );
        f32 nv = lo + t * ( hi - lo );
        if ( step > 0.0f )
            nv = lo + floorf( ( nv - lo ) / step + 0.5f ) * step;   /* nearest step from lo */
        if ( nv < lo ) nv = lo;
        if ( nv > hi ) nv = hi;
        if ( nv != *v )
        {
            *v      = nv;
            changed = true;
        }
    }

    f32  t_cur = ( hi > lo ) ? ( ( *v - lo ) / ( hi - lo ) ) : 0.0f;
    char buf[ 32 ];
    snprintf( buf, sizeof( buf ), "%.3f", *v );
    slider_render( track_r, st, t_cur, buf );
    return changed;
}

bool
imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
{
    return imgui_slider_float_step( label, v, lo, hi, 0.0f );
}

/* slider_int -- the integer slider; every track position lands on a whole value in [lo,hi]. */
bool
imgui_slider_int( const char* label, i32* v, i32 lo, i32 hi )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    imgui_rect_t track_r = widget_split_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT );
    widget_state_t st = widget_behavior( id, track_r, WIDGET_KIND_DRAG );

    bool changed = false;
    if ( st.active )
    {
        f32 t  = saturate( ( s_io.mouse_x - track_r.x ) / track_r.w );
        i32 nv = lo + (i32)floorf( t * (f32)( hi - lo ) + 0.5f );    /* nearest whole step */
        if ( nv < lo ) nv = lo;
        if ( nv > hi ) nv = hi;
        if ( nv != *v )
        {
            *v      = nv;
            changed = true;
        }
    }

    f32  t_cur = ( hi > lo ) ? ( (f32)( *v - lo ) / (f32)( hi - lo ) ) : 0.0f;
    char buf[ 32 ];
    snprintf( buf, sizeof( buf ), "%d", *v );
    slider_render( track_r, st, t_cur, buf );
    return changed;
}

/*----------------------------------------------------------------------------------------------
    drag_int -- a framed integer field whose value is changed by dragging left / right (the Dear
    ImGui DragInt analogue).  No track travel: the value is relative to where the press landed,
    so it has no max -- v_speed units of value per pixel dragged.  v_min < v_max bounds it; both
    equal (e.g. 0,0) leaves it unbounded.  format is the printf form for the displayed value
    ("%d" when NULL/empty; embed a caption like "HP: %d").  Returns true only on the frames the
    drag actually changes the value, so the caller can react to live edits.

    Mouse capture works exactly like slider_float: widget_behavior claims active_id on the press
    (WIDGET_KIND_DRAG), so the drag stays bound to this widget while the cursor sweeps off it and
    no neighbour can steal it.  The press anchor is s_click_x[0] (recorded by the input snapshot)
    paired with the value captured here at press time; re-deriving from that anchor every frame
    keeps the drag exact and drift-free.
----------------------------------------------------------------------------------------------*/

/* Value at the press that started the active drag.  Single-slot: only one widget owns active_id
   at a time, the same reason the resize / repeat scratch state is a lone static. */
static i32 s_drag_anchor_v;

bool
imgui_drag_int( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format )
{
    if ( v_speed <= 0.0f ) v_speed = 1.0f;
    if ( !format || !format[ 0 ] ) format = "%d";

    imgui_id_t   id    = widget_id( label );
    imgui_rect_t r     = widget_next_rect( WIDGET_H );
    imgui_rect_t box_r = widget_split_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT );

    widget_state_t st = widget_behavior( id, box_r, WIDGET_KIND_DRAG );

    /* Capture the value at the grab, then map total cursor displacement since the press into a new
       value each frame (recomputed from the anchor, so rounding never accumulates drift). */
    if ( st.pressed )
        s_drag_anchor_v = *v;

    bool changed = false;
    if ( st.active )
    {
        f32 acc = (f32)s_drag_anchor_v + ( s_io.mouse_x - s_click_x[ 0 ] ) * v_speed;
        i32 nv  = (i32)floorf( acc + 0.5f );                  /* nearest int */
        if ( v_min < v_max ) nv = nv < v_min ? v_min : ( nv > v_max ? v_max : nv );
        if ( nv != *v )
        {
            *v      = nv;
            changed = true;
        }
    }

    /* Frame: a slider-track box that brightens on hover / nav / active so the drag affordance reads. */
    u32 bg = frame_bg_color( st, COL_SLIDER_TRACK );
    draw_push_rect_filled ( box_r.x, box_r.y, box_r.w, box_r.h, 0,0,1,1, 0, bg );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h, WIN_BORDER, 0, COL_BORDER );

    /* Formatted value, centered in the box and fitted (ellipsized) to its inner width. */
    char buf[ 64 ];
    snprintf( buf, sizeof( buf ), format, *v );
    f32 inner = box_r.w - 2.0f * WIDGET_PAD;
    f32 tw    = font_text_w_n( buf, 0xFFFFFFFFu );
    f32 tx    = box_r.x + ( box_r.w - tw ) * 0.5f;
    if ( tx < box_r.x + WIDGET_PAD ) tx = box_r.x + WIDGET_PAD;
    draw_text_fit_n( tx, text_center_y( box_r.y, box_r.h ), COL_TEXT, buf, 0xFFFFFFFFu, inner );

    return changed;
}

/*----------------------------------------------------------------------------------------------
    drag_float -- the floating-point sibling of drag_int: a framed value field changed by a
    left / right drag, v_speed units of value per pixel, with no track and so no travel cap.  The
    Dear ImGui DragFloat analogue.  v_min < v_max bounds the value; both equal leaves it unbounded.
    fmt is the printf form of the displayed value ("%.3f" when NULL).  drag_float2/3/4 lay N equal
    sub-boxes across the control track (a vector edit), each an independent drag.
----------------------------------------------------------------------------------------------*/

/* Float anchor captured at the press that started the active drag -- the float counterpart of
   s_drag_anchor_v, a lone static since only one widget owns active_id at a time. */
static f32 s_drag_anchor_f;

/* One drag-float box in box_r (no label split): the shared interaction + frame draw for a single
   component, so drag_float and the drag_floatN row both reduce to placing boxes and calling this. */
static bool
drag_float_box( imgui_id_t id, imgui_rect_t box_r, f32* v,
                f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    widget_state_t st = widget_behavior( id, box_r, WIDGET_KIND_DRAG );

    /* Capture the value at the grab, then re-derive from the anchor each frame (drift-free). */
    if ( st.pressed )
        s_drag_anchor_f = *v;

    bool changed = false;
    if ( st.active )
    {
        f32 nv = s_drag_anchor_f + ( s_io.mouse_x - s_click_x[ 0 ] ) * v_speed;
        if ( v_min < v_max ) nv = nv < v_min ? v_min : ( nv > v_max ? v_max : nv );
        if ( nv != *v )
        {
            *v      = nv;
            changed = true;
        }
    }

    u32 bg = frame_bg_color( st, COL_SLIDER_TRACK );
    draw_push_rect_filled ( box_r.x, box_r.y, box_r.w, box_r.h, 0,0,1,1, 0, bg );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h, WIN_BORDER, 0, COL_BORDER );

    char buf[ 64 ];
    snprintf( buf, sizeof( buf ), fmt, *v );
    f32 inner = box_r.w - 2.0f * WIDGET_PAD;
    f32 tw    = font_text_w_n( buf, 0xFFFFFFFFu );
    f32 tx    = box_r.x + ( box_r.w - tw ) * 0.5f;
    if ( tx < box_r.x + WIDGET_PAD ) tx = box_r.x + WIDGET_PAD;
    draw_text_fit_n( tx, text_center_y( box_r.y, box_r.h ), COL_TEXT, buf, 0xFFFFFFFFu, inner );

    return changed;
}

bool
imgui_drag_float( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    if ( v_speed <= 0.0f ) v_speed = 1.0f;
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";

    imgui_id_t   id    = widget_id( label );
    imgui_rect_t r     = widget_next_rect( WIDGET_H );
    imgui_rect_t box_r = widget_split_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT );

    return drag_float_box( id, box_r, v, v_speed, v_min, v_max, fmt );
}

/* N-component drag row: N equal drag-float sub-boxes across the control track. */
static bool
drag_float_n( const char* label, f32* v, u32 n, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    if ( v_speed <= 0.0f ) v_speed = 1.0f;
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";

    imgui_id_t   id   = widget_id( label );
    imgui_rect_t r    = widget_next_rect( WIDGET_H );
    imgui_rect_t ctrl = widget_split_label( r, label, font_char_h() * 3.0f * (f32)n, COL_TEXT );

    bool changed = false;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 x0 = ctrl.x + (f32)i        * ctrl.w / (f32)n;
        f32 x1 = ctrl.x + (f32)(i + 1u) * ctrl.w / (f32)n;
        imgui_rect_t sub = { floorf( x0 ), ctrl.y, floorf( x1 ) - floorf( x0 ), ctrl.h };
        if ( drag_float_box( id_combine( id, i + 1u ), sub, &v[ i ], v_speed, v_min, v_max, fmt ) )
            changed = true;
    }
    return changed;
}

bool imgui_drag_float2( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{ return drag_float_n( label, v, 2u, v_speed, v_min, v_max, fmt ); }

bool imgui_drag_float3( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{ return drag_float_n( label, v, 3u, v_speed, v_min, v_max, fmt ); }

bool imgui_drag_float4( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{ return drag_float_n( label, v, 4u, v_speed, v_min, v_max, fmt ); }

// clang-format on
/*============================================================================================*/
