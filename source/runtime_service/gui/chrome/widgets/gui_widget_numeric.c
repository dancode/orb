/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_widget_numeric.c -- Numeric text-input widgets.

    A family of widgets that present a text field seeded with the formatted current value on
    focus gain.  The user edits freely; on Enter or focus loss the text is parsed back via
    strtod (which handles both decimal and scientific notation, e.g. "1e+8" -> 1000000.0).

        input_int / input_float / input_double  -- optional [-][+] step buttons at the right
            edge of the control rect when step != 0.  Ctrl held at click uses step_fast instead.
            The buttons use GUI_ITEM_BUTTON_REPEAT so holding fires continuously.

        input_float2 / float3 / float4  -- N equal sub-boxes across the control track with no
            step buttons.

    Shared scratch buffer: one static slot (only one field can be focused at a time), keyed by id.
    Focus gain seeds it from the current value; Enter / focus loss parses it back.

    Included by gui.c after gui_widget_slider.c (which is after the widget family files, so
    item_state, draw_field_label, input_field_edit, and the COL_* / WIDGET_ / WIN_
    style vocabulary from gui_style.c are all in scope).

==============================================================================================*/
// clang-format off

#define NUM_BUF_CAP 64

/* Inner body: framed text box for one numeric component.  The seed / edit / parse cycle is the
   shared num_edit_field (gui_text_edit.c); this wrapper owns only the input-style frame draw and,
   when not focused, the static value display.  Returns true and writes *out only on a commit. */
static bool
input_num_field( gui_id_t id, gui_rect_t box_r, gui_item_state_t st,
                 const char* fmt, bool is_int, double cur, double* out )
{
    /* Box background and border. */
    draw_fill( box_r, st.focused ? COL_INPUT_FOCUS : col_frame_bg( st, COL_INPUT_BG ) );
    draw_outline( box_r, WIN_BORDER, st.focused ? COL_WIDGET_HOT : COL_BORDER );

    /* input_* seed the editor with the same format they display, so the field opens on the value
       exactly as it was shown. */
    bool committed = num_edit_field( id, box_r, st, fmt, is_int, cur, out );

    /* Not focused: input_field_edit drew nothing this frame, so paint the static value.  No
       per-widget clip: a static value never scrolls and fits the box in the common case, and the
       window's clip rect already bounds any overflow -- so the field never forces a batch split,
       and a rare over-long value is clipped by the window edge rather than ellipsized. */
    if ( !st.focused )
    {
        char disp[ NUM_BUF_CAP ];
        if ( is_int ) fmt_snprintf( disp, NUM_BUF_CAP, fmt, (int)( committed ? *out : cur ) );
        else          fmt_snprintf( disp, NUM_BUF_CAP, fmt, committed ? *out : cur );
        draw_push_text( box_r.x + WIDGET_PAD, text_center_y( box_r.y, box_r.h ),
                        COL_TEXT, disp );
    }

    return committed;
}

/* Small framed [-] or [+] button used by the step controls. */
static bool
num_step_button( gui_id_t id, gui_rect_t r, bool is_minus )
{
    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
    draw_fill( r, col_item_bg( st ) );
    draw_outline( r, WIN_BORDER, COL_BORDER );
    const char* sym = is_minus ? "-" : "+";
    f32 sw = font_text_w( sym );
    draw_push_text( r.x + ( r.w - sw ) * 0.5f, text_center_y( r.y, r.h ), COL_TEXT, sym );
    return st.clicked;
}

/* Floor a step increment at what the display can actually show, so a [-]/[+] click always moves
   the rendered value instead of nudging it by a sub-display amount (e.g. step 0.001 against "%.1f"
   would leave the printed "0.1" unchanged for ten clicks).  Only the magnitude is raised, so the
   button's direction is preserved.  Integer formats have no sub-unit resolution and a whole-number
   step already clears their display, so they pass through.  The double-domain, increment-side twin
   of value_step_f32's value-side floor (gui_widget_slider.c) -- both read the one display-resolution
   primitive, fmt_decimal_step. */
static double
num_step_visible( double inc, const char* fmt, bool is_int )
{
    if ( is_int ) return inc;
    double min_step = (double)fmt_decimal_step( fmt );
    double mag      = ( inc < 0.0 ) ? -inc : inc;
    if ( mag < min_step )
        inc = ( inc < 0.0 ) ? -min_step : min_step;
    return inc;
}

/* Shared label-split + interaction for a single numeric value.
   step == 0 suppresses the [-][+] buttons; step_fast applies when Ctrl is held.  Whichever step
   applies is floored (num_step_visible) so every click produces a visible change. */
