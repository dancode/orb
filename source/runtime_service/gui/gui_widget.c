/*==============================================================================================

    runtime_service/gui/gui_widget.c -- Core leaf widgets.

    The everyday controls a caller emits between window_begin / window_end: text, button,
    checkbox, radio_button, input_text, label_text, selectable, collapsing_header, tree_node,
    spacers, draw_rect / draw_text escape hatches, and invisible_button.
    Each takes its rect from widget_next_rect, which carves the next cell out of the active
    region's row template (gui_widget_core.c) -- a plain vertical stack by default, or any
    multi-column shape set via gui_layout / the row sugar.

    Slider and drag widgets (gui_slider_float_step, gui_slider_int, gui_drag_int) are in
    gui_widget_slider.c, included just after this file.

    Numeric text inputs (gui_input_int, gui_input_float, gui_input_float2/3/4, etc.) are in
    gui_widget_numeric.c, also included after this file.

    The window compound widget lives in gui_widget_window.c; the shared interaction state
    machine, theme, and layout macros these widgets build on live in gui_widget_core.c.

    Included by gui.c after gui_widget_core.c so widget_behavior, widget_next_rect, the
    COL_* palette, and the WIDGET_/WIN_ layout macros are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    text
----------------------------------------------------------------------------------------------*/

/* Shared text-run emit: reserve a natural-width cell, place the run by the region's content
   alignment, and draw it in `col`.  text / text_colored / text_disabled differ only by colour. */
static void
text_emit( u32 col, const char* str )
{
    f32          tw = font_text_w( str );
    gui_rect_t r  = widget_next_rect_w( tw, font_char_h() );   /* natural width feeds same_line */

    /* Place the run inside its cell per the region's content alignment (default LEFT | TOP, the
       original top-left).  A row tall enough for the glyph centers vertically when asked. */
    gui_rect_t tr = rect_align( r, tw, font_char_h(), lf()->lay_align );

    /* Draw the run plainly -- no ellipsis.  When it fits the cell (the common case, e.g. a stack's
       full-width row) this is an exact draw; when a narrow cell squeezes it the run overflows and is
       bounded by the window's clip rect rather than ellipsized -- matching the input / display
       widgets, which rely on the window border instead of a per-widget fit. */
    f32 x = ( tw <= r.w ) ? tr.x : r.x;
    draw_push_text( x, tr.y, col, str );
    /* Always track the natural text width so content_w reflects the full extent: an autosize
       window needs this to grow wide enough to fit the text, and a scrollable window needs it
       to show a horizontal bar when the text is longer than the view. */
    widget_track_width( x + tw );
}

void gui_text( const char* str ) { text_emit( COL_TEXT, str ); }

/* text_colored -- a text run in an explicit colour (GUI_COLOR abgr), the ImGui TextColored
   analogue.  text_disabled is the dim-text shorthand (COL_TEXT_DIM) for secondary / inert labels. */
void gui_text_colored ( u32 abgr, const char* str ) { text_emit( abgr,         str ); }
void gui_text_disabled( const char* str )           { text_emit( COL_TEXT_DIM, str ); }

/*----------------------------------------------------------------------------------------------
    text_wrapped -- a text run word-wrapped to the region's content width (the ImGui TextWrapped
    analogue), for paragraphs / help blurbs that should reflow instead of clipping or overflowing.
    Breaks on spaces (a word longer than the line hard-breaks before it) and honours explicit '\n'.
----------------------------------------------------------------------------------------------*/

/* Walk s word-wrapped to max_w.  When draw, render each line left-anchored at (x, y0 + i*line_h);
   either way return the line count, so a measure pass can size the cell before the draw pass. */
