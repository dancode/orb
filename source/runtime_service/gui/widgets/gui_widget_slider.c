/*==============================================================================================

    runtime_service/gui/widgets/gui_widget_slider.c -- Slider, drag, and color-edit widgets.

    Every value-editing control here shares the drag-value interaction pattern: item_state
    claims active_id on mouse press (ITEM_DRAG), keeping the drag bound to the widget while
    the cursor sweeps off it.  The displayed value changes live while dragging.

        slider_float / slider_float_step / slider_int  -- a horizontal track with a knob;
            the cursor's fraction along the track maps directly to the value range.
        drag_int / drag_float / drag_float2/3/4  -- no track travel; value changes by cursor
            displacement from the press anchor (s_drag_anchor_v / s_drag_anchor_f), so there is
            no range cap and no knob.  The N-component variants lay equal sub-boxes across the row.
        color_edit3 / color_edit4  -- an inline [swatch][R/G/B(/A) drag fields] row (RGB or HSV
            per the color-edit flags) plus a click-to-open picker popup; built from the drag-float
            boxes above, with color_hsv_to_rgb / color_rgb_to_hsv converting the working copy.

    slider_render is the shared visual: track frame, fill bar, knob, and centered value text.

    Included by gui.c after the widget family files (shares item_state, draw_field_label,
    cell_next, the COL_* palette, and the WIDGET_/WIN_ layout macros from
    gui_paint_core.c).

==============================================================================================*/
// clang-format off

/* The printf format slider_float / slider_float_step render their value with.  Named so the
   keyboard-step floor below and the snprintf that draws the value stay pinned to the same
   precision -- raise the decimals here and the arrow step widens to match automatically. */
#define SLIDER_FLOAT_FMT "%.3f"

/* Smallest step fmt's own decimal precision can actually show, e.g. "%.1f" -> 0.1, "%.3f" -> 0.001.
   0.0f (no floor) when fmt has no explicit ".N" precision -- printf's default (6 places) is fine
   enough that nothing needs flooring.  Used to keep drag_float_box's keyboard step natural: raise
   v_speed only up to the display's own resolution, never past it. */
static f32
fmt_decimal_step( const char* fmt )
{
    const char* dot = fmt ? strchr( fmt, '.' ) : NULL;
    if ( !dot ) return 0.0f;

    i32 digits = 0;
    for ( const char* p = dot + 1; *p >= '0' && *p <= '9'; ++p ) ++digits;
    if ( digits <= 0 ) return 0.0f;

    f32 step = 1.0f;
    for ( i32 i = 0; i < digits; ++i ) step *= 0.1f;
    return step;
}

/*----------------------------------------------------------------------------------------------
    Keyboard arrow-step apply -- the shared tail every value widget (slider / drag, float / int)
    runs once its captured nav_adjust (-1 / +1 this frame) is known.  The per-widget code derives
    the BASE step magnitude (its own semantics: a slider's 1%-of-range, a drag's v_speed) and hands
    it here; the helper owns the parts that must be identical everywhere -- the "never nudge by less
    than the display can show" floor, the bounded clamp, and the write-and-report gate -- so a fix
    to any of those lands on all of them at once.  Returns true and writes *v only on a real change.
    Bounds clamp only when lo < hi (a drag passing v_min == v_max stays unbounded; a slider always
    has lo < hi).
----------------------------------------------------------------------------------------------*/

/* Float: floor the step at fmt's decimal resolution so a small-range slider / slow drag still moves
   the printed value every press (see fmt_decimal_step). */
static bool
value_step_f32( f32* v, i32 nav_adjust, f32 step, const char* fmt, f32 lo, f32 hi )
{
    f32 min_step = fmt_decimal_step( fmt );
    if ( min_step > step ) step = min_step;

    f32 nv = *v + (f32)nav_adjust * step;
    if ( lo < hi ) nv = nv < lo ? lo : ( nv > hi ? hi : nv );
    if ( nv == *v ) return false;
    *v = nv;
    return true;
}

/* Integer: floor the step at one whole unit so a sub-unit v_speed still advances the value. */
static bool
value_step_i32( i32* v, i32 nav_adjust, i32 step, i32 lo, i32 hi )
{
    if ( step < 1 ) step = 1;

    i32 nv = *v + nav_adjust * step;
    if ( lo < hi ) nv = nv < lo ? lo : ( nv > hi ? hi : nv );
    if ( nv == *v ) return false;
    *v = nv;
    return true;
}

/* Draw a slider's track, the fill bar up to t (0..1), the knob, and -- unless GUI_ITEM_NO_VALUE_TEXT
   is set -- value_text centered on top, fitted to the inner width. */
