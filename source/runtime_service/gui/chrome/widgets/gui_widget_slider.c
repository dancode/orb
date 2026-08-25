/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_widget_slider.c -- Slider and drag widgets.

    Every value-editing control here shares the drag-value interaction pattern: item_state
    claims active_id on mouse press (ITEM_DRAG), keeping the drag bound to the widget while
    the cursor sweeps off it.  The displayed value changes live while dragging.

        slider_float / slider_float_step / slider_int  -- a horizontal track with a knob;
            the cursor's fraction along the track maps directly to the value range.
        drag_int / drag_float / drag_float2/3/4  -- no track travel; value changes by cursor
            displacement from the press anchor (s_drag_anchor_v / s_drag_anchor_f), so there is
            no range cap and no knob.  The N-component variants lay equal sub-boxes across the row.

    The color widgets (color_edit3/4, color_picker3/4) build on the drag boxes here; they live
    in gui_widget_color.c, included after this file.

    slider_render is the shared visual: track frame, fill bar, knob, and centered value text.

    Included by gui_chrome.c after the widget family files, sharing their vocabulary: item_state
    (core), cell_next (flow), gui_field_row (stock), and the COL_* / WIDGET_ / WIN_ macros
    (style/gui_style.h).

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

/*==============================================================================================
    Keyboard arrow-step apply -- the shared tail every value widget (slider / drag, float / int)
    runs once its captured nav_adjust (-1 / +1 this frame) is known.  The per-widget code derives
    the BASE step magnitude (its own semantics: a slider's 1%-of-range, a drag's v_speed) and hands
    it here; the helper owns the parts that must be identical everywhere -- the "never nudge by less
    than the display can show" floor, the bounded clamp, and the write-and-report gate -- so a fix
    to any of those lands on all of them at once.  Returns true and writes *v only on a real change.
    Bounds clamp only when lo < hi (a drag passing v_min == v_max stays unbounded; a slider always
    has lo < hi).
==============================================================================================*/

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
slider_render( gui_id_t id, gui_rect_t track_r, gui_item_state_t st, f32 t, const char* value_text )
{
    t = saturate( t );

    /* ONE mix for the whole control: track, fill bar and knob are three rows of a single
       interaction and have to travel together, which two probes would not guarantee. */
    gui_style_mix_t mix = style_mix( id, st, false );

    /* Track frame -- the widget BODY, so it lifts exactly like every other control face: the same
       col_frame_bg step the checkbox box, the drag box, and the input field take (BG[HOT] on
       hover / nav, BG[ACTIVE] on press) over the track's own ACCENT[DIM] resting colour.  It used
       to lift WITHIN the accent role instead (DIM -> IDLE), but ACCENT[IDLE] IS the value bar's
       colour, so hovering painted the EMPTY remainder the same blue as the filled part and every
       slider read as 100% full.  The knob does NOT ride this row (see col_grab below) -- if it
       did it would match the hovered track exactly and vanish into it. */
    draw_face_field_mix( track_r, mix, GUI_ROLE_ACCENT, GUI_PHASE_INERT, 0u, 0.0f );
    /* Captured for keyboard value edit (st.focused -- see nav_item_register) gets the same border
       lift text/numeric fields use on focus, so going from nav highlight to Left/Right-adjust reads
       as a real state change instead of an invisible one. */
    draw_outline( track_r, WIN_BORDER, col_field_border( st ) );

    /* Fill bar up to t.  Round only the start (left) corners to match the track frame; keep the
       leading (right) edge facing the knob square, so a rounded leading edge never leaves a gap
       between the fill and the handle.  Per-corner via draw_round_rect_ex (gui_symbol.c).

       The bar lifts along its OWN role, straight off gui_item_phase -- ACCENT is the value the
       control HOLDS and its four cells are a real ramp (empty track / fill / engaged / dragged),
       so the generic state->phase mapping applies here exactly as it does to a button face.  It
       has to lift: the track it sits in lifts too, and ACCENT[IDLE] against BG[HOT] is
       near-isoluminant in the dark palette, so a fixed bar colour on a hovered track would trade
       one "looks full" read for another.  (Before the ACCENT/MARK split this cell held the check
       mark -- a green -- and this line could not have been written.) */
    f32 fill_w = t * ( track_r.w - SLIDER_KNOB_W );
    if ( fill_w > 0.0f )
        draw_round_rect_ex( ( gui_rect_t ){ track_r.x, track_r.y + 1.0f, fill_w, track_r.h - 2.0f },
                            ROUND_WIDGET, 0.0f, 0.0f, ROUND_WIDGET, 0.0f,
                            style_col_mix( GUI_ROLE_ACCENT, mix ) );

    /* Knob (grab): the GRAB row, which exists precisely so this element has somewhere to stand.
       It is the only part of the slider with two lifting neighbours -- the track beneath it and
       the fill bar it butts against -- and GRAB is authored per theme as the contrast anchor
       (light knob on a dark theme, dark knob on a light one), so it reads against both without
       depending on the outline to carry it.  The outline stays as the edge, not as the contrast.
       A bar grab by default (grab radius -- raise GUI_VAR_ROUND for a pill), or a circular handle
       when GUI_VAR_KNOB_SHAPE selects it. */
    f32 knob_x = track_r.x + t * ( track_r.w - SLIDER_KNOB_W );
    if ( style_shape( GUI_VAR_KNOB_SHAPE ) == GUI_SLIDER_KNOB_CIRCLE )
    {
        f32 kcx = knob_x + SLIDER_KNOB_W * 0.5f;
        f32 kcy = track_r.y + track_r.h * 0.5f;
        f32 kr  = track_r.h * 0.5f;
        draw_circle( kcx, kcy, kr, 0.0f,      style_col_mix( GUI_ROLE_GRAB, mix ) );
        draw_circle( kcx, kcy, kr, WIN_BORDER, COL_BORDER_IDLE );
    }
    else
    {
        f32 save_round = draw_rounding();
        draw_set_rounding( ROUND_WIDGET );
        gui_rect_t knob_r = { knob_x, track_r.y, SLIDER_KNOB_W, track_r.h };
        draw_fill   ( knob_r, style_col_mix( GUI_ROLE_GRAB, mix ) );
        draw_outline( knob_r, WIN_BORDER, COL_BORDER_IDLE );
        draw_set_rounding( save_round );
    }

    if ( value_text && !( s_scope.flags & GUI_ITEM_NO_VALUE_TEXT ) )
    {
        f32 inner = track_r.w - 2.0f * WIDGET_PAD;
        f32 tw    = font_text_w_n( value_text, 0xFFFFFFFFu );
        f32 tx    = track_r.x + ( track_r.w - tw ) * 0.5f;
        if ( tx < track_r.x + WIDGET_PAD ) tx = track_r.x + WIDGET_PAD;
        draw_text_fit_n( tx, text_center_y( track_r.y, track_r.h ), COL_TEXT_PRIMARY_IDLE, value_text, 0xFFFFFFFFu, inner );
    }
}