static u32
text_wrap_walk( const char* s, f32 max_w, bool draw, f32 x, f32 y0, u32 col )
{
    f32         lh    = font_line_h();
    u32         lines = 0;
    const char* p     = s;

    while ( *p )
    {
        const char* line_beg = p;
        const char* brk      = NULL;   /* last space seen on this line -- the break candidate */
        f32         w        = 0.0f;

        while ( *p && *p != '\n' )
        {
            f32 adv = font_char_advance( (u8)*p );
            if ( *p == ' ' ) brk = p;                         /* a space is where we may wrap */
            if ( w + adv > max_w && p != line_beg )
            {
                if ( brk ) p = brk;                           /* break at the last space */
                break;                                        /* (long word: hard break here) */
            }
            w += adv;
            ++p;
        }

        if ( draw )
            draw_push_text_n( x, y0 + (f32)lines * lh, col, line_beg, (u32)( p - line_beg ) );
        ++lines;

        if      ( *p == '\n' )               ++p;   /* consume the explicit break  */
        else if ( *p == ' ' && brk == p )    ++p;   /* consume the wrap-point space */
    }

    return lines ? lines : 1u;                       /* empty string still owns one line */
}

void
gui_text_wrapped( const char* str )
{
    if ( !str ) return;

    f32 avail = gui_content_avail().x;             /* width a full cell would fill */
    if ( avail < 1.0f ) avail = 1.0f;

    u32          lines = text_wrap_walk( str, avail, false, 0.0f, 0.0f, 0 );
    f32          h     = font_char_h() + (f32)( lines - 1u ) * font_line_h();
    gui_rect_t r     = widget_next_rect( h );

    text_wrap_walk( str, avail, true, r.x, r.y, COL_TEXT );
}

/*----------------------------------------------------------------------------------------------
    textf -- printf-style text label (no overloading, so distinct from text())
----------------------------------------------------------------------------------------------*/

void
gui_textf( const char* fmt, ... )
{
    /* Format into a frame-local buffer; oversized output is truncated, not wrapped. */
    char buf[ 1024 ];

    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    gui_text( buf );
}

/*----------------------------------------------------------------------------------------------
    bullet_glyph -- the shared mark for bullet / bullet_text: a filled disc (RenderBullet) by
    default, or a square when GUI_VAR_BULLET_STYLE is set.  `br` is the bsz x bsz cell already
    placed in the row; the square draws with rounding forced off so the frame radius cannot bend a
    tiny mark into a dot.
----------------------------------------------------------------------------------------------*/

static void
bullet_glyph( gui_rect_t br, f32 bsz, u32 col )
{
    if ( style_var( GUI_VAR_BULLET_STYLE ) >= 0.5f )
    {
        f32 save_round = draw_rounding();
        draw_set_rounding( 0.0f );
        draw_push_rect_filled( br.x, br.y, bsz, bsz, 0,0,1,1, 0, col );
        draw_set_rounding( save_round );
    }
    else
    {
        draw_bullet( br.x + bsz * 0.5f, br.y + bsz * 0.5f, bsz * 0.5f, col );
    }
}

/*----------------------------------------------------------------------------------------------
    bullet_text -- a bullet glyph followed by a text run, the building block of a bulleted list.
    The bullet is a small mark (disc / square) vertically centered against the glyph line.
----------------------------------------------------------------------------------------------*/

void
gui_bullet_text( const char* str )
{
    f32 ch  = font_char_h();
    f32 bsz = floorf( ch * 0.35f );  if ( bsz < 2.0f ) bsz = 2.0f;   /* bullet side */
    f32 tw  = font_text_w( str );
    f32 gap = WIDGET_PAD;

    /* Natural width = bullet + gap + text, so a same_line bullet item shrinks to its content. */
    gui_rect_t r = widget_next_rect_w( bsz + gap + tw, ch );

    /* Bullet mark, vertically centered in the row; then the run just past it.  A disc by default
       (RenderBullet), or a square when GUI_VAR_BULLET_STYLE selects it. */
    gui_rect_t br = rect_align( r, bsz, bsz, GUI_ALIGN_VCENTER );
    bullet_glyph( br, bsz, COL_TEXT );
    draw_push_text( r.x + bsz + gap, r.y, COL_TEXT, str );
    widget_track_width( r.x + bsz + gap + tw );   /* natural width may exceed the row */
}

/*----------------------------------------------------------------------------------------------
    bullet -- a standalone bullet glyph (the ImGui Bullet analogue): the bullet of bullet_text with
    no trailing text, so a caller can follow it on the same line with any widget(s).

        gui()->bullet();  gui()->same_line( 0.0f );  gui()->button( "Action" );
----------------------------------------------------------------------------------------------*/