static void
slider_render( gui_rect_t track_r, gui_item_state_t st, f32 t, const char* value_text )
{
    t = saturate( t );

    /* Track frame.  Unlike a button, a slider has a handle on top, so the frame must not take the
       same hover/active colour the knob does (col_item_bg below) or the highlight would swallow
       the handle.  It lifts to a subtler tint -- distinct from the knob in every state -- so the
       hover still reads as a fill while the knob stays clearly the brighter element. */
    u32 track_col = ( st.hover || st.nav || st.active ) ? COL_INPUT_FOCUS : COL_SLIDER_TRACK;
    draw_fill( track_r, track_col );
    /* Captured for keyboard value edit (st.focused -- see nav_item_register) gets the same border
       lift text/numeric fields use on focus, so going from nav highlight to Left/Right-adjust reads
       as a real state change instead of an invisible one. */
    draw_outline( track_r, WIN_BORDER, st.focused ? COL_WIDGET_HOT : COL_BORDER );

    /* Fill bar up to t.  Round only the start (left) corners to match the track frame; keep the
       leading (right) edge facing the knob square, so a rounded leading edge never leaves a gap
       between the fill and the handle.  Per-corner via draw_round_rect_ex (gui_symbol.c). */
    f32 fill_w = t * ( track_r.w - SLIDER_KNOB_W );
    if ( fill_w > 0.0f )
        draw_round_rect_ex( ( gui_rect_t ){ track_r.x, track_r.y + 1.0f, fill_w, track_r.h - 2.0f },
                            ROUND_WIDGET, 0.0f, 0.0f, ROUND_WIDGET, true, 0.0f, COL_WIDGET_FG );

    /* Knob (grab): the brighter hover/active element, outlined so its edge stays crisp against the
       track and the fill bar regardless of how close their colours get.  A bar grab by default
       (grab radius -- raise GUI_VAR_GRAB_ROUNDING for a pill), or a circular handle when
       GUI_VAR_SLIDER_KNOB selects it. */
    f32 knob_x = track_r.x + t * ( track_r.w - SLIDER_KNOB_W );
    if ( style_var( GUI_VAR_SLIDER_KNOB ) >= 0.5f )
    {
        f32 kcx = knob_x + SLIDER_KNOB_W * 0.5f;
        f32 kcy = track_r.y + track_r.h * 0.5f;
        f32 kr  = track_r.h * 0.5f;
        draw_circle( kcx, kcy, kr, true,  0.0f,      col_item_bg( st ) );
        draw_circle( kcx, kcy, kr, false, WIN_BORDER, COL_BORDER );
    }
    else
    {
        f32 save_round = draw_rounding();
        draw_set_rounding( ROUND_GRAB );
        gui_rect_t knob_r = { knob_x, track_r.y, SLIDER_KNOB_W, track_r.h };
        draw_fill   ( knob_r, col_item_bg( st ) );
        draw_outline( knob_r, WIN_BORDER, COL_BORDER );
        draw_set_rounding( save_round );
    }

    if ( value_text && !( s_scope.flags & GUI_ITEM_NO_VALUE_TEXT ) )
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
gui_slider_float_step( const char* label, f32* v, f32 lo, f32 hi, f32 step )
{
    gui_id_t   id = item_id( label );
    gui_rect_t r  = cell_next( WIDGET_H );

    /* Track takes the left portion; the label sits at the right.  The min track width keeps the
       knob travel usable when the label is long. */
    gui_rect_t track_r = draw_field_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT_DIM );
    gui_item_state_t st = item_state( id, track_r, ITEM_DRAG );

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

    /* Keyboard value edit (activation captured this slider -- st.nav_adjust): each Left/Right
       repeat steps the quantize step when set, else 1% of the range.  value_step_f32 floors that at
       SLIDER_FLOAT_FMT's resolution and clamps, so a small-range slider still moves the printed
       value every press. */
    if ( st.nav_adjust != 0 )
    {
        f32 base = ( step > 0.0f ) ? step : ( hi - lo ) * 0.01f;
        if ( value_step_f32( v, st.nav_adjust, base, SLIDER_FLOAT_FMT, lo, hi ) )
            changed = true;
    }

    /* Paint gate: a scrolled-out slider skips its whole render prep (the value snprintf is real
       per-row cost in a long list) -- state and value edits above already ran, so behavior is
       untouched; every push in slider_render would have been culled individually anyway. */
    if ( !draw_cull_box( track_r.x, track_r.y, track_r.w, track_r.h ) )
    {
        f32  t_cur = ( hi > lo ) ? ( ( *v - lo ) / ( hi - lo ) ) : 0.0f;
        char buf[ 32 ];
        fmt_snprintf( buf, sizeof( buf ), SLIDER_FLOAT_FMT, *v );
        slider_render( track_r, st, t_cur, buf );
    }
    return changed;
}

bool
gui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
{
    return gui_slider_float_step( label, v, lo, hi, 0.0f );
}