/* Track rect at the press that started the active slider drag -- single-slot like s_drag_anchor_x,
   since only one widget owns active_id at a time.  Shared by slider_float_step and slider_int.

   The fraction has to map against THIS, not the live per-frame track_r: a slider can sit in a
   panel whose own layout responds to the value the slider writes (e.g. a UI-scale lever resizing
   its own host panel).  Reading live geometry mid-drag turns that into a feedback loop -- the
   panel reflows under a stationary cursor, the reflowed track maps the same cursor x to a
   different value, and the two states alternate every frame.  Anchoring the mapping to the
   geometry at press time keeps the value a pure function of cursor displacement, so it converges
   instead of oscillating no matter what the drag itself causes to move. */

static gui_rect_t s_slider_anchor_track;

/*==============================================================================================
    Click-to-animate -- opt-in, one-shot per slider call (gui_next_slider_animate).  Armed, a
    plain click (press+release, no drag) eases the value to the clicked position instead of
    jumping there; an actual drag past SLIDER_ANIM_DRAG_THRESH cancels the tween and hands the
    value straight back to live cursor tracking, same as an un-armed slider.  Un-armed, none of
    this runs: a slider that never opts in costs one GUI_STATE_PEEK per call and nothing else.
==============================================================================================*/

#define GUI_SLIDER_ANIM_SALT    0x51DEA000u
#define SLIDER_ANIM_DRAG_THRESH 4.0f    // px; matches GUI_DRAG_THRESH / PRESS_DEFER_THRESH elsewhere