static bool
input_scalar( const char* label, double cur, double* out,
              double step, double step_fast, const char* fmt, bool is_int )
{
    gui_id_t   id  = item_id( label );
    gui_rect_t r   = cell_next( WIDGET_H );

    bool has_steps = ( step != 0.0 );
    f32  btn_w     = has_steps ? 2.0f * WIDGET_H : 0.0f;
    f32  min_ctrl  = font_char_h() * 3.0f + btn_w;
    gui_rect_t ctrl = draw_field_label( r, label, min_ctrl, COL_TEXT_DIM );

    gui_rect_t   box_r = { ctrl.x, ctrl.y, ctrl.w - btn_w, ctrl.h };
    gui_item_state_t st    = item_state( id, box_r, ITEM_FOCUSABLE );

    bool   changed = input_num_field( id, box_r, st, fmt, is_int, cur, out );
    double base    = changed ? *out : cur;

    if ( has_steps )
    {
        bool ctrl_held = io_ctrl();
        double inc = ( ctrl_held && step_fast > 0.0 ) ? step_fast : step;
        inc = num_step_visible( inc, fmt, is_int );

        f32 bx = ctrl.x + ctrl.w - btn_w;
        gui_rect_t minus_r = { bx,            ctrl.y, WIDGET_H, ctrl.h };
        gui_rect_t plus_r  = { bx + WIDGET_H, ctrl.y, WIDGET_H, ctrl.h };

        /* Bracket the step buttons as sub-items of this widget: is_item_* after input_int/float
           reports the text box, not the final button, and the BUTTON_REPEAT tweak (the buttons
           call item_state directly, no cell emit, so item_flags_resolve never runs) stays scoped
           to the pair. */
        gui_item_sub_t sub = gui_item_sub_begin();
        s_scope.flags |= GUI_ITEM_BUTTON_REPEAT;
        if ( num_step_button( id_combine( id, 1u ), minus_r, true  ) ) { *out = base - inc; changed = true; }
        if ( num_step_button( id_combine( id, 2u ), plus_r,  false ) ) { *out = base + inc; changed = true; }
        gui_item_sub_end( sub );
    }

    return changed;
}

bool
gui_input_int( const char* label, i32* v, i32 step, i32 step_fast )
{
    double out;
    bool changed = input_scalar( label, (double)*v, &out,
                                 (double)step, (double)step_fast, "%d", true );
    if ( changed ) *v = (i32)out;
    return changed;
}

bool
gui_input_float( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt )
{
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";
    double out;
    bool changed = input_scalar( label, (double)*v, &out,
                                 (double)step, (double)step_fast, fmt, false );
    if ( changed ) *v = (f32)out;
    return changed;
}

bool
gui_input_double( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt )
{
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.6f";
    double out;
    bool changed = input_scalar( label, *v, &out, step, step_fast, fmt, false );
    if ( changed ) *v = out;
    return changed;
}

/* N-component float row: N equal text sub-boxes across the control track, no step buttons. */
static bool
input_float_n( const char* label, f32* v, u32 n, const char* fmt )
{
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";
    gui_id_t   id   = item_id( label );
    gui_rect_t r    = cell_next( WIDGET_H );
    gui_rect_t ctrl = draw_field_label( r, label, font_char_h() * 3.0f * (f32)n, COL_TEXT_DIM );

    bool changed = false;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 x0 = ctrl.x + (f32)i        * ctrl.w / (f32)n;
        f32 x1 = ctrl.x + (f32)(i + 1u) * ctrl.w / (f32)n;
        gui_rect_t sub  = { floorf( x0 ), ctrl.y, floorf( x1 ) - floorf( x0 ), ctrl.h };
        gui_id_t   sid  = id_combine( id, i + 1u );
        gui_item_state_t st = item_state( sid, sub, ITEM_FOCUSABLE );

        double out;
        if ( input_num_field( sid, sub, st, fmt, false, (double)v[ i ], &out ) )
        {
            v[ i ]  = (f32)out;
            changed = true;
        }
    }
    return changed;
}

bool gui_input_float2( const char* label, f32* v, const char* fmt )
{ return input_float_n( label, v, 2u, fmt ); }

bool gui_input_float3( const char* label, f32* v, const char* fmt )
{ return input_float_n( label, v, 3u, fmt ); }

bool gui_input_float4( const char* label, f32* v, const char* fmt )
{ return input_float_n( label, v, 4u, fmt ); }

// clang-format on
/*============================================================================================*/