void
gui_bullet( void )
{
    f32 ch  = font_char_h();
    f32 bsz = floorf( ch * 0.35f );  if ( bsz < 2.0f ) bsz = 2.0f;   /* bullet side */

    gui_rect_t r  = widget_next_rect_w( bsz, ch );
    gui_rect_t br = rect_align( r, bsz, bsz, GUI_ALIGN_VCENTER );   /* centered in the row */
    bullet_glyph( br, bsz, COL_TEXT );
    widget_track_width( r.x + bsz );
}

/*----------------------------------------------------------------------------------------------
    button -- returns true on the frame the button is released while hovered
----------------------------------------------------------------------------------------------*/

bool
gui_button( const char* label )
{
    gui_id_t   id = widget_id( label );

    /* Natural width = label + padding.  Shrinks to this in stack and same_line; fills in columns. */
    gui_rect_t r  = widget_next_rect_w( label_width( label ) + 2.0f * WIDGET_PAD, WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, gui_anim_bg( id, st ) );

    /* Centered label -- a button always centers, independent of the region's content align.  When
       the label outgrows the button (a squeezed cell), fall back to a left-anchored ellipsized fit
       so it truncates cleanly instead of spilling past both edges. */

    f32 lw = label_width( label );
    f32 avail = r.w - 2.0f * WIDGET_PAD;
    if ( lw <= avail )
    {
        gui_rect_t lr = rect_align( r, lw, font_char_h(), GUI_ALIGN_CENTER );
        draw_label( lr.x, lr.y, COL_TEXT, label );
    }
    else
    {
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label, avail );
    }

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    button_fill -- a button that fills the remaining height of its containing region.

    Identical to button() in every respect except the height comes from content_avail().y
    instead of the fixed WIDGET_H.  Intended for use inside a split panel where you want the
    button to match the height of the adjacent panel's content.
----------------------------------------------------------------------------------------------*/

bool
gui_button_fill( const char* label )
{
    gui_id_t id = widget_id( label );

    f32 avh = gui_content_avail().y;
    if ( avh < WIDGET_H ) avh = WIDGET_H;

    gui_rect_t r = widget_next_rect( avh );   /* fill the cell; height from content_avail */

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, gui_anim_bg( id, st ) );

    f32 lw    = label_width( label );
    f32 avail = r.w - 2.0f * WIDGET_PAD;
    if ( lw <= avail )
    {
        gui_rect_t lr = rect_align( r, lw, font_char_h(), GUI_ALIGN_CENTER );
        draw_label( lr.x, lr.y, COL_TEXT, label );
    }
    else
    {
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label, avail );
    }

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    small_button -- a compact button with no vertical frame padding (the ImGui SmallButton): a
    text-height row instead of the full WIDGET_H, for inline controls packed onto a text line.
----------------------------------------------------------------------------------------------*/

bool
gui_small_button( const char* label )
{
    gui_id_t   id = widget_id( label );

    /* Height hugs the glyph (plus 2px so the frame does not touch the text); width is label + pad. */
    f32          h  = font_char_h() + 2.0f;
    gui_rect_t r  = widget_next_rect_w( label_width( label ) + 2.0f * WIDGET_PAD, h );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    f32 lw = label_width( label );
    f32 avail = r.w - 2.0f * WIDGET_PAD;
    if ( lw <= avail )
    {
        gui_rect_t lr = rect_align( r, lw, font_char_h(), GUI_ALIGN_CENTER );
        draw_label( lr.x, lr.y, COL_TEXT, label );
    }
    else
    {
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label, avail );
    }

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    progress_bar -- a filled track showing `fraction` (0..1) of completion with a centered caption
    (the ImGui ProgressBar analogue).  overlay is the text drawn over the bar; NULL shows a "NN%"
    percentage, an empty string shows nothing.  Consumes one standard-height full-width cell.
----------------------------------------------------------------------------------------------*/

