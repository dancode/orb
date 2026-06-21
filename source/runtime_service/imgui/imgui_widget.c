/*==============================================================================================

    runtime_service/imgui/imgui_widget.c -- Core leaf widgets.

    The everyday controls a caller emits between begin_window / end_window: text, button,
    checkbox, radio_button, input_text, label_text, selectable, collapsing_header, tree_node,
    spacers, draw_rect / draw_text escape hatches, and invisible_button.
    Each takes its rect from widget_next_rect, which carves the next cell out of the active
    region's row template (imgui_widget_core.c) -- a plain vertical stack by default, or any
    multi-column shape set via imgui_layout / the row sugar.

    Slider and drag widgets (imgui_slider_float_step, imgui_slider_int, imgui_drag_int) are in
    imgui_widget_slider.c, included just after this file.

    Numeric text inputs (imgui_input_int, imgui_input_float, imgui_input_float2/3/4, etc.) are in
    imgui_widget_numeric.c, also included after this file.

    The window compound widget lives in imgui_widget_window.c; the shared interaction state
    machine, theme, and layout macros these widgets build on live in imgui_widget_core.c.

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
    arrow_button -- a small square button with a directional triangle instead of a text label.

    The non-text button: same interaction and framed background as button(), but it draws an arrow
    pointing `dir` and sizes to a square row-height cell (so a same_line pair sits snug, the spinner
    layout).  Pass a label for the id only -- "##left" / "##right" -- since nothing is displayed.
    Pairs naturally with the IMGUI_ITEM_BUTTON_REPEAT flag for press-and-hold stepping:

        imgui()->push_item_flag( IMGUI_ITEM_BUTTON_REPEAT, true );
        if ( imgui()->arrow_button( "##left",  IMGUI_DIR_LEFT  ) ) counter--;
        imgui()->same_line( spacing );
        if ( imgui()->arrow_button( "##right", IMGUI_DIR_RIGHT ) ) counter++;
        imgui()->pop_item_flag();
----------------------------------------------------------------------------------------------*/

bool
imgui_arrow_button( const char* id_str, imgui_dir_t dir )
{
    imgui_id_t   id = widget_id( id_str );

    /* Square natural size (row height), so a same_line row of arrows packs tightly. */
    imgui_rect_t r  = widget_next_rect_w( WIDGET_H, WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );
    draw_arrow( r, dir, COL_TEXT );

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
    draw_push_rect_filled( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, 0,0,1,1, 0, widget_bg_color( st ) );
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
    radio_button -- one option of a mutually-exclusive set.  `v` holds the selected value and
    `value` is the one this button stands for: the button shows "on" while *v == value, and a click
    sets *v = value.  Emit several against the same v (commonly with same_line between them) to form
    a radio group; returns true only on the frame a click changes the selection.

        static i32 e = 0;
        imgui()->radio_button( "a", &e, 0 ); imgui()->same_line( -1 );
        imgui()->radio_button( "b", &e, 1 ); imgui()->same_line( -1 );
        imgui()->radio_button( "c", &e, 2 );

    The round sibling of checkbox: the same cell split (indicator in the control track, label
    trailing) and the same natural width, but a disc indicator -- a border ring, a hover-tinted
    well, and a filled centre dot when selected -- instead of the square box + check. */

bool
imgui_radio_button( const char* label, i32* v, i32 value )
{
    imgui_id_t   id = widget_id( label );

    /* Natural width = disc + gap + label, so a same_line radio shrinks to fit (a group on one row). */
    imgui_rect_t r  = widget_next_rect_w( CHECKBOX_SZ + WIDGET_PAD + label_width( label ), WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Same label/control split as checkbox: the disc sits at the start of the control track. */
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
        label_x = bx + CHECKBOX_SZ + WIDGET_PAD;   /* default: label just right of the disc */
        label_w = ( r.x + r.w ) - label_x;         /* trails to the cell's right edge        */
    }

    /* Disc centred in a CHECKBOX_SZ box, vertically centred in the row. */
    f32 by  = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, IMGUI_ALIGN_VCENTER ).y;
    f32 cx  = bx + CHECKBOX_SZ * 0.5f;
    f32 cy  = by + CHECKBOX_SZ * 0.5f;
    f32 rad = CHECKBOX_SZ * 0.5f;

    const u32 segs = 16;   /* facets -- round at widget sizes */
    bool      on   = ( v && *v == value );

    /* Border ring, then the well (hover/active tinted like a button knob), then the selected dot. */
    draw_push_circle_filled( cx, cy, rad,              segs, COL_BORDER );
    draw_push_circle_filled( cx, cy, rad - WIN_BORDER, segs, widget_bg_color( st ) );
    if ( on )
        draw_push_circle_filled( cx, cy, rad - (f32)s_layout.checkmark_pad, segs, COL_CHECK_MARK );

    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT, label, label_w );

    bool changed = false;
    if ( st.clicked && v && *v != value )
    {
        *v      = value;
        changed = true;
    }
    return changed;
}