typedef struct
{
    f32        from;      // value when the tween armed
    f32        dest;      // click-target value; re-aimed each held frame until release or cancel
    gui_ease_t ease;      // captured at arm -- the next-call latch may be reused before this settles
    bool       active;    // tween in flight -- owns the value instead of live cursor tracking
    bool       canceled;  // press turned into a real drag -- live tracking took the value back

} slider_anim_t;

/* The one-shot latch itself: set by gui_next_slider_animate, consumed unconditionally by the very
   next slider call -- same push-model shape as gui_next_input_filter (chrome/widgets/gui_input.c),
   which is cleared by input_text_begin on EVERY call, not only on some interaction event.  A
   slider must do the same: consuming only inside st.pressed left the flag standing on every frame
   the armed slider wasn't actually clicked, so the first click on a LATER, un-armed slider picked
   up the stale latch instead. */
static gui_ease_t s_slider_anim_next_ease;
static f32        s_slider_anim_next_duration;
static bool       s_slider_anim_next_armed;

void
gui_next_slider_animate( gui_ease_t ease, f32 duration )
{
    s_slider_anim_next_ease     = ease;
    s_slider_anim_next_duration = duration;
    s_slider_anim_next_armed    = true;
}

typedef struct
{
    bool       armed;
    gui_ease_t ease;
    f32        duration;

} slider_anim_req_t;

/* Read-and-clear the latch -- call once per slider, unconditionally, before st.pressed is even
   known, so a slider that goes unclicked this frame still consumes (and drops) whatever was
   armed for it rather than leaving it for the next slider down. */
static slider_anim_req_t
slider_anim_consume_next( void )
{
    slider_anim_req_t req = { s_slider_anim_next_armed, s_slider_anim_next_ease, s_slider_anim_next_duration };
    s_slider_anim_next_armed = false;
    return req;
}

/* Press-frame consume: arms a tween from `v_now` if `req` was armed for this call, otherwise drops
   any tween still settling from a prior click on this same slider (a plain click always jumps,
   even if the last click armed one).  Called once, from inside the st.pressed branch; `dest` is
   set the same frame by slider_anim_on_active (st.pressed implies st.active). */
static void
slider_anim_on_press( gui_id_t id, f32 v_now, slider_anim_req_t req )
{
    item_drag_arm( id );

    gui_id_t       aid  = id_combine( id, GUI_SLIDER_ANIM_SALT );
    slider_anim_t* anim = GUI_STATE( slider_anim_t, aid );
    if ( req.armed )
    {
        anim->from     = v_now;
        anim->ease     = req.ease;
        anim->active   = true;
        anim->canceled = false;
        gui_anim_start( aid, req.duration );
    }
    else
    {
        anim->active = false;
    }
}

/* Held-frame consume: aims the tween at the live click point and cancels it the instant the press
   turns into a real drag.  `nv` is the freshly computed cursor value.  Returns true if the caller
   should leave *v alone this frame (the tween owns it). */
static bool
slider_anim_on_active( gui_id_t id, f32 nv )
{
    gui_id_t aid = id_combine( id, GUI_SLIDER_ANIM_SALT );
    const slider_anim_t* peek = GUI_STATE_PEEK( slider_anim_t, aid );
    if ( !peek || !peek->active || peek->canceled )
        return false;

    slider_anim_t* anim = GUI_STATE( slider_anim_t, aid );
    anim->dest = nv;

    if ( item_drag_exceeded( id, SLIDER_ANIM_DRAG_THRESH ) )
    {
        anim->canceled = true;
        return false;   /* real drag -- hand the value straight back to live tracking */
    }
    return true;
}

/* Runs every call, independent of st.active: a tween keeps driving the value after release, until
   it settles.  PEEK first so an un-armed slider (the default) pays for one lookup and nothing
   more.  Returns true and writes *out_value when the tween is live this frame. */