void
gui_progress_bar( f32 fraction, const char* overlay )
{
    fraction = saturate( fraction );

    gui_rect_t r = widget_next_rect( WIDGET_H );

    /* Track, then the fill bar up to the fraction, then the border on top so the fill stays inside.
       Solid fill by default; a top-to-bottom gradient gloss when GUI_VAR_PROGRESS_STYLE selects it. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, COL_SLIDER_TRACK );
    f32 fw = fraction * r.w;
    if ( fw > 0.0f )
    {
        if ( style_var( GUI_VAR_PROGRESS_STYLE ) >= 0.5f )
            draw_gradient( ( gui_rect_t ){ r.x, r.y, fw, r.h },
                           COL_WIDGET_FG, col_lerp( COL_WIDGET_FG, 0xFFFFFFFFu, 0.45f ), true );
        else
            draw_push_rect_filled( r.x, r.y, fw, r.h, 0,0,1,1, 0, COL_WIDGET_FG );
    }
    draw_push_rect_outline( r.x, r.y, r.w, r.h, WIN_BORDER, 0, COL_BORDER );

    /* Caption: caller text, or a default percentage; centered and fitted to the inner width. */
    char        buf[ 32 ];
    const char* txt = overlay;
    if ( !txt )
    {
        snprintf( buf, sizeof( buf ), "%d%%", (int)( fraction * 100.0f + 0.5f ) );
        txt = buf;
    }
    if ( txt[ 0 ] )
    {
        f32 tw = font_text_w( txt );
        f32 tx = r.x + ( r.w - tw ) * 0.5f;
        if ( tx < r.x + WIDGET_PAD ) tx = r.x + WIDGET_PAD;
        draw_text_fit_n( tx, text_center_y( r.y, r.h ), COL_TEXT, txt, 0xFFFFFFFFu,
                         r.w - 2.0f * WIDGET_PAD );
    }
}

/*----------------------------------------------------------------------------------------------
    arrow_button -- a small square button with a directional triangle instead of a text label.

    The non-text button: same interaction and framed background as button(), but it draws an arrow
    pointing `dir` and sizes to a square row-height cell (so a same_line pair sits snug, the spinner
    layout).  Pass a label for the id only -- "##left" / "##right" -- since nothing is displayed.
    Pairs naturally with the GUI_ITEM_BUTTON_REPEAT flag for press-and-hold stepping:

        gui()->push_item_flag( GUI_ITEM_BUTTON_REPEAT, true );
        if ( gui()->arrow_button( "##left",  GUI_DIR_LEFT  ) ) counter--;
        gui()->same_line( spacing );
        if ( gui()->arrow_button( "##right", GUI_DIR_RIGHT ) ) counter++;
        gui()->pop_item_flag();
----------------------------------------------------------------------------------------------*/