/* slider_int -- the integer slider; every track position lands on a whole value in [lo,hi]. */
bool
gui_slider_int( const char* label, i32* v, i32 lo, i32 hi )
{
    gui_id_t   id = item_id( label );
    gui_rect_t r  = cell_next( WIDGET_H );

    gui_rect_t track_r = draw_field_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT_DIM );
    gui_item_state_t st = item_state( id, track_r, ITEM_DRAG );

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

    /* Keyboard value edit: one whole step per Left/Right repeat. */
    if ( st.nav_adjust != 0 && value_step_i32( v, st.nav_adjust, 1, lo, hi ) )
        changed = true;

    /* Paint gate -- see slider_float_step. */
    if ( !draw_cull_box( track_r.x, track_r.y, track_r.w, track_r.h ) )
    {
        f32  t_cur = ( hi > lo ) ? ( (f32)( *v - lo ) / (f32)( hi - lo ) ) : 0.0f;
        char buf[ 32 ];
        fmt_snprintf( buf, sizeof( buf ), "%d", *v );
        slider_render( track_r, st, t_cur, buf );
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
    drag_int -- a framed integer field whose value is changed by dragging left / right (the Dear
    ImGui DragInt analogue).  No track travel: the value is relative to where the press landed,
    so it has no max -- v_speed units of value per pixel dragged.  v_min < v_max bounds it; both
    equal (e.g. 0,0) leaves it unbounded.  format is the printf form for the displayed value
    ("%d" when NULL/empty; embed a caption like "HP: %d").  Returns true only on the frames the
    drag actually changes the value, so the caller can react to live edits.

    Mouse capture works exactly like slider_float: item_state claims active_id on the press
    (ITEM_DRAG), so the drag stays bound to this widget while the cursor sweeps off it and
    no neighbour can steal it.  The value is re-derived every frame from an anchor (value +
    mouse x) captured at the press, which keeps the drag exact and drift-free; the anchor is re-set
    mid-drag whenever the rate modifier changes so the value never jumps.

    Modifiers (Dear ImGui parity, drag_speed_scale / drag_text_enter below):
        Alt   + drag  -- finer (v_speed x 0.1)
        Shift + drag  -- faster (v_speed x 10)
        Ctrl  + click -- switch the box to a text-entry field (num_edit_field) instead of dragging

    Range-relative speed: v_speed is units of value per pixel dragged, but a fixed per-pixel speed
    feels wildly different across ranges -- a 0.5/px drag crawls over a 0..1000 field and instantly
    clamps a 50..68 one.  So the API treats v_speed <= 0 as "auto" and derives it from the value
    range (drag_resolve_speed), making every bounded drag take the same wrist travel end to end no
    matter how wide its range.  A caller that needs an exact per-pixel feel still passes an explicit
    v_speed > 0.  This is the one consistent range->feel rule for every drag widget.
----------------------------------------------------------------------------------------------*/

/* Drag rate modifiers (Dear ImGui parity): Alt drags finer, Shift drags faster.  Returned as a
   multiplier on the resolved v_speed; neither held leaves it at 1.  Alt wins if both are down.
   Named so the feel is tunable in one place, and shared by drag_int_box and drag_float_box alike. */
#define DRAG_SPEED_FINE 0.1f
#define DRAG_SPEED_FAST 10.0f

static f32
drag_speed_scale( void )
{
    if ( io_alt() )   return DRAG_SPEED_FINE;
    if ( io_shift() ) return DRAG_SPEED_FAST;
    return 1.0f;
}

/* Pixels of horizontal drag that sweep a bounded value across its whole range -- the single global
   knob tying "range" to "drag feel".  Bigger = more deliberate; the Shift / Alt modifiers scale
   around it.  (A #define rather than a style var by design: the range->speed relationship is a
   behavior contract every drag shares, not a per-theme metric.) */
#define DRAG_RANGE_SPAN_PX 200.0f

/* Resolve the per-pixel drag speed.  A caller-supplied v_speed > 0 is honored verbatim.  v_speed
   <= 0 is the "auto" signal: a bounded value (range > 0) gets range / DRAG_RANGE_SPAN_PX, so its
   whole span takes the same drag distance as any other; an unbounded one falls back to a fixed
   per-pixel default (no range to divide).  Both drag_int and drag_float route through here, so the
   rule is identical for every drag widget. */
static f32
drag_resolve_speed( f32 v_speed, f32 range, f32 unbounded_default )
{
    if ( v_speed > 0.0f ) return v_speed;
    if ( range   > 0.0f ) return range / DRAG_RANGE_SPAN_PX;
    return unbounded_default;
}

/* Value at the press that started the active drag, plus the mouse x and rate scale it was anchored
   at.  Single-slot: only one widget owns active_id at a time, the same reason the resize / repeat
   scratch state is a lone static.  s_drag_anchor_x / s_drag_scale are shared by the int and float
   boxes (one drag at a time) and re-set mid-drag whenever the rate modifier changes, so the value
   continues smoothly from where it is instead of rescaling the whole press-to-now offset. */
static i32 s_drag_anchor_v;
static f32 s_drag_anchor_x;
static f32 s_drag_scale = 1.0f;

/* Draw the framed drag value text, centered and clipped to the box.  Shared by the int and float
   boxes and by their one-frame text-entry blur.  Floor-bias the center: the odd remainder pixel
   goes consistently to the right margin, so the glyph run lands on a whole pixel (no sub-pixel
   shimmer) and steps monotonically on resize rather than the two margins alternately absorbing it. */
static void
drag_value_text( gui_rect_t box_r, const char* buf )
{
    f32 tw = font_text_w_n( buf, 0xFFFFFFFFu );
    f32 tx = floorf( box_r.x + ( box_r.w - tw ) * 0.5f );
    if ( tx < box_r.x + WIDGET_PAD ) tx = box_r.x + WIDGET_PAD;
    draw_push_text_clip_n( tx, text_center_y( box_r.y, box_r.h ), COL_TEXT, buf,
                           0xFFFFFFFFu, box_r.x, box_r.x + box_r.w - WIDGET_PAD );
}

/* Enter text-entry mode when the box is Ctrl+Clicked (ImGui parity): focus it for keyboard input
   instead of starting a drag, reflecting the focus locally this frame so it seeds + edits at once
   (a FOCUSABLE field gets focus the same frame as its focus-gaining click).  st is passed by
   pointer so the caller sees the focus this frame. */
static void
drag_text_enter( gui_id_t id, gui_item_state_t* st )
{
    if ( st->pressed && io_ctrl() )
    {
        s_interaction.focused_id = id;
        st->focused              = true;
    }
}

/* Draw the input-style frame a drag box wears in text-entry mode -- distinct from the slider-track
   frame so the mode switch reads at a glance, and identical to the numeric input field. */
static void
drag_text_frame( gui_rect_t box_r, gui_item_state_t st )
{
    draw_fill( box_r, st.focused ? COL_INPUT_FOCUS : col_frame_bg( st, COL_INPUT_BG ) );
    draw_outline( box_r, WIN_BORDER, st.focused ? COL_WIDGET_HOT : COL_BORDER );
}

static bool
drag_int_box( gui_id_t id, gui_rect_t box_r, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format )
{
    gui_item_state_t st = item_state( id, box_r, ITEM_DRAG );
    drag_text_enter( id, &st );

    bool changed   = false;
    bool text_mode = st.focused || num_edit_active( id );

    if ( text_mode )
    {
        /* Text entry: a plain "%d" seeds the editor even when `format` carries a caption. */
        drag_text_frame( box_r, st );
        double out;
        if ( num_edit_field( id, box_r, st, "%d", true, (double)*v, &out ) )
        {
            *v      = (i32)out;
            changed = true;
        }
    }
    else
    {
        if ( st.pressed )
        {
            s_drag_anchor_v = *v;
            s_drag_anchor_x = s_io.mouse_x;
            s_drag_scale    = drag_speed_scale();
        }
        if ( st.active )
        {
            f32 scale = drag_speed_scale();
            if ( scale != s_drag_scale )     /* modifier changed mid-drag: re-anchor, no jump */
            {
                s_drag_anchor_v = *v;
                s_drag_anchor_x = s_io.mouse_x;
                s_drag_scale    = scale;
            }
            f32 acc = (f32)s_drag_anchor_v + ( s_io.mouse_x - s_drag_anchor_x ) * v_speed * scale;
            i32 nv  = (i32)floorf( acc + 0.5f );
            if ( v_min < v_max ) nv = nv < v_min ? v_min : ( nv > v_max ? v_max : nv );
            if ( nv != *v )
            {
                *v      = nv;
                changed = true;
            }
        }

        /* Keyboard value edit: v_speed rounded to whole units per Left/Right repeat.  value_step_i32
           floors it at one unit, so a v_speed below 1 still advances rather than rounding to no
           change. */
        if ( st.nav_adjust != 0
             && value_step_i32( v, st.nav_adjust, (i32)( v_speed + 0.5f ), v_min, v_max ) )
            changed = true;

        u32 bg = col_frame_bg( st, COL_SLIDER_TRACK );
        draw_fill( box_r, bg );
        draw_outline( box_r, WIN_BORDER, st.focused ? COL_WIDGET_HOT : COL_BORDER );
    }

    /* Value text -- unless the focused editor already painted its own (and caret), or the box is
       scrolled out (paint gate -- see slider_float_step; skips the snprintf + measure). */
    if ( !st.focused && !draw_cull_box( box_r.x, box_r.y, box_r.w, box_r.h ) )
    {
        char buf[ 64 ];
        fmt_snprintf( buf, sizeof( buf ), format, *v );
        drag_value_text( box_r, buf );
    }

    return changed;
}

bool
gui_drag_int( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format )
{
    /* Auto (v_speed <= 0): span the [v_min,v_max] range over DRAG_RANGE_SPAN_PX; unbounded -> 1/px. */
    v_speed = drag_resolve_speed( v_speed, (f32)( v_max - v_min ), 1.0f );
    if ( !format || !format[ 0 ] ) format = "%d";

    gui_id_t   id    = item_id( label );
    gui_rect_t r     = cell_next( WIDGET_H );
    gui_rect_t box_r = draw_field_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT_DIM );

    return drag_int_box( id, box_r, v, v_speed, v_min, v_max, format );
}

