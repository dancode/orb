/*==============================================================================================

    runtime_service/imgui/imgui_widget.c -- Leaf widgets.

    The everyday controls a caller emits between begin_window / end_window: text, button,
    checkbox, slider, text input, plus the low-level draw_rect / draw_text escape hatches.
    Each takes its rect from widget_next_rect, which carves the next cell out of the active
    region's row template (imgui_widget_core.c) -- a plain vertical stack by default, or any
    multi-column shape set via imgui_layout / the row sugar.  The widget just fills the rect.

    The window itself is a compound widget and lives in imgui_widget_window.c; the shared
    interaction state machine, theme, and layout macros these widgets build on live in
    imgui_widget_core.c.

    Included by imgui.c after imgui_widget_core.c so widget_behavior, widget_next_rect, the
    COL_* palette, and the WIDGET_/WIN_ layout macros are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    text
----------------------------------------------------------------------------------------------*/

void
imgui_text( const char* str )
{
    f32          tw = font_text_w( str );
    imgui_rect_t r  = widget_next_rect_w( tw, font_char_h() );   /* natural width feeds same_line */

    /* Place the run inside its cell per the region's content alignment (default LEFT | TOP, the
       original top-left).  A row tall enough for the glyph centers vertically when asked. */
    imgui_rect_t tr = rect_align( r, tw, font_char_h(), lf()->lay_align );
    draw_push_text( tr.x, tr.y, COL_TEXT, str );
    widget_track_width( tr.x + tw );   /* natural width may exceed the row */
}

/*----------------------------------------------------------------------------------------------
    textf -- printf-style text label (no overloading, so distinct from text())
----------------------------------------------------------------------------------------------*/

void
imgui_textf( const char* fmt, ... )
{
    /* Format into a frame-local buffer; oversized output is truncated, not wrapped. */
    char buf[ 1024 ];

    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    imgui_text( buf );
}

/*----------------------------------------------------------------------------------------------
    bullet_text -- a bullet glyph followed by a text run, the building block of a bulleted list.
    The bullet is a small filled square vertically centered against the glyph line.
----------------------------------------------------------------------------------------------*/

void
imgui_bullet_text( const char* str )
{
    f32 ch  = font_char_h();
    f32 bsz = floorf( ch * 0.35f );  if ( bsz < 2.0f ) bsz = 2.0f;   /* bullet side */
    f32 tw  = font_text_w( str );
    f32 gap = WIDGET_PAD;

    /* Natural width = bullet + gap + text, so a same_line bullet item shrinks to its content. */
    imgui_rect_t r = widget_next_rect_w( bsz + gap + tw, ch );

    /* Bullet square, vertically centered in the row; then the run just past it. */
    imgui_rect_t br = rect_align( r, bsz, bsz, IMGUI_ALIGN_VCENTER );
    draw_push_rect_filled( br.x, br.y, bsz, bsz, 0,0,1,1, 0, COL_TEXT );
    draw_push_text( r.x + bsz + gap, r.y, COL_TEXT, str );
    widget_track_width( r.x + bsz + gap + tw );   /* natural width may exceed the row */
}

/*----------------------------------------------------------------------------------------------
    button -- returns true on the frame the button is released while hovered
----------------------------------------------------------------------------------------------*/

