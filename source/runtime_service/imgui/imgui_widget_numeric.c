/*==============================================================================================

    runtime_service/imgui/imgui_widget_numeric.c -- Numeric text-input widgets.

    A family of widgets that present a text field seeded with the formatted current value on
    focus gain.  The user edits freely; on Enter or focus loss the text is parsed back via
    strtod (which handles both decimal and scientific notation, e.g. "1e+8" -> 1000000.0).

        input_int / input_float / input_double  -- optional [-][+] step buttons at the right
            edge of the control rect when step != 0.  Ctrl held at click uses step_fast instead.
            The buttons use IMGUI_ITEM_BUTTON_REPEAT so holding fires continuously.

        input_float2 / float3 / float4  -- N equal sub-boxes across the control track with no
            step buttons.

    Shared scratch buffer: one static slot (only one field can be focused at a time), keyed by id.
    Focus gain seeds it from the current value; Enter / focus loss parses it back.

    Included by imgui.c after imgui_widget_slider.c (which is after imgui_widget.c, so
    widget_behavior, widget_split_label, input_field_edit, the COL_* palette, and the
    WIDGET_/WIN_ layout macros from imgui_widget_core.c are all in scope).

==============================================================================================*/
// clang-format off

#define NUM_BUF_CAP 64

typedef struct
{
    imgui_id_t id;
    char       text[ NUM_BUF_CAP ];
} num_scratch_t;

static num_scratch_t s_num_scratch;

/* Format a double into buf.  is_int casts to (int) first so "%d" does not UB. */
static void
num_format( char* buf, u32 cap, const char* fmt, double val, bool is_int )
{
    if ( is_int ) snprintf( buf, cap, fmt, (int)val );
    else          snprintf( buf, cap, fmt, val );
}

/* Inner body: framed text box for one numeric component.
   Seeds the scratch on focus gain; edits it while focused; parses on Enter or focus loss.
   Returns true and writes *out only when a value commit happens. */
static bool
input_num_field( imgui_id_t id, imgui_rect_t box_r, widget_state_t st,
                 const char* fmt, bool is_int, double cur, double* out )
{
    bool committed = false;

    /* Focus gain: seed the scratch with the current formatted value. */
    if ( st.focused && s_num_scratch.id != id )
    {
        num_format( s_num_scratch.text, NUM_BUF_CAP, fmt, cur, is_int );
        s_num_scratch.id = id;
    }

    /* Box background and border. */
    draw_push_rect_filled( box_r.x, box_r.y, box_r.w, box_r.h,
                           0, 0, 1, 1, 0,
                           st.focused ? COL_INPUT_FOCUS : frame_bg_color( st, COL_INPUT_BG ) );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h,
                            WIN_BORDER, 0,
                            st.focused ? COL_WIDGET_HOT : COL_BORDER );

    if ( st.focused )
    {
        /* Text editing from the scratch; Enter commits, Escape reverts via undo ring. */
        input_field_result_t res =
            input_field_edit( id, box_r, st, s_num_scratch.text, NUM_BUF_CAP, NULL, NULL );
        if ( res.enter )
        {
            double parsed = strtod( s_num_scratch.text, NULL );
            if ( parsed != cur ) { *out = parsed; committed = true; }
            s_num_scratch.id = IMGUI_ID_NONE;
        }
    }
    else
    {
        /* Focus loss: if the scratch was ours, parse and commit. */
        if ( s_num_scratch.id == id )
        {
            double parsed = strtod( s_num_scratch.text, NULL );
            if ( parsed != cur ) { *out = parsed; committed = true; }
            s_num_scratch.id = IMGUI_ID_NONE;
        }
        /* Display the current (or freshly committed) value, clipped to the box. */
        char disp[ NUM_BUF_CAP ];
        num_format( disp, NUM_BUF_CAP, fmt, committed ? *out : cur, is_int );
        draw_push_clip_rect( box_r.x + WIN_BORDER, box_r.y + WIN_BORDER,
                             box_r.w - 2.0f * WIN_BORDER, box_r.h - 2.0f * WIN_BORDER );
        draw_push_text( box_r.x + WIDGET_PAD, text_center_y( box_r.y, box_r.h ),
                        COL_TEXT, disp );
        draw_pop_clip_rect();
    }

    return committed;
}

/* Small framed [-] or [+] button used by the step controls. */
static bool
num_step_button( imgui_id_t id, imgui_rect_t r, bool is_minus )
{
    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    draw_push_rect_filled ( r.x, r.y, r.w, r.h, 0, 0, 1, 1, 0, widget_bg_color( st ) );
    draw_push_rect_outline( r.x, r.y, r.w, r.h, WIN_BORDER, 0, COL_BORDER );
    const char* sym = is_minus ? "-" : "+";
    f32 sw = font_text_w( sym );
    draw_push_text( r.x + ( r.w - sw ) * 0.5f, text_center_y( r.y, r.h ), COL_TEXT, sym );
    return st.clicked;
}