bool
gui_arrow_button( const char* id_str, gui_dir_t dir )
{
    gui_id_t   id = widget_id( id_str );

    /* Square natural size (row height), so a same_line row of arrows packs tightly. */
    gui_rect_t r  = widget_next_rect_w( WIDGET_H, WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );
    draw_arrow( r, dir, COL_TEXT );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
gui_checkbox( const char* label, bool* v )
{
    gui_id_t   id = widget_id( label );

    /* Natural width = box + gap + label, so a same_line checkbox shrinks to fit. */
    gui_rect_t r  = widget_next_rect_w( CHECKBOX_SZ + WIDGET_PAD + label_width( label ), WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Field split mode aligns with the other labeled widgets: the label takes its track and the
       box sits at the start of the control track.  Default mode keeps the box on the left with the
       label trailing it.  The box only needs CHECKBOX_SZ of the control track. */

    f32         label_x, label_w;
    gui_rect_t  control;
    f32         bx;

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

    f32 by = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, GUI_ALIGN_VCENTER ).y;
    draw_push_rect_filled( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, 0,0,1,1, 0, widget_bg_color( st ) );
    draw_push_rect_outline( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, WIN_BORDER, 0, COL_BORDER );

    if ( *v )
    {
        /* Indicator: a 'v' tick (default), a filled disc, or an 'X' cross per GUI_VAR_CHECK_STYLE. */
        draw_check_indicator( ( gui_rect_t ){ bx, by, CHECKBOX_SZ, CHECKBOX_SZ }, COL_CHECK_MARK );
    }

    /* Draw the label plainly -- no ellipsis (markers still stripped); a label too wide for its
       track overflows and is bounded by the window clip, matching text() and the input widgets. */
    (void)label_w;
    draw_label( label_x, text_center_y( r.y, r.h ), COL_TEXT, label );

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
        gui()->radio_button( "a", &e, 0 ); gui()->same_line( -1 );
        gui()->radio_button( "b", &e, 1 ); gui()->same_line( -1 );
        gui()->radio_button( "c", &e, 2 );

    The round sibling of checkbox: the same cell split (indicator in the control track, label
    trailing) and the same natural width, but a disc indicator -- a border ring, a hover-tinted
    well, and a filled centre dot when selected -- instead of the square box + check. */

bool
gui_radio_button( const char* label, i32* v, i32 value )
{
    gui_id_t   id = widget_id( label );

    /* Natural width = disc + gap + label, so a same_line radio shrinks to fit (a group on one row). */
    gui_rect_t r  = widget_next_rect_w( CHECKBOX_SZ + WIDGET_PAD + label_width( label ), WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Same label/control split as checkbox: the disc sits at the start of the control track. */
    f32          label_x, label_w;
    gui_rect_t control;
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
    f32 by  = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, GUI_ALIGN_VCENTER ).y;
    f32 cx  = bx + CHECKBOX_SZ * 0.5f;
    f32 cy  = by + CHECKBOX_SZ * 0.5f;
    f32 rad = CHECKBOX_SZ * 0.5f;

    const u32 segs = 16;   /* facets -- round at widget sizes */
    bool      on   = ( v && *v == value );

    /* Border ring, then the well (hover/active tinted like a button knob), then the selected dot. */
    draw_push_circle_filled( cx, cy, rad,              segs, COL_BORDER );
    draw_push_circle_filled( cx, cy, rad - WIN_BORDER, segs, widget_bg_color( st ) );
    if ( on )
        draw_push_circle_filled( cx, cy, rad - (f32)s_style.checkmark_pad, segs, COL_CHECK_MARK );

    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT, label, label_w );

    bool changed = false;
    if ( st.clicked && v && *v != value )
    {
        *v      = value;
        changed = true;
    }
    return changed;
}

/* Slider, drag, and numeric input widgets are in gui_widget_slider.c and
   gui_widget_numeric.c (both included after this file in gui.c). */

/*----------------------------------------------------------------------------------------------
    input_text / input_text_ex / input_text_with_hint -- single-line text field variants.

    All three share the same layout (label split, one WIDGET_H row) and the same frame draw
    (focused-tinted fill + hot-tinted border).  input_text_begin factors out those shared steps
    so each variant reduces to its one point of difference.  All editing logic (cursor movement,
    selection, insertion, deletion, horizontal scroll, rendering) delegates to input_field_edit
    (gui_text_edit.c); the wrapper handles only the label split, box background, border, and
    focus claim.
----------------------------------------------------------------------------------------------*/

typedef struct { gui_id_t id; gui_rect_t box; widget_state_t st; } input_text_frame_t;

static input_text_frame_t
input_text_begin( const char* label )
{
    gui_id_t     id    = widget_id( label );
    gui_rect_t   box_r = widget_split_label( widget_next_rect( WIDGET_H ), label,
                                               font_char_h() * 3.0f, COL_TEXT_DIM );
    widget_state_t st    = widget_behavior( id, box_r, WIDGET_KIND_FOCUSABLE );
    draw_push_rect_filled( box_r.x, box_r.y, box_r.w, box_r.h, 0, 0, 1, 1, 0,
                           st.focused ? COL_INPUT_FOCUS : frame_bg_color( st, COL_INPUT_BG ) );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h, WIN_BORDER, 0,
                            st.focused ? COL_WIDGET_HOT : COL_BORDER );
    return ( input_text_frame_t ){ id, box_r, st };
}

bool
gui_input_text( const char* label, char* buf, u32 bufsz )
{
    input_text_frame_t f = input_text_begin( label );
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, NULL, NULL ).enter;
}

bool
gui_input_text_ex( const char* label, char* buf, u32 bufsz,
                     gui_text_cb_fn on_change, void* cb_user )
{
    input_text_frame_t f = input_text_begin( label );
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, on_change, cb_user ).enter;
}