bool
imgui_button( const char* label )
{
    imgui_id_t   id = widget_id( label );

    /* Natural width = label plus breathing room, so a same_line button shrinks to its text. */
    imgui_rect_t r  = widget_next_rect_w( label_width( label ) + 2.0f * WIDGET_PAD, WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Background. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    /* Centered label -- a button always centers, independent of the region's content align.  When
       the label outgrows the button (a squeezed cell), fall back to a left-anchored ellipsized fit
       so it truncates cleanly instead of spilling past both edges. */
    f32 lw    = label_width( label );
    f32 avail = r.w - 2.0f * WIDGET_PAD;
    if ( lw <= avail )
    {
        imgui_rect_t lr = rect_align( r, lw, font_char_h(), IMGUI_ALIGN_CENTER );
        draw_label( lr.x, lr.y, COL_TEXT, label );
    }
    else
    {
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label, avail );
    }

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
imgui_checkbox( const char* label, bool* v )
{
    imgui_id_t   id = widget_id( label );

    /* Natural width = box + gap + label, so a same_line checkbox shrinks to fit. */
    imgui_rect_t r  = widget_next_rect_w( CHECKBOX_SZ + WIDGET_PAD + label_width( label ), WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Field split mode aligns with the other labeled widgets: the label takes its track and the
       box sits at the start of the control track.  Default mode keeps the box on the left with the
       label trailing it.  The box only needs CHECKBOX_SZ of the control track. */
    f32          label_x, label_w;
    imgui_rect_t control;
    f32          bx;
    if ( field_split_resolve( r, CHECKBOX_SZ, &label_x, &label_w, &control ) )
    {
        bx = control.x;
    }
    else
    {
        bx      = r.x;
        label_x = bx + CHECKBOX_SZ + WIDGET_PAD;   /* default: label just right of the box */
        label_w = ( r.x + r.w ) - label_x;         /* trails to the cell's right edge      */
    }

    f32 by = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, IMGUI_ALIGN_VCENTER ).y;
    draw_push_rect_filled( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, 0,0,1,1, 0, COL_WIDGET_BG );
    draw_push_rect_outline( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, WIN_BORDER, 0, COL_BORDER );

    if ( *v )
    {
        /* Check mark: simple smaller filled square. */
        f32 pad = (f32)s_layout.checkmark_pad;
        draw_push_rect_filled( bx + pad, by + pad,
                               CHECKBOX_SZ - 2.0f * pad, CHECKBOX_SZ - 2.0f * pad,
                               0,0,1,1, 0, COL_CHECK_MARK );
    }

    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT, label, label_w );

    bool changed = false;
    if ( st.clicked )
    {
        *v    = !( *v );
        changed = true;
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
    slider_float -- draggable horizontal slider; returns true while dragging
----------------------------------------------------------------------------------------------*/

bool
imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Track takes the left portion; the label sits at the right.  The min track width keeps the
       knob travel usable when the label is long. */
    imgui_rect_t track_r = widget_split_label( r, label, (f32)( s_layout.slider_knob_w * 3u ), COL_TEXT );
    widget_state_t st = widget_behavior( id, track_r, WIDGET_KIND_DRAG );

    /* Drag: update value when active. */
    bool changed = false;
    if ( st.active )
    {
        f32 t = saturate( ( s_io.mouse_x - track_r.x ) / track_r.w );
        f32 nv = lo + t * ( hi - lo );
        if ( nv != *v )
        {
            *v     = nv;
            changed = true;
        }
    }

    /* Draw track. */
    draw_push_rect_filled( track_r.x, track_r.y, track_r.w, track_r.h,
                           0,0,1,1, 0, COL_SLIDER_TRACK );
    draw_push_rect_outline( track_r.x, track_r.y, track_r.w, track_r.h,
                            WIN_BORDER, 0, COL_BORDER );

    /* Draw fill bar up to the current value. */
    f32 t_cur    = ( hi > lo ) ? ( ( *v - lo ) / ( hi - lo ) ) : 0.0f;
    f32 fill_w   = t_cur * ( track_r.w - SLIDER_KNOB_W );
    if ( fill_w > 0.0f )
        draw_push_rect_filled( track_r.x, track_r.y + 1.0f, fill_w, track_r.h - 2.0f,
                               0,0,1,1, 0, COL_WIDGET_FG );

    /* Draw knob. */
    f32 knob_x = track_r.x + t_cur * ( track_r.w - SLIDER_KNOB_W );
    draw_push_rect_filled( knob_x, track_r.y, SLIDER_KNOB_W, track_r.h,
                           0,0,1,1, 0, widget_bg_color( st ) );

    return changed;
}

/*----------------------------------------------------------------------------------------------
    input_text -- single-line text field; returns true when Enter is pressed.

    Delegates all editing logic to input_field_edit (imgui_widget_core.c): cursor movement,
    selection, insertion, deletion, horizontal scroll, and rendering.  This wrapper is
    responsible only for the label split, the box background / border, and the focus claim.
----------------------------------------------------------------------------------------------*/

bool
imgui_input_text( const char* label, char* buf, u32 bufsz )
{
    imgui_id_t   id    = widget_id( label );
    imgui_rect_t r     = widget_next_rect( WIDGET_H );
    imgui_rect_t box_r = widget_split_label( r, label, s_font->char_h * 3.0f, COL_TEXT_DIM );

    widget_state_t st = widget_behavior( id, box_r, WIDGET_KIND_FOCUSABLE );

    draw_push_rect_filled( box_r.x, box_r.y, box_r.w, box_r.h,
                           0, 0, 1, 1, 0,
                           st.focused ? COL_INPUT_FOCUS : COL_INPUT_BG );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h,
                            WIN_BORDER, 0,
                            st.focused ? COL_WIDGET_HOT : COL_BORDER );

    input_field_result_t res = input_field_edit( id, box_r, st, buf, bufsz );
    return res.enter;
}