static bool
slider_anim_sample( gui_id_t id, f32* out_value )
{
    gui_id_t aid = id_combine( id, GUI_SLIDER_ANIM_SALT );
    const slider_anim_t* peek = GUI_STATE_PEEK( slider_anim_t, aid );
    if ( !peek || !peek->active || peek->canceled )
        return false;

    slider_anim_t* anim   = GUI_STATE( slider_anim_t, aid );
    bool           active = false;
    f32            t      = gui_anim_ease( aid, anim->ease, &active );
    *out_value = f32_lerp( anim->from, anim->dest, t );
    if ( !active )
        anim->active = false;   /* settled -- next call falls through to normal handling */
    return true;
}

/* slider_float_step -- slider_float quantized to `step` (e.g. 0.25 lands the value on 1/4 marks);
   step <= 0 leaves it continuous, so plain slider_float just forwards with step 0. */

bool
gui_slider_float_step( const char* label, f32* v, f32 lo, f32 hi, f32 step )
{
    gui_id_t id = item_id( label );

    /* Unconditional, every call -- see slider_anim_consume_next. */
    slider_anim_req_t anim_req = slider_anim_consume_next();

    /* Route the widget's own label through the ambient field seam (gui_field_row): it paints the label
       -- an aligned column under a form / field_split, or trailing otherwise -- and hands back the
       control track as the next cell.  With labels hidden (field.hide), skipped (skip_label), or
       empty ("##id") it is a no-op and the track fills the whole row.  No second "_label" widget. */

    gui_field_row( label );
    gui_rect_t track_r = cell_next( WIDGET_H );
    gui_item_state_t st = item_state( id, track_r, ITEM_DRAG );

    if ( st.pressed )
    {
        s_slider_anchor_track = track_r;
        slider_anim_on_press( id, *v, anim_req );
    }

    /* Drag: map the cursor's track fraction to a value, snapping to the step grid when asked. */

    bool changed = false;
    if ( st.active )
    {
        gui_rect_t at = s_slider_anchor_track;
        f32 t  = saturate( ( s_io.mouse_x - at.x ) / at.w );
        f32 nv = lo + t * ( hi - lo );
        if ( step > 0.0f )
             nv = lo + floorf( ( nv - lo ) / step + 0.5f ) * step;   /* nearest step from lo */
        if ( nv < lo ) nv = lo;
        if ( nv > hi ) nv = hi;

        if ( !slider_anim_on_active( id, nv ) && nv != *v )
        {
            *v      = nv;
            changed = true;
        }
    }

    /* Keyboard value edit (activation captured this slider -- st.nav_adjust): each Left/Right
       repeat steps the quantize step when set, else 1% of the range.  value_step_f32 floors that at
       SLIDER_FLOAT_FMT's resolution and clamps, so a small-range slider still moves the printed
       value every press.  A live click-to-animate tween is dropped first -- direct keyboard edits
       win outright rather than fighting the tween's write next frame. */
    if ( st.nav_adjust != 0 )
    {
        slider_anim_t* anim = GUI_STATE( slider_anim_t, id_combine( id, GUI_SLIDER_ANIM_SALT ) );
        anim->active = false;

        f32 base = ( step > 0.0f ) ? step : ( hi - lo ) * 0.01f;
        if ( value_step_f32( v, st.nav_adjust, base, SLIDER_FLOAT_FMT, lo, hi ) )
            changed = true;
    }

    /* A tween keeps driving the value after release too, until it settles -- independent of
       st.active so the ease continues on the frames after the mouse comes up. */
    f32 anim_v;
    if ( slider_anim_sample( id, &anim_v ) && anim_v != *v )
    {
        *v      = anim_v;
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
        slider_render( id, track_r, st, t_cur, buf );
    }
    return changed;
}

bool
gui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
{
    return gui_slider_float_step( label, v, lo, hi, 0.0f );
}

/* slider_int -- the integer slider; every track position lands on a whole value in [lo,hi].
   format is the printf form of the shown value ("%d" when NULL/empty, e.g. "Quality: %d") --
   same DragInt idiom drag_int_box uses.  A format with no conversion specifier at all (e.g. an
   options ladder passing items[*v] straight through) prints that string verbatim and *v is
   simply an unused vararg, so an option-list slider reads its rung's name instead of its
   index -- see the DPI ladder in gui_frame_overlay.c. */