bool
gui_input_text_with_hint( const char* label, const char* hint, char* buf, u32 bufsz )
{
    input_text_frame_t f = input_text_begin( label );
    if ( !f.st.focused && buf[ 0 ] == '\0' && hint && hint[ 0 ] )
    {
        /* No per-widget clip: the hint fits the box in the common case, and the window's clip rect
           already bounds any overflow -- so no scissor (no batch split) and no ellipsis. */
        draw_push_text( f.box.x + WIDGET_PAD, text_center_y( f.box.y, f.box.h ),
                        COL_TEXT_DIM, hint );
    }
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, NULL, NULL ).enter;
}

/* (Numeric text inputs -- input_int, _float, _double, _float2/3/4 -- live in
   gui_widget_numeric.c, included after gui_widget_slider.c.) */

/*----------------------------------------------------------------------------------------------
    label_text -- a read-only "value + label" row, the display sibling of the labeled value widgets.

    Lays out exactly like input_text / slider_float -- the label takes its side of the cell (its
    track under a form / field_split, or trailing on the right by default) and the value sits where
    the control would -- but nothing is interactive: it just presents information that lines up with
    the editable rows around it.  The ImGui LabelText analogue.

        gui()->form( GUI_LABEL_LEFT, 90.0f );
        gui()->label_text( "Mode",   "Edit" );      // read-only rows...
        gui()->slider_float( "Gain", &gain, 0, 1 ); // ...aligned with editable ones
----------------------------------------------------------------------------------------------*/

void
gui_label_text( const char* label, const char* value )
{
    gui_rect_t r       = widget_next_rect( WIDGET_H );
    gui_rect_t control = widget_split_label( r, label, 0.0f, COL_TEXT_DIM );

    /* The value is the primary content: draw it where a control would sit, vertically centered and
       fitted (ellipsized) to the track width.  Plain text -- no "##" grammar -- so it shows as-is. */
    draw_text_fit_n( control.x, text_center_y( control.y, control.h ), COL_TEXT,
                     value, 0xFFFFFFFFu, control.w );
}

/*----------------------------------------------------------------------------------------------
    selectable -- a full-width row that highlights on hover and fills when selected.

    The building block for list boxes: emit one per item (typically inside a child_begin
    region so they scroll and clip independently).  When `selected` is non-NULL a click
    toggles it; pass NULL for a click-only row.  Returns true on the frame it is clicked, so
    a caller managing single-selection can set its own index from the return without relying
    on the toggle.
----------------------------------------------------------------------------------------------*/

bool
gui_selectable( const char* label, bool* selected )
{
    gui_id_t   id = widget_id( label );
    gui_rect_t r  = widget_next_rect( WIDGET_H );

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

    /* Inside a combo dropdown a clicked row dismisses the combo: flag it for combo_end to close
       (the popup machinery is not in scope here).  Inert for an ordinary list selectable. */
    if ( st.clicked && s_build.combo_open )
        s_build.combo_item_clicked = true;

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    collapsing_header -- a full-width clickable bar with a fold arrow that toggles a section open
    or closed, returning the open state.  There is no end call: the caller guards its body with the
    return ( if ( header(...) ) { widgets } ), exactly like window_begin's collapse, so a closed
    header simply skips emitting its contents.  The open flag persists across frames in the keyed
    state pool, keyed by the header id -- the same store windows, tree nodes, and combos use; this
    is the smallest example of it.  Closed by default (zeroed on first sight). */

typedef struct { bool open; } gui_header_state_t;

bool
gui_collapsing_header( const char* label )
{
    gui_id_t   id = widget_id( label );
    gui_rect_t r  = widget_next_rect( WIDGET_H );

    gui_header_state_t* hs = GUI_STATE( gui_header_state_t, id );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    if ( st.clicked ) hs->open = !hs->open;

    /* Clickable bar with hover/active feedback, an arrow box on the left, then the label. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    gui_rect_t arrow = { r.x, r.y, r.h, r.h };          /* a square the height of the bar */
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

        if ( gui()->tree_node( "Parent" ) )
        {
            gui()->text( "Child" );
            if ( gui()->tree_node( "Nested" ) ) { gui()->text( "Deep" ); gui()->tree_pop(); }
            gui()->tree_pop();
        }

    Open state persists per id in the keyed pool, like collapsing_header.  The indent step is one
    row height, so a child's content lines up under the parent label just past the fold arrow. */

bool
gui_tree_node( const char* label )
{
    gui_id_t   id = widget_id( label );
    gui_rect_t r  = widget_next_rect( WIDGET_H );

    gui_header_state_t* hs = GUI_STATE( gui_header_state_t, id );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );
    if ( st.clicked ) hs->open = !hs->open;

    /* No framed bar: tint only on hover / active / nav (like selectable), so a tree is a list of rows. */
    if ( st.hover || st.active || st.nav )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    gui_rect_t arrow = { r.x, r.y, r.h, r.h };          /* fold arrow in a square at the left */
    draw_collapse_arrow( arrow, !hs->open, COL_TEXT );    /* closed -> points right */

    f32 label_x = r.x + r.h;
    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT, label, ( r.x + r.w ) - label_x );
    widget_track_width( label_x + label_width( label ) );   /* natural width may exceed the row */

    /* Indent the body while open; tree_pop removes the matching step.  Done here so children land
       inset the instant the caller starts emitting them under the true return. */
    if ( hs->open )
        gui_indent( WIDGET_H );

    return hs->open;
}