/*----------------------------------------------------------------------------------------------
    drag_float -- the floating-point sibling of drag_int: a framed value field changed by a
    left / right drag, v_speed units of value per pixel, with no track and so no travel cap.  The
    Dear ImGui DragFloat analogue.  v_min < v_max bounds the value; both equal leaves it unbounded.
    fmt is the printf form of the displayed value ("%.3f" when NULL).  drag_float2/3/4 lay N equal
    sub-boxes across the control track (a vector edit), each an independent drag.
----------------------------------------------------------------------------------------------*/

/* Float anchor captured at the press that started the active drag -- the float counterpart of
   s_drag_anchor_v (s_drag_anchor_x / s_drag_scale are shared), a lone static since only one widget
   owns active_id at a time. */
static f32 s_drag_anchor_f;

/* One drag-float box in box_r (no label split): the shared interaction + frame draw for a single
   component, so drag_float and the drag_floatN row both reduce to placing boxes and calling this.
   Modifiers and Ctrl+Click text entry match drag_int_box (see the section header). */
static bool
drag_float_box( gui_id_t id, gui_rect_t box_r, f32* v,
                f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    gui_item_state_t st = item_state( id, box_r, ITEM_DRAG );
    drag_text_enter( id, &st );

    bool changed   = false;
    bool text_mode = st.focused || num_edit_active( id );

    if ( text_mode )
    {
        /* Text entry: seed with a decoration-free "%g" (fmt may carry a caption like "X: %.2f"). */
        drag_text_frame( box_r, st );
        double out;
        if ( num_edit_field( id, box_r, st, "%g", false, (double)*v, &out ) )
        {
            *v      = (f32)out;
            changed = true;
        }
    }
    else
    {
        /* Capture value + mouse x + rate at the grab, then re-derive from the anchor each frame
           (drift-free), re-anchoring when the rate modifier changes so the value never jumps. */
        if ( st.pressed )
        {
            s_drag_anchor_f = *v;
            s_drag_anchor_x = s_io.mouse_x;
            s_drag_scale    = drag_speed_scale();
        }
        if ( st.active )
        {
            f32 scale = drag_speed_scale();
            if ( scale != s_drag_scale )
            {
                s_drag_anchor_f = *v;
                s_drag_anchor_x = s_io.mouse_x;
                s_drag_scale    = scale;
            }
            f32 nv = s_drag_anchor_f + ( s_io.mouse_x - s_drag_anchor_x ) * v_speed * scale;
            if ( v_min < v_max ) nv = nv < v_min ? v_min : ( nv > v_max ? v_max : nv );
            if ( nv != *v )
            {
                *v      = nv;
                changed = true;
            }
        }

        /* Keyboard value edit: a raw per-pixel v_speed suits a multi-pixel mouse drag, but one key
           repeat moving *v by less than fmt's decimal places can show makes every other press look
           like nothing happened (e.g. v_speed 0.05 against "%.1f": 0.00->0.05 reads as "0.1", but
           0.05->0.10 reads as the SAME "0.1" -- the printed text only ticks over every other step).
           value_step_f32 floors the step at fmt's own resolution -- never lower, never past what
           v_speed already clears -- so the nudge stays the natural per-press increment everywhere
           it can and only widens exactly enough to clear the display where it can't. */
        if ( st.nav_adjust != 0 && value_step_f32( v, st.nav_adjust, v_speed, fmt, v_min, v_max ) )
            changed = true;

        u32 bg = col_frame_bg( st, COL_SLIDER_TRACK );
        draw_fill( box_r, bg );
        draw_outline( box_r, WIN_BORDER, st.focused ? COL_WIDGET_HOT : COL_BORDER );
    }

    /* Value text -- unless the focused editor already painted its own (and caret), or the box is
       scrolled out (paint gate -- see slider_float_step; skips the snprintf + measure). */
    if ( !st.focused && !draw_cull_box( box_r.x, box_r.y, box_r.w, box_r.h ) )
    {
        char buf[ 64 ];
        fmt_snprintf( buf, sizeof( buf ), fmt, *v );
        drag_value_text( box_r, buf );
    }

    return changed;
}