bool
gui_slider_int( const char* label, i32* v, i32 lo, i32 hi, const char* format )
{
    if ( !format || !format[ 0 ] ) format = "%d";

    gui_id_t id = item_id( label );

    /* Unconditional, every call -- see slider_anim_consume_next. */
    slider_anim_req_t anim_req = slider_anim_consume_next();

    gui_field_row( label );   /* label via the ambient field seam; track_r = control track (see slider_float_step) */
    gui_rect_t track_r = cell_next( WIDGET_H );
    gui_item_state_t st = item_state( id, track_r, ITEM_DRAG );

    if ( st.pressed )
    {
        s_slider_anchor_track = track_r;
        slider_anim_on_press( id, (f32)*v, anim_req );
    }

    bool changed = false;
    if ( st.active )
    {
        gui_rect_t at = s_slider_anchor_track;
        f32 t  = saturate( ( s_io.mouse_x - at.x ) / at.w );
        i32 nv = lo + (i32)floorf( t * (f32)( hi - lo ) + 0.5f );    /* nearest whole step */
        if ( nv < lo ) nv = lo;
        if ( nv > hi ) nv = hi;

        if ( !slider_anim_on_active( id, (f32)nv ) && nv != *v )
        {
            *v      = nv;
            changed = true;
        }
    }

    /* Keyboard value edit: one whole step per Left/Right repeat.  Drop a live tween first -- see
       the matching comment in gui_slider_float_step. */
    if ( st.nav_adjust != 0 )
    {
        slider_anim_t* anim = GUI_STATE( slider_anim_t, id_combine( id, GUI_SLIDER_ANIM_SALT ) );
        anim->active = false;

        if ( value_step_i32( v, st.nav_adjust, 1, lo, hi ) )
            changed = true;
    }

    /* A tween keeps driving the value after release too -- see gui_slider_float_step. */
    f32 anim_v;
    if ( slider_anim_sample( id, &anim_v ) )
    {
        i32 av = (i32)floorf( anim_v + 0.5f );
        if ( av != *v )
        {
            *v      = av;
            changed = true;
        }
    }

    /* Paint gate -- see slider_float_step. */
    if ( !draw_cull_box( track_r.x, track_r.y, track_r.w, track_r.h ) )
    {
        f32  t_cur = ( hi > lo ) ? ( (f32)( *v - lo ) / (f32)( hi - lo ) ) : 0.0f;
        char buf[ 32 ];
        fmt_snprintf( buf, sizeof( buf ), format, *v );
        slider_render( id, track_r, st, t_cur, buf );
    }
    return changed;
}

/*==============================================================================================
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
==============================================================================================*/

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
    draw_push_text_clip_n( tx, text_center_y( box_r.y, box_r.h ), COL_TEXT_PRIMARY_IDLE, buf,
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
    draw_face( box_r, GUI_ROLE_BG, GUI_PHASE_IDLE );
    draw_outline( box_r, WIN_BORDER, col_field_border( st ) );
}