void
gui_tree_pop( void )
{
    gui_unindent( WIDGET_H );
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
gui_skip( void )
{
    widget_next_rect( WIDGET_H );
}

/* Consume one cell of height h (<= 0 falls back to the default gap) and draw nothing. */
void
gui_spacing( f32 h )
{
    widget_next_rect( h > 0.0f ? h : WIDGET_GAP );
}

/* Break to a fresh line of one text-height (the ImGui NewLine): undoes a same_line and inserts a
   blank line between runs.  A spacing() sized to the glyph height, named for its intent. */
void
gui_new_line( void )
{
    widget_next_rect( font_char_h() );
}

/* Reserve a rectangular drawing area in the layout and hand back its screen rect, for custom
   geometry (draw_line / draw_polyline / draw_rect / draw_text).  Consumes one cell like any
   widget -- full content width, `height` pixels tall (height <= 0 fills the remaining region
   height) -- so it flows in the vertical list and the pen resumes below it.  The returned rect is
   in the same screen space the draw_* calls take, and the enclosing window clips it. */
gui_rect_t
gui_canvas( f32 height )
{
    if ( height <= 0.0f )
        height = gui_content_avail().y;
    return widget_next_rect( height );
}

/* A horizontal rule: a thin line spanning the cell width, centered in a standard-height cell.
   Solid by default; a dashed rule when GUI_VAR_SEPARATOR_STYLE selects it (draw_rule). */
void
gui_separator( void )
{
    gui_rect_t r  = widget_next_rect( WIDGET_H );
    gui_rect_t ln = rect_align( r, r.w, WIN_BORDER, GUI_ALIGN_VCENTER );
    draw_rule( ln.x, ln.y + ln.h * 0.5f, ln.w, WIN_BORDER, COL_BORDER );
}

/* A labeled rule: a short leading rule, the text, then a rule filling the rest -- "-- Text ----".
   The visible span obeys the "##" / "###" label grammar (markers stripped) like every label. */
void
gui_separator_text( const char* label )
{
    gui_rect_t r   = widget_next_rect( WIDGET_H );
    f32          ly  = r.y + r.h * 0.5f;                 /* line centre */
    f32          tw  = label_width( label );
    f32          pre = 2.0f * WIDGET_PAD;                /* short leading rule before the text */

    draw_rule( r.x, ly, pre, WIN_BORDER, COL_BORDER );

    f32 tx = r.x + pre + WIDGET_PAD;
    draw_label( tx, text_center_y( r.y, r.h ), COL_TEXT, label );

    f32 rx = tx + tw + WIDGET_PAD;                       /* trailing rule to the right edge */
    f32 rw = ( r.x + r.w ) - rx;
    draw_rule( rx, ly, rw, WIN_BORDER, COL_BORDER );     /* draw_rule no-ops on rw <= 0 */
}

/*----------------------------------------------------------------------------------------------
    Low-level draw_rect / draw_text
----------------------------------------------------------------------------------------------*/

void
gui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

void
gui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
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
gui_invisible_button( const char* id_str, gui_rect_t r )
{
    gui_id_t     id = widget_id( id_str );
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
gui_vec2_t
gui_text_size( const char* s )
{
    if ( !s ) return ( gui_vec2_t ){ 0.0f, 0.0f };

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
    return ( gui_vec2_t ){ max_w, font_char_h() + (f32)( lines - 1 ) * font_line_h() };
}

/* text_h -- the laid-out pixel height of s (text_size( s ).y); the height twin of text_w, for sizing
   a row to a (possibly multi-line) caption without taking the whole vec2. */
f32 gui_text_h( const char* s ) { return gui_text_size( s ).y; }

/* draw_text_in -- draw s aligned within rect r (gui_align_t).  Multi-line: the block is placed by
   the vertical flag, each line by the horizontal flag, so RIGHT flushes every line to r's right edge. */
void
gui_draw_text_in( gui_rect_t r, gui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32         y    = align_y( r.y, r.h, gui_text_size( s ).y, align );
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
gui_draw_text_clipped( gui_rect_t r, gui_align_t align, u32 col, const char* s )
{
    if ( !s ) return;

    f32 w = font_text_w( s );
    f32 y = align_y( r.y, r.h, font_char_h(), align );
    if ( w <= r.w )
        draw_push_text( align_x( r.x, r.w, w, align ), y, col, s );
    else
        draw_text_fit_n( r.x, y, col, s, 0xFFFFFFFFu, r.w );
}

/*----------------------------------------------------------------------------------------------
    Icons -- thin public surface over the runtime icon atlas (gui_icon.c, backend unit).

    register_icon / find_icon / icon_size pass straight through; image is a layout widget that
    reserves a box and fills it; draw_icon_in is the custom-draw placement primitive (the icon
    analogue of draw_text_in) for a rect the caller already holds -- a table cell, a button label,
    a canvas cut.  Both draw helpers aspect-fit the icon centered in the rect so a non-square box
    never stretches the art, and default a 0 color to opaque white (icons are usually drawn plain).
----------------------------------------------------------------------------------------------*/

gui_icon_id_t
gui_register_icon( const char* name, u32 w, u32 h, const u8* coverage )
{
    return icon_register( name, w, h, coverage );
}

gui_icon_id_t
gui_find_icon( const char* name )
{
    return icon_find( name );
}

gui_vec2_t
gui_icon_size( gui_icon_id_t id )
{
    u32 w = 0, h = 0;
    icon_get( id, NULL, NULL, NULL, NULL, &w, &h );
    return ( gui_vec2_t ){ (f32)w, (f32)h };
}

void
gui_draw_icon_in( gui_rect_t r, gui_icon_id_t id, u32 col )
{
    u32 iw = 0, ih = 0;
    if ( !icon_get( id, NULL, NULL, NULL, NULL, &iw, &ih ) || iw == 0 || ih == 0 )
        return;

    /* Aspect-fit: scale to the tighter of the two axes, then center the fitted box in r. */
    f32 sx  = r.w / (f32)iw;
    f32 sy  = r.h / (f32)ih;
    f32 s   = sx < sy ? sx : sy;
    f32 w   = (f32)iw * s;
    f32 h   = (f32)ih * s;
    gui_rect_t box = rect_align( r, w, h, GUI_ALIGN_HCENTER | GUI_ALIGN_VCENTER );

    draw_push_icon( box.x, box.y, box.w, box.h, id, col ? col : 0xFFFFFFFFu );
}

void
gui_image( gui_icon_id_t id, f32 w, f32 h, u32 col )
{
    gui_rect_t r = widget_next_rect_w( w, h );   /* reserve a w x h layout slot (like dummy) */
    gui_draw_icon_in( r, id, col );
}

/* The public gui_render_* symbol surface (draw_check_mark / draw_arrow / draw_frame /
   draw_round_rect / ... and the set_*_style setters) lives in gui_symbol.c, beside the
   draw_* helpers it wraps. */

// clang-format on
/*============================================================================================*/