bool
gui_drag_float( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    /* Auto (v_speed <= 0): span the [v_min,v_max] range over DRAG_RANGE_SPAN_PX; unbounded -> 1/px. */
    v_speed = drag_resolve_speed( v_speed, v_max - v_min, 1.0f );
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";

    gui_id_t   id    = item_id( label );
    gui_rect_t r     = cell_next( WIDGET_H );
    gui_rect_t box_r = draw_field_label( r, label, SLIDER_KNOB_W * 3.0f, COL_TEXT_DIM );

    return drag_float_box( id, box_r, v, v_speed, v_min, v_max, fmt );
}

/* N-component drag row: N equal drag-float sub-boxes across the control track. */
static bool
drag_float_n( const char* label, f32* v, u32 n, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    /* Auto (v_speed <= 0): span the shared [v_min,v_max] range over DRAG_RANGE_SPAN_PX. */
    v_speed = drag_resolve_speed( v_speed, v_max - v_min, 1.0f );
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";

    gui_id_t   id   = item_id( label );
    gui_rect_t r    = cell_next( WIDGET_H );
    gui_rect_t ctrl = draw_field_label( r, label, font_char_h() * 3.0f * (f32)n, COL_TEXT_DIM );

    bool changed = false;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 x0 = ctrl.x + (f32)i        * ctrl.w / (f32)n;
        f32 x1 = ctrl.x + (f32)(i + 1u) * ctrl.w / (f32)n;
        gui_rect_t sub = { floorf( x0 ), ctrl.y, floorf( x1 ) - floorf( x0 ), ctrl.h };
        if ( drag_float_box( id_combine( id, i + 1u ), sub, &v[ i ], v_speed, v_min, v_max, fmt ) )
            changed = true;
    }
    return changed;
}