/*----------------------------------------------------------------------------------------------
    selectable -- a full-width row that highlights on hover and fills when selected.

    The building block for list boxes: emit one per item (typically inside a begin_child
    region so they scroll and clip independently).  When `selected` is non-NULL a click
    toggles it; pass NULL for a click-only row.  Returns true on the frame it is clicked, so
    a caller managing single-selection can set its own index from the return without relying
    on the toggle.
----------------------------------------------------------------------------------------------*/

bool
imgui_selectable( const char* label, bool* selected )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Fill: selected rows use the active tint, a hovered row the hot tint; otherwise the row
       is transparent so the region background shows through. */
    bool on = ( selected && *selected );
    if ( on || st.hover )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0,
                               on ? COL_WIDGET_ACT : COL_WIDGET_HOT );

    /* Label, left-aligned with the standard padding. */
    draw_label( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label );
    widget_track_width( r.x + WIDGET_PAD + label_width( label ) );   /* natural width may exceed the row */

    if ( st.clicked && selected )
        *selected = !( *selected );
    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    collapsing_header -- a full-width clickable bar with a fold arrow that toggles a section open
    or closed, returning the open state.  There is no end call: the caller guards its body with the
    return ( if ( header(...) ) { widgets } ), exactly like begin_window's collapse, so a closed
    header simply skips emitting its contents.  The open flag persists across frames in the keyed
    state pool, keyed by the header id -- the same store windows, tree nodes, and combos use; this
    is the smallest example of it.  Closed by default (zeroed on first sight). */

typedef struct { bool open; } imgui_header_state_t;

bool
imgui_collapsing_header( const char* label )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    imgui_header_state_t* hs = IMGUI_STATE( imgui_header_state_t, id );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    if ( st.clicked ) hs->open = !hs->open;

    /* Clickable bar with hover/active feedback, an arrow box on the left, then the label. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    imgui_rect_t arrow = { r.x, r.y, r.h, r.h };          /* a square the height of the bar */
    draw_collapse_arrow( arrow, !hs->open, COL_TEXT );    /* closed -> points right */
    draw_label( r.x + r.h, text_center_y( r.y, r.h ), COL_TEXT, label );

    return hs->open;
}

/*----------------------------------------------------------------------------------------------
    tree_node / tree_pop -- a collapsing_header without the frame: an arrow + label row that folds
    a nested block and indents it while open.  The unframed sibling of collapsing_header (no filled
    bar; it highlights only on hover, so a tree reads as rows rather than stacked headers) and the
    building block for file explorers / outline views.  Guard the body with the return and, when it
    is true, close it with tree_pop -- which removes exactly the indent the open node added:

        if ( imgui()->tree_node( "Parent" ) )
        {
            imgui()->text( "Child" );
            if ( imgui()->tree_node( "Nested" ) ) { imgui()->text( "Deep" ); imgui()->tree_pop(); }
            imgui()->tree_pop();
        }

    Open state persists per id in the keyed pool, like collapsing_header.  The indent step is one
    row height, so a child's content lines up under the parent label just past the fold arrow. */