/* Shared label-split + interaction for a single numeric value.
   step == 0 suppresses the [-][+] buttons; step_fast applies when Ctrl is held. */
static bool
input_scalar( const char* label, double cur, double* out,
              double step, double step_fast, const char* fmt, bool is_int )
{
    imgui_id_t   id  = widget_id( label );
    imgui_rect_t r   = widget_next_rect( WIDGET_H );

    bool has_steps = ( step != 0.0 );
    f32  btn_w     = has_steps ? 2.0f * WIDGET_H : 0.0f;
    f32  min_ctrl  = font_char_h() * 3.0f + btn_w;
    imgui_rect_t ctrl = widget_split_label( r, label, min_ctrl, COL_TEXT_DIM );

    imgui_rect_t   box_r = { ctrl.x, ctrl.y, ctrl.w - btn_w, ctrl.h };
    widget_state_t st    = widget_behavior( id, box_r, WIDGET_KIND_FOCUSABLE );

    bool   changed = input_num_field( id, box_r, st, fmt, is_int, cur, out );
    double base    = changed ? *out : cur;

    if ( has_steps )
    {
        bool ctrl_held = io_ctrl();
        double inc = ( ctrl_held && step_fast > 0.0 ) ? step_fast : step;

        f32 bx = ctrl.x + ctrl.w - btn_w;
        imgui_rect_t minus_r = { bx,            ctrl.y, WIDGET_H, ctrl.h };
        imgui_rect_t plus_r  = { bx + WIDGET_H, ctrl.y, WIDGET_H, ctrl.h };

        /* Step buttons call widget_behavior directly (no cell emit), bypassing item_flags_resolve.
           Set BUTTON_REPEAT on cur_item_flags for the pair, then restore so callers are unaffected. */
        imgui_item_flags_t saved = s_build.cur_item_flags;
        s_build.cur_item_flags   = saved | IMGUI_ITEM_BUTTON_REPEAT;
        if ( num_step_button( id_combine( id, 1u ), minus_r, true  ) ) { *out = base - inc; changed = true; }
        if ( num_step_button( id_combine( id, 2u ), plus_r,  false ) ) { *out = base + inc; changed = true; }
        s_build.cur_item_flags   = saved;
    }

    return changed;
}

bool
imgui_input_int( const char* label, i32* v, i32 step, i32 step_fast )
{
    double out;
    bool changed = input_scalar( label, (double)*v, &out,
                                 (double)step, (double)step_fast, "%d", true );
    if ( changed ) *v = (i32)out;
    return changed;
}

bool
imgui_input_float( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt )
{
    if ( !fmt || !fmt[ 0 ] ) fmt = "%.3f";
    double out;
    bool changed = input_scalar( label, (double)*v, &out,
                                 (double)step, (double)step_fast, fmt, false );
    if ( changed ) *v = (f32)out;
    return changed;
}

bool
imgui_input_double( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt )
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
    imgui_id_t   id   = widget_id( label );
    imgui_rect_t r    = widget_next_rect( WIDGET_H );
    imgui_rect_t ctrl = widget_split_label( r, label, font_char_h() * 3.0f * (f32)n, COL_TEXT_DIM );

    bool changed = false;
    for ( u32 i = 0; i < n; ++i )
    {
        f32 x0 = ctrl.x + (f32)i        * ctrl.w / (f32)n;
        f32 x1 = ctrl.x + (f32)(i + 1u) * ctrl.w / (f32)n;
        imgui_rect_t sub  = { floorf( x0 ), ctrl.y, floorf( x1 ) - floorf( x0 ), ctrl.h };
        imgui_id_t   sid  = id_combine( id, i + 1u );
        widget_state_t st = widget_behavior( sid, sub, WIDGET_KIND_FOCUSABLE );

        double out;
        if ( input_num_field( sid, sub, st, fmt, false, (double)v[ i ], &out ) )
        {
            v[ i ]  = (f32)out;
            changed = true;
        }
    }
    return changed;
}

bool imgui_input_float2( const char* label, f32* v, const char* fmt )
{ return input_float_n( label, v, 2u, fmt ); }

bool imgui_input_float3( const char* label, f32* v, const char* fmt )
{ return input_float_n( label, v, 3u, fmt ); }

bool imgui_input_float4( const char* label, f32* v, const char* fmt )
{ return input_float_n( label, v, 4u, fmt ); }

// clang-format on
/*============================================================================================*/