/* Slider, drag, and numeric input widgets are in imgui_widget_slider.c and
   imgui_widget_numeric.c (both included after this file in imgui.c). */

/*----------------------------------------------------------------------------------------------
    input_text -- single-line text field; returns true when Enter is pressed.

    Delegates all editing logic to input_field_edit (imgui_widget_core.c): cursor movement,
    selection, insertion, deletion, horizontal scroll, and rendering.  This wrapper is
    responsible only for the label split, the box background / border, and the focus claim.
----------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------
    input_text / input_text_ex / input_text_with_hint -- single-line text field variants.

    All three share the same layout (label split, one WIDGET_H row) and the same frame draw
    (focused-tinted fill + hot-tinted border).  input_text_begin factors out those shared steps
    so each variant reduces to its one point of difference.
----------------------------------------------------------------------------------------------*/

typedef struct { imgui_id_t id; imgui_rect_t box; widget_state_t st; } input_text_frame_t;

static input_text_frame_t
input_text_begin( const char* label )
{
    imgui_id_t     id    = widget_id( label );
    imgui_rect_t   box_r = widget_split_label( widget_next_rect( WIDGET_H ), label,
                                               font_char_h() * 3.0f, COL_TEXT_DIM );
    widget_state_t st    = widget_behavior( id, box_r, WIDGET_KIND_FOCUSABLE );
    draw_push_rect_filled( box_r.x, box_r.y, box_r.w, box_r.h, 0, 0, 1, 1, 0,
                           st.focused ? COL_INPUT_FOCUS : frame_bg_color( st, COL_INPUT_BG ) );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h, WIN_BORDER, 0,
                            st.focused ? COL_WIDGET_HOT : COL_BORDER );
    return ( input_text_frame_t ){ id, box_r, st };
}

bool
imgui_input_text( const char* label, char* buf, u32 bufsz )
{
    input_text_frame_t f = input_text_begin( label );
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, NULL, NULL ).enter;
}

bool
imgui_input_text_ex( const char* label, char* buf, u32 bufsz,
                     imgui_text_cb_fn on_change, void* cb_user )
{
    input_text_frame_t f = input_text_begin( label );
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, on_change, cb_user ).enter;
}

bool
imgui_input_text_with_hint( const char* label, const char* hint, char* buf, u32 bufsz )
{
    input_text_frame_t f = input_text_begin( label );
    if ( !f.st.focused && buf[ 0 ] == '\0' && hint && hint[ 0 ] )
    {
        draw_push_clip_rect( f.box.x + WIN_BORDER, f.box.y + WIN_BORDER,
                             f.box.w - 2.0f * WIN_BORDER, f.box.h - 2.0f * WIN_BORDER );
        draw_push_text( f.box.x + WIDGET_PAD, text_center_y( f.box.y, f.box.h ),
                        COL_TEXT_DIM, hint );
        draw_pop_clip_rect();
    }
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, NULL, NULL ).enter;
}

/* (Numeric text inputs -- input_int, _float, _double, _float2/3/4 -- live in
   imgui_widget_numeric.c, included after imgui_widget_slider.c.) */

/*----------------------------------------------------------------------------------------------
    label_text -- a read-only "value + label" row, the display sibling of the labeled value widgets.

    Lays out exactly like input_text / slider_float -- the label takes its side of the cell (its
    track under a form / field_split, or trailing on the right by default) and the value sits where
    the control would -- but nothing is interactive: it just presents information that lines up with
    the editable rows around it.  The ImGui LabelText analogue.

        imgui()->form( IMGUI_LABEL_LEFT, 90.0f );
        imgui()->label_text( "Mode",   "Edit" );      // read-only rows...
        imgui()->slider_float( "Gain", &gain, 0, 1 ); // ...aligned with editable ones
----------------------------------------------------------------------------------------------*/

