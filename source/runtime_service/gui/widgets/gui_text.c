/*==============================================================================================

    runtime_service/gui/widgets/gui_text.c -- Display widgets: text runs, bullets, rules.

    The interaction-less half of the everyday controls: text / text_colored / text_disabled /
    text_wrapped / textf, bullet + bullet_text, label_text (the read-only labeled-value row),
    progress_bar, and the cell-consuming spacers (skip, new_line, separator, separator_text).
    Nothing here runs item_state -- these consume cells and paint, so they compose with
    rows and grids exactly like interactive widgets without ever entering arbitration.

    Press widgets (button, checkbox, radio_button, selectable) are in gui_button.c; folding
    rows (collapsing_header, tree_node) in gui_tree.c; text fields in gui_input.c -- all
    included after this file in gui.c.

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
    gui_rect_t r  = cell_next_w( tw, font_char_h() );   /* natural width feeds same_line */

    /* Place the run inside its cell per the region's content alignment (default LEFT | TOP, the
       original top-left).  A row tall enough for the glyph centers vertically when asked. */

    gui_rect_t tr = rect_align( r, tw, font_char_h(), lf()->mod.align );

    /* When the run fits its cell, draw at the aligned position.  When it overflows, ellipsize to the
       cell width so the widget self-fits regardless of whether a clip rect is active (GUI_WIN_NO_CLIP
       children have no scissor, so the scissor is never the clipping mechanism here). */

    f32 x = ( tw <= r.w ) ? tr.x : r.x;
    if ( tw <= r.w )
        draw_push_text( tr.x, tr.y, col, str );
    else
    {
        draw_set_text_clip_x( r.x, r.x + r.w );
        draw_push_text( r.x, tr.y, col, str );
        draw_clear_text_clip();
    }
    /* Always track the natural text width so content_w reflects the full extent: an autosize
       window needs this to grow wide enough to fit the text, and a scrollable window needs it
       to show a horizontal bar when the text is longer than the view. */

    cell_reach( x + tw );
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
    gui_rect_t r     = cell_next( h );

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
    fmt_vsnprintf( buf, sizeof( buf ), fmt, ap );
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
        draw_fill( ( gui_rect_t ){ br.x, br.y, bsz, bsz }, col );
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
    gui_rect_t r = cell_next_w( bsz + gap + tw, ch );

    /* Bullet mark, vertically centered in the row; then the run just past it.  A disc by default
       (RenderBullet), or a square when GUI_VAR_BULLET_STYLE selects it. */
    gui_rect_t br = rect_align( r, bsz, bsz, GUI_ALIGN_VCENTER );
    bullet_glyph( br, bsz, COL_TEXT );
    draw_push_text( r.x + bsz + gap, r.y, COL_TEXT, str );
    cell_reach( r.x + bsz + gap + tw );   /* natural width may exceed the row */
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

    gui_rect_t r  = cell_next_w( bsz, ch );
    gui_rect_t br = rect_align( r, bsz, bsz, GUI_ALIGN_VCENTER );   /* centered in the row */
    bullet_glyph( br, bsz, COL_TEXT );
    cell_reach( r.x + bsz );
}

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
    gui_rect_t r       = cell_next( WIDGET_H );
    gui_rect_t control = draw_field_label( r, label, 0.0f, COL_TEXT_DIM );

    /* The value is the primary content: draw it where a control would sit, vertically centered and
       fitted (ellipsized) to the track width.  Plain text -- no "##" grammar -- so it shows as-is. */
    draw_text_fit_n( control.x, text_center_y( control.y, control.h ), COL_TEXT,
                     value, 0xFFFFFFFFu, control.w );
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

    gui_rect_t r = cell_next( WIDGET_H );

    /* Track, then the fill bar up to the fraction, then the border on top so the fill stays inside.
       Solid fill by default; a top-to-bottom gradient gloss when GUI_VAR_PROGRESS_STYLE selects it. */
    draw_fill( r, COL_SLIDER_TRACK );
    f32 fw = fraction * r.w;
    if ( fw > 0.0f )
    {
        if ( style_var( GUI_VAR_PROGRESS_STYLE ) >= 0.5f )
            draw_gradient( ( gui_rect_t ){ r.x, r.y, fw, r.h },
                           COL_WIDGET_FG, col_lerp( COL_WIDGET_FG, 0xFFFFFFFFu, 0.45f ), true );
        else
            draw_fill( ( gui_rect_t ){ r.x, r.y, fw, r.h }, COL_WIDGET_FG );
    }
    draw_outline( r, WIN_BORDER, COL_BORDER );

    /* Caption: caller text, or a default percentage; centered and fitted to the inner width. */
    char        buf[ 32 ];
    const char* txt = overlay;
    if ( !txt )
    {
        fmt_snprintf( buf, sizeof( buf ), "%d%%", (int)( fraction * 100.0f + 0.5f ) );
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
    Spacers -- cell-consuming widgets that emit no interaction.

    Each takes the next cell from the active template exactly like a real widget, so they compose
    with rows and grids the same way: skip() leaves a hole (a blank cell of one standard line, the
    natural way to step over a grid slot), spacing() inserts a blank gap of a chosen height, and
    separator() draws a thin rule centered in its cell.  That these fall out as one-liners on
    cell_next is the point of the cell model -- "advance one slot" needs no special case.
----------------------------------------------------------------------------------------------*/

/* Consume one cell and draw nothing -- a blank slot of one standard line height. */
void
gui_skip( void )
{
    cell_next( WIDGET_H );
}

/* Break to a fresh line of height h and draw nothing: undoes a same_line and inserts a blank line
   between runs (the ImGui NewLine, generalized).  A line has no body to measure, so its own
   "natural" size is literally zero: h == 0 is a true zero-height break, not a fallback.  h < 0
   defers to the theme's line height (font_char_h()), the vertical mirror of same_line(-1). */
void
gui_new_line( f32 h )
{
    cell_next( h >= 0.0f ? h : font_char_h() );
}

/* A horizontal rule: a thin line spanning the cell width, centered in a half-height cell -- a
   full line of dead air reads as a section break, not a divider (worst under the roomy ramp).
   Solid by default; a dashed rule when GUI_VAR_SEPARATOR_STYLE selects it (draw_rule). */
void
gui_separator( void )
{
    gui_rect_t r  = cell_next( WIDGET_H * 0.5f );
    gui_rect_t ln = rect_align( r, r.w, WIN_BORDER, GUI_ALIGN_VCENTER );
    draw_rule( ln.x, ln.y + ln.h * 0.5f, ln.w, WIN_BORDER, COL_BORDER );
}

/* A labeled rule: a short leading rule, the text, then a rule filling the rest -- "-- Text ----".
   The visible span obeys the "##" / "###" label grammar (markers stripped) like every label. */
void
gui_separator_text( const char* label )
{
    gui_rect_t r   = cell_next( WIDGET_H );
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

// clang-format on
/*============================================================================================*/