bool
imgui_tree_node( const char* label )
{
    imgui_id_t   id = widget_id( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    imgui_header_state_t* hs = IMGUI_STATE( imgui_header_state_t, id );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    if ( st.clicked ) hs->open = !hs->open;

    /* No framed bar: tint only on hover / active (like selectable), so a tree is a list of rows. */
    if ( st.hover || st.active )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    imgui_rect_t arrow = { r.x, r.y, r.h, r.h };          /* fold arrow in a square at the left */
    draw_collapse_arrow( arrow, !hs->open, COL_TEXT );    /* closed -> points right */

    f32 label_x = r.x + r.h;
    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT, label, ( r.x + r.w ) - label_x );
    widget_track_width( label_x + label_width( label ) );   /* natural width may exceed the row */

    /* Indent the body while open; tree_pop removes the matching step.  Done here so children land
       inset the instant the caller starts emitting them under the true return. */
    if ( hs->open )
        imgui_indent( WIDGET_H );

    return hs->open;
}

void
imgui_tree_pop( void )
{
    imgui_unindent( WIDGET_H );
}

/*----------------------------------------------------------------------------------------------
    Spacers -- cell-consuming widgets that emit no interaction.

    Each takes the next cell from the active template exactly like a real widget, so they compose
    with rows and grids the same way: skip() leaves a hole (a blank cell of one standard line, the
    natural way to step over a grid slot), spacing() inserts a blank gap of a chosen height, and
    separator() draws a thin rule centered in its cell.  That these fall out as one-liners on
    widget_next_rect is the point of the cell model -- "advance one slot" needs no special case.
----------------------------------------------------------------------------------------------*/

/* Consume one cell and draw nothing -- a blank slot of one standard line height. */
void
imgui_skip( void )
{
    widget_next_rect( WIDGET_H );
}

/* Consume one cell of height h (<= 0 falls back to the default gap) and draw nothing. */
void
imgui_spacing( f32 h )
{
    widget_next_rect( h > 0.0f ? h : WIDGET_GAP );
}

/* A horizontal rule: a thin line spanning the cell width, centered in a standard-height cell. */
void
imgui_separator( void )
{
    imgui_rect_t r  = widget_next_rect( WIDGET_H );
    imgui_rect_t ln = rect_align( r, r.w, WIN_BORDER, IMGUI_ALIGN_VCENTER );
    draw_push_rect_filled( ln.x, ln.y, ln.w, ln.h, 0,0,1,1, 0, COL_BORDER );
}

/* A labeled rule: a short leading rule, the text, then a rule filling the rest -- "-- Text ----".
   The visible span obeys the "##" / "###" label grammar (markers stripped) like every label. */
void
imgui_separator_text( const char* label )
{
    imgui_rect_t r   = widget_next_rect( WIDGET_H );
    f32          ly  = r.y + r.h * 0.5f;                 /* line centre */
    f32          tw  = label_width( label );
    f32          pre = 2.0f * WIDGET_PAD;                /* short leading rule before the text */

    draw_push_rect_filled( r.x, ly, pre, WIN_BORDER, 0,0,1,1, 0, COL_BORDER );

    f32 tx = r.x + pre + WIDGET_PAD;
    draw_label( tx, text_center_y( r.y, r.h ), COL_TEXT, label );

    f32 rx = tx + tw + WIDGET_PAD;                       /* trailing rule to the right edge */
    f32 rw = ( r.x + r.w ) - rx;
    if ( rw > 0.0f )
        draw_push_rect_filled( rx, ly, rw, WIN_BORDER, 0,0,1,1, 0, COL_BORDER );
}

/*----------------------------------------------------------------------------------------------
    Low-level draw_rect / draw_text
----------------------------------------------------------------------------------------------*/

void
imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

void
imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text( x, y, abgr, str );
}

// clang-format on
/*============================================================================================*/