static bool
drag_int_box( gui_id_t id, gui_rect_t box_r, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format )
{
    /* While the numeric scratch owns this box (text-entry mode) it must interact as a FIELD,
       not a drag: only a FOCUSABLE press re-claims focus, so under ITEM_DRAG the frame's global
       click-blur would let a caret click inside the box silently drop text mode.  num_edit_active
       is the persistent owner test, known before item_state. */
    gui_item_state_t st = item_state( id, box_r, num_edit_active( id ) ? ITEM_FOCUSABLE : ITEM_DRAG );
    drag_text_enter( id, &st );

    /* Keyboard activation opens text entry, input-field parity: Enter/Space on the nav cursor
       lands as a fresh value-edit capture (nav.edit_id, nav_item_register), and a drag box has a
       text mode to promote it into -- take real focus so the editor seeds this same frame (the
       activating key was already claimed, so it cannot instantly submit).  Sliders keep the
       arrow-step capture; they have nothing to type into. */
    if ( g_ctx->nav.edit_id == id )
    {
        g_ctx->nav.edit_id       = GUI_ID_NONE;
        s_interaction.focused_id = id;
        st.focused               = true;
    }

    bool changed    = false;
    /* Text entry rides on real focus ownership, never st.focused alone: a keyboard value capture
       mirrors focused onto DRAG widgets for the border treatment only (nav_item_register), and
       must keep the drag presentation -- its arrows step the value, they do not type. */
    bool edit_focus = ( s_interaction.focused_id == id );
    bool text_mode  = edit_focus || num_edit_active( id );

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

        u32 bg = col_frame_bg_mix( style_mix( id, st, false ), COL_ACCENT_INERT );
        draw_fill( box_r, bg );
        draw_outline( box_r, WIN_BORDER, col_field_border( st ) );
    }

    /* Value text -- unless the focused editor already painted its own (and caret), or the box is
       scrolled out (paint gate -- see slider_float_step; skips the snprintf + measure). */
    if ( !edit_focus && !draw_cull_box( box_r.x, box_r.y, box_r.w, box_r.h ) )
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

    gui_id_t id = item_id( label );
    gui_field_row( label );   /* label via the ambient field seam; box_r = control track (see slider_float_step) */
    gui_rect_t box_r = cell_next( WIDGET_H );

    return drag_int_box( id, box_r, v, v_speed, v_min, v_max, format );
}

/*==============================================================================================
    drag_float -- the floating-point sibling of drag_int: a framed value field changed by a
    left / right drag, v_speed units of value per pixel, with no track and so no travel cap.  The
    Dear ImGui DragFloat analogue.  v_min < v_max bounds the value; both equal leaves it unbounded.
    fmt is the printf form of the displayed value ("%.3f" when NULL).  drag_float2/3/4 lay N equal
    sub-boxes across the control track (a vector edit), each an independent drag.
==============================================================================================*/

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
    /* FOCUSABLE while the scratch owns the box, text entry on real focus only -- see drag_int_box. */
    gui_item_state_t st = item_state( id, box_r, num_edit_active( id ) ? ITEM_FOCUSABLE : ITEM_DRAG );
    drag_text_enter( id, &st );

    /* Nav Enter/Space promotes the fresh value-edit capture to text entry -- see drag_int_box. */
    if ( g_ctx->nav.edit_id == id )
    {
        g_ctx->nav.edit_id       = GUI_ID_NONE;
        s_interaction.focused_id = id;
        st.focused               = true;
    }

    bool changed    = false;
    bool edit_focus = ( s_interaction.focused_id == id );
    bool text_mode  = edit_focus || num_edit_active( id );

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

        u32 bg = col_frame_bg_mix( style_mix( id, st, false ), COL_ACCENT_INERT );
        draw_fill( box_r, bg );
        draw_outline( box_r, WIN_BORDER, col_field_border( st ) );
    }

    /* Value text -- unless the focused editor already painted its own (and caret), or the box is
       scrolled out (paint gate -- see slider_float_step; skips the snprintf + measure). */
    if ( !edit_focus && !draw_cull_box( box_r.x, box_r.y, box_r.w, box_r.h ) )
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

    gui_id_t id = item_id( label );
    gui_field_row( label );   /* label via the ambient field seam; box_r = control track (see slider_float_step) */
    gui_rect_t box_r = cell_next( WIDGET_H );

    return drag_float_box( id, box_r, v, v_speed, v_min, v_max, fmt );
}

/* N-component drag row: N equal drag-float sub-boxes across the control track. */
static bool
drag_float_n( const char* label, f32* v, u32 n, f32 v_speed, f32 v_min, f32 v_max, const char* fmt )
{
    /* Auto (v_speed <= 0): span the shared [v_min,v_max] range over DRAG_RANGE_SPAN_PX. */
    v_speed = drag_resolve_speed( v_speed, v_max - v_min, 1.0f );
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";

    gui_id_t id = item_id( label );
    gui_field_row( label );
    gui_rect_t ctrl = cell_next( WIDGET_H );

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

// clang-format on
/*============================================================================================*/