void
imgui_label_text( const char* label, const char* value )
{
    imgui_rect_t r       = widget_next_rect( WIDGET_H );
    imgui_rect_t control = widget_split_label( r, label, 0.0f, COL_TEXT_DIM );

    /* The value is the primary content: draw it where a control would sit, vertically centered and
       fitted (ellipsized) to the track width.  Plain text -- no "##" grammar -- so it shows as-is. */
    draw_text_fit_n( control.x, text_center_y( control.y, control.h ), COL_TEXT,
                     value, 0xFFFFFFFFu, control.w );
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
    if ( on || st.hover || st.nav )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0,
                               on ? COL_WIDGET_ACT : COL_WIDGET_HOT );

    /* Label, left-aligned with the standard padding. */
    draw_label( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label );
    widget_track_width( r.x + WIDGET_PAD + label_width( label ) );   /* natural width may exceed the row */

    if ( st.clicked && selected )
        *selected = !( *selected );

    /* Inside a combo dropdown a clicked row dismisses the combo: flag it for end_combo to close
       (the popup machinery is not in scope here).  Inert for an ordinary list selectable. */
    if ( st.clicked && s_build.combo_open )
        s_build.combo_item_clicked = true;

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

    /* No framed bar: tint only on hover / active / nav (like selectable), so a tree is a list of rows. */
    if ( st.hover || st.active || st.nav )
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

/* Reserve a rectangular drawing area in the layout and hand back its screen rect, for custom
   geometry (draw_line / draw_polyline / draw_rect / draw_text).  Consumes one cell like any
   widget -- full content width, `height` pixels tall (height <= 0 fills the remaining region
   height) -- so it flows in the vertical list and the pen resumes below it.  The returned rect is
   in the same screen space the draw_* calls take, and the enclosing window clips it. */
imgui_rect_t
imgui_canvas( f32 height )
{
    if ( height <= 0.0f )
        height = imgui_content_avail().y;
    return widget_next_rect( height );
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

/*----------------------------------------------------------------------------------------------
    invisible_button -- standard button interaction (hover, press-capture, click) on an explicit rect
    you already hold: a cell cut from a canvas(), a dummy() slot, any custom-drawn region.  Returns
    true on the click frame.  It owns no layout reservation (the rect is the caller's), so it composes
    with the rect helpers: cut/draw the region, then make it clickable.  For just a hover tint use
    is_mouse_hovering_rect; this adds the full press + release-on-target click semantics.
----------------------------------------------------------------------------------------------*/

bool
imgui_invisible_button( const char* id_str, imgui_rect_t r )
{
    imgui_id_t     id = widget_id( id_str );
    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    Text measurement + aligned draw -- the placement primitives for custom drawing.  draw_text gives
    a top-left anchor only; these let a caller size to text and place it within a rect by intent
    (right-align a caption, center a label) instead of hand-computing the anchor -- the arithmetic
    that silently overflows when a constant is wrong.
----------------------------------------------------------------------------------------------*/

/* text_size -- the laid-out pixel size of s as draw_text_in / draw_text render it: width is the
   widest line, height spans the lines ('\n' breaks; one line is char_h, each extra adds a full line
   advance).  The CalcTextSize analogue, for sizing a rect or centering by hand. */
imgui_vec2_t
imgui_text_size( const char* s )
{
    if ( !s ) return ( imgui_vec2_t ){ 0.0f, 0.0f };

    f32         max_w = 0.0f;
    u32         lines = 1;
    const char* line  = s;
    for ( const char* p = s; ; ++p )
    {
        if ( *p == '\n' || *p == '\0' )
        {
            f32 w = font_text_w_n( line, (u32)( p - line ) );
            if ( w > max_w ) max_w = w;
            if ( *p == '\0' ) break;
            ++lines;
            line = p + 1;
        }
    }
    return ( imgui_vec2_t ){ max_w, font_char_h() + (f32)( lines - 1 ) * font_line_h() };
}

/* draw_text_in -- draw s aligned within rect r (imgui_align_t).  Multi-line: the block is placed by
   the vertical flag, each line by the horizontal flag, so RIGHT flushes every line to r's right edge. */
void
imgui_draw_text_in( imgui_rect_t r, imgui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32         y    = align_y( r.y, r.h, imgui_text_size( s ).y, align );
    const char* line = s;
    for ( const char* p = s; ; ++p )
    {
        if ( *p == '\n' || *p == '\0' )
        {
            u32 n = (u32)( p - line );
            draw_push_text_n( align_x( r.x, r.w, font_text_w_n( line, n ), align ), y, col, line, n );
            if ( *p == '\0' ) break;
            y   += font_line_h();
            line = p + 1;
        }
    }
}

/* draw_text_clipped -- single-line draw_text_in that ellipsizes to r's width when s does not fit
   (the fitted run is left-anchored; alignment applies only while it fits). */
void
imgui_draw_text_clipped( imgui_rect_t r, imgui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32 w = font_text_w( s );
    f32 y = align_y( r.y, r.h, font_char_h(), align );
    if ( w <= r.w )
        draw_push_text( align_x( r.x, r.w, w, align ), y, col, s );
    else
        draw_text_fit_n( r.x, y, col, s, 0xFFFFFFFFu, r.w );
}

// clang-format on
/*============================================================================================*/