bool gui_drag_float2( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{ return drag_float_n( label, v, 2u, v_speed, v_min, v_max, fmt ); }

bool gui_drag_float3( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{ return drag_float_n( label, v, 3u, v_speed, v_min, v_max, fmt ); }

bool gui_drag_float4( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{ return drag_float_n( label, v, 4u, v_speed, v_min, v_max, fmt ); }

/* HSV -> RGB, h/s/v and r/g/b all in [0,1] (h wraps).  The standard six-sector conversion:
   which sector h falls in selects which of r/g/b holds the max/min/ramp role. */
static void
color_hsv_to_rgb( f32 h, f32 s, f32 v, f32* r, f32* g, f32* b )
{
    if ( s == 0.0f )
    {
        *r = *g = *b = v;
        return;
    }
    h = fmodf( h, 1.0f );
    if ( h < 0.0f ) h += 1.0f;
    h *= 6.0f;
    int i = (int)floorf( h );
    f32 f = h - (f32)i;
    f32 p = v * ( 1.0f - s );
    f32 q = v * ( 1.0f - s * f );
    f32 t = v * ( 1.0f - s * ( 1.0f - f ) );
    switch ( i )
    {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        case 5: *r = v; *g = p; *b = q; break;
        default: *r = v; *g = v; *b = v; break;
    }
}

/* RGB -> HSV, the inverse of color_hsv_to_rgb.  K tracks which channel permutation was applied
   (identity / rg-swap / rgb-rotate) so h can be recovered from the sorted channels; the 1e-20f
   terms guard the two divisions against a zero chroma / zero r (grayscale input). */
static void
color_rgb_to_hsv( f32 r, f32 g, f32 b, f32* h, f32* s, f32* v )
{
    f32 K = 0.0f;
    if ( g < b )
    {
        f32 tmp = g; g = b; b = tmp;
        K = -1.0f;
    }
    if ( r < g )
    {
        f32 tmp = r; r = g; g = tmp;
        K = -2.0f / 6.0f - K;
    }
    f32 chroma = r - ( g < b ? g : b );
    *h = fabsf( K + ( g - b ) / ( 6.0f * chroma + 1e-20f ) );
    *s = chroma / ( r + 1e-20f );
    *v = r;
}

static bool
color_edit_n( const char* label, f32* v, u32 n, gui_color_edit_flags_t flags )
{
    gui_id_t id = item_id( label );
    gui_rect_t r = cell_next( WIDGET_H );

    u32  comps  = ( n == 4 && ( flags & GUI_COLOR_EDIT_NO_ALPHA ) ) ? 3 : n;
    bool is_hsv = ( flags & GUI_COLOR_EDIT_DISPLAY_HSV ) != 0;
    bool is_flt = ( flags & GUI_COLOR_EDIT_FLOAT ) != 0;

    /* HSV working copy (only valid when is_hsv). */
    f32 hsv[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if ( is_hsv )
    {
        color_rgb_to_hsv( v[0], v[1], v[2], &hsv[0], &hsv[1], &hsv[2] );
        if ( n == 4 ) hsv[3] = v[3];
    }

    /* ABGR preview color -- recomputed after any change. */
    u8 pr = (u8)( saturate( v[0] ) * 255.0f + 0.5f );
    u8 pg = (u8)( saturate( v[1] ) * 255.0f + 0.5f );
    u8 pb = (u8)( saturate( v[2] ) * 255.0f + 0.5f );
    u8 pa = ( n == 4 && !( flags & GUI_COLOR_EDIT_NO_ALPHA ) )
                ? (u8)( saturate( v[3] ) * 255.0f + 0.5f ) : 255u;
    u32 abgr = GUI_COLOR( pr, pg, pb, pa );

    /* ---- Inline row: [preview_sq] [drag0 .. dragN-1] | label ---- */
    f32 preview_w = (f32)WIDGET_H;
    f32 gap       = (f32)s_style.widget_gap;
    f32 ctrl_min  = preview_w + gap + 44.0f * (f32)comps + gap * (f32)( comps - 1u );
    gui_rect_t ctrl = draw_field_label( r, label, ctrl_min, COL_TEXT_DIM );

    /* Clickable color square -- placed first for fast visual identification. */
    gui_rect_t preview_r = { ctrl.x, ctrl.y, preview_w, ctrl.h };
    gui_item_state_t pst = item_state( id_combine( id, 1u ), preview_r, ITEM_BUTTON );
    {
        f32 sv = draw_rounding();
        draw_set_rounding( 2.0f );
        draw_fill( preview_r, col_item_bg( pst ) );
        gui_rect_t inner = { preview_r.x + 2.0f, preview_r.y + 2.0f,
                             preview_r.w - 4.0f,  preview_r.h - 4.0f };
        if ( pa < 255u )
            draw_checker( inner, 3.0f, GUI_COLOR( 200, 200, 200, 255 ), GUI_COLOR( 100, 100, 100, 255 ) );
        draw_fill( inner, abgr );
        draw_outline( preview_r, WIN_BORDER, pst.hover ? COL_WIDGET_HOT : COL_BORDER );
        draw_set_rounding( sv );
    }

    /* Hover tooltip: swatch + hex + component values.
       tooltip_begin does NOT check hover -- it always opens the window; caller must guard.
       Pre-compute all strings and the required content width before opening the tooltip so
       that gui_empty() can force content_w to the correct value on this frame.  Without this,
       gui_text() inside would call draw_text_fit_n which truncates text at the (still-narrow)
       window width, preventing content_w from ever growing to fit the actual text. */
    if ( pst.hover )
    {
        char tip_hex[ 12 ], tip_vals[ 32 ], tip_alp[ 24 ];
        tip_alp[ 0 ] = '\0';
        fmt_snprintf( tip_hex, sizeof( tip_hex ), "#%02X%02X%02X%02X", pr, pg, pb, pa );
        if ( is_hsv )
            fmt_snprintf( tip_vals, sizeof( tip_vals ), "H:%d  S:%d  V:%d",
                      (i32)( hsv[0] * 360.0f + 0.5f ),
                      (i32)( hsv[1] * 100.0f + 0.5f ),
                      (i32)( hsv[2] * 100.0f + 0.5f ) );
        else
            fmt_snprintf( tip_vals, sizeof( tip_vals ), "R:%d  G:%d  B:%d",
                      (i32)pr, (i32)pg, (i32)pb );
        bool tip_has_alpha = ( n == 4 && !( flags & GUI_COLOR_EDIT_NO_ALPHA ) );
        if ( tip_has_alpha )
            fmt_snprintf( tip_alp, sizeof( tip_alp ), "A:%d  (%.0f%%)", (i32)pa, (f64)v[3] * 100.0 );

        f32 tip_w = 72.0f;
        f32 hw = font_text_w_n( tip_hex,  0xFFFFFFFFu ); if ( hw > tip_w ) tip_w = hw;
        f32 vw = font_text_w_n( tip_vals, 0xFFFFFFFFu ); if ( vw > tip_w ) tip_w = vw;
        if ( tip_has_alpha ) {
            f32 aw = font_text_w_n( tip_alp, 0xFFFFFFFFu ); if ( aw > tip_w ) tip_w = aw;
        }

        if ( gui_tooltip_begin() )
        {
            gui_stack();
            gui_empty( tip_w, 0.0f );   /* force content_w immediately so autosize is right */
            gui_rect_t tp = gui_canvas( 56.0f );
            {
                f32 sv = draw_rounding();
                draw_set_rounding( 3.0f );
                if ( pa < 255u )
                    draw_checker( tp, 6.0f, GUI_COLOR( 200, 200, 200, 255 ),
                                  GUI_COLOR( 100, 100, 100, 255 ) );
                draw_fill( tp, abgr );
                draw_outline( tp, WIN_BORDER, COL_BORDER );
                draw_set_rounding( sv );
            }
            gui_text( tip_hex );
            gui_text( tip_vals );
            if ( tip_has_alpha ) gui_text( tip_alp );
        }
        gui_tooltip_end();
    }

    /* Drag fields -- one per component, sharing remaining control width equally.  slot is the
       IDEAL (fractional) box width; the loop snaps cumulative edges rather than flooring slot per
       box, so the sub-pixel remainder is spread one pixel at a time across the borders as the row
       grows.  Every border then steps monotonically instead of the last box absorbing the whole
       remainder and snapping back each time a floored field width ticked (border wobble). */
    bool changed  = false;
    f32  base_x   = ctrl.x + preview_w + gap;                             /* left edge, first box */
    f32  span     = ctrl.w - preview_w - gap;                            /* boxes + inter-box gaps */
    f32  slot     = ( span - gap * (f32)( comps - 1u ) ) / (f32)comps;   /* ideal fractional width */

    /* Integer formats are space-padded to a fixed 3-digit field (R/G/B/A max 255, H max 360,
       S/V max 100 -- all <= 3 digits) so the monospace label width never changes with the value.
       Constant width keeps every box's centered text on a stable column: it no longer ticks
       forward at a different resize threshold than its neighbors.  Float labels are already
       constant width ("0.00".."1.00"). */
    static const char* s_rgb_i[] = { "R:%3d",  "G:%3d",  "B:%3d",  "A:%3d"  };
    static const char* s_rgb_f[] = { "R:%.2f", "G:%.2f", "B:%.2f", "A:%.2f" };
    static const char* s_hsv_i[] = { "H:%3d",  "S:%3d",  "V:%3d",  "A:%3d"  };
    static const char* s_hsv_f[] = { "H:%.2f", "S:%.2f", "V:%.2f", "A:%.2f" };

    for ( u32 i = 0; i < comps; ++i )
    {
        f32 lead = base_x + (f32)i * ( slot + gap );   /* ideal left edge of this box */
        f32 x0   = floorf( lead );
        f32 x1   = ( i + 1u < comps ) ? floorf( lead + slot ) : floorf( ctrl.x + ctrl.w );
        gui_rect_t drag_r = { x0, ctrl.y, x1 - x0, ctrl.h };

        gui_id_t cid = id_combine( id, 10u + i );
        f32      val  = is_hsv ? hsv[i] : v[i];

        if ( is_flt )
        {
            const char* fmt = is_hsv ? s_hsv_f[i] : s_rgb_f[i];
            if ( drag_float_box( cid, drag_r, &val, 0.005f, 0.0f, 1.0f, fmt ) )
            {
                if ( is_hsv ) hsv[i] = val; else v[i] = val;
                changed = true;
            }
        }
        else
        {
            i32         max_v = ( is_hsv && i == 0 ) ? 360
                              : ( ( is_hsv && i < 3u ) ? 100 : 255 );
            i32         ival  = (i32)( val * (f32)max_v + 0.5f );
            const char* fmt   = is_hsv ? s_hsv_i[i] : s_rgb_i[i];
            if ( drag_int_box( cid, drag_r, &ival, 1.0f, 0, max_v, fmt ) )
            {
                val = (f32)ival / (f32)max_v;
                if ( is_hsv ) hsv[i] = val; else v[i] = val;
                changed = true;
            }
        }
    }
    #undef EVEN_FLOOR

    if ( changed && is_hsv )
    {
        color_hsv_to_rgb( hsv[0], hsv[1], hsv[2], &v[0], &v[1], &v[2] );
        if ( n == 4 ) v[3] = hsv[3];
        /* Refresh ABGR after HSV writeback. */
        pr   = (u8)( saturate( v[0] ) * 255.0f + 0.5f );
        pg   = (u8)( saturate( v[1] ) * 255.0f + 0.5f );
        pb   = (u8)( saturate( v[2] ) * 255.0f + 0.5f );
        abgr = GUI_COLOR( pr, pg, pb, pa );
    }

    /* ---- Picker popup (click on the color square to open) ---- */
    char pid[64];
    fmt_snprintf( pid, sizeof( pid ), "##cpick_%u", id );

    if ( pst.clicked )
        gui_popup_open( pid );

    if ( gui_popup_begin( pid, GUI_WIN_ALWAYS_AUTOSIZE ) )
    {
        gui_stack();
        /* Pin popup width: autosize normally sizes to content; an explicit empty at the target width
           forces content_w to 200px so the popup isn't as narrow as the shortest drag-float label. */
        gui_empty( 200.0f, 0.0f );

        /* Large color preview swatch. */
        {
            gui_rect_t pp = gui_canvas( 52.0f );
            f32 sv = draw_rounding();
            draw_set_rounding( 4.0f );
            if ( pa < 255u )
                draw_checker( pp, 6.0f, GUI_COLOR( 200, 200, 200, 255 ), GUI_COLOR( 100, 100, 100, 255 ) );
            draw_fill( pp, abgr );
            draw_outline( pp, WIN_BORDER, COL_BORDER );
            draw_set_rounding( sv );
        }

        /* Hex string centered under the swatch. */
        {
            char hex[12];
            fmt_snprintf( hex, sizeof( hex ), "#%02X%02X%02X%02X", pr, pg, pb, pa );
            gui_text( hex );
        }

        gui_separator();

        /* Channel sliders -- respect the display mode flag. */
        if ( is_hsv )
        {
            f32  hh = hsv[0], ss = hsv[1], vv = hsv[2];
            bool hc = gui_drag_float( "H", &hh, 0.002f, 0.0f, 1.0f, "H: %.3f" );
            bool sc = gui_drag_float( "S", &ss, 0.005f, 0.0f, 1.0f, "S: %.3f" );
            bool vc = gui_drag_float( "V", &vv, 0.005f, 0.0f, 1.0f, "V: %.3f" );
            if ( hc || sc || vc )
            {
                color_hsv_to_rgb( hh, ss, vv, &v[0], &v[1], &v[2] );
                changed = true;
            }
        }
        else
        {
            if ( gui_drag_float( "R", &v[0], 0.005f, 0.0f, 1.0f, "R: %.3f" ) ) changed = true;
            if ( gui_drag_float( "G", &v[1], 0.005f, 0.0f, 1.0f, "G: %.3f" ) ) changed = true;
            if ( gui_drag_float( "B", &v[2], 0.005f, 0.0f, 1.0f, "B: %.3f" ) ) changed = true;
        }
        if ( n == 4 && !( flags & GUI_COLOR_EDIT_NO_ALPHA ) )
        {
            if ( gui_drag_float( "A", &v[3], 0.005f, 0.0f, 1.0f, "A: %.3f" ) ) changed = true;
        }

        gui_popup_end();
    }

    return changed;
}

bool gui_color_edit3( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags )
{ return color_edit_n( label, col, 3u, flags ); }

bool gui_color_edit4( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags )
{ return color_edit_n( label, col, 4u, flags ); }

// clang-format on
/*============================================================================================*/
