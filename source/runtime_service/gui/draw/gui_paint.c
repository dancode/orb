/*==============================================================================================

    runtime_service/gui/draw/gui_paint.c -- the rect-taking paint floor + text painters.

    The draw unit's foundation routines: a solid fill and a border outline over a gui_rect_t,
    and the fitted text painters (truncate on a glyph boundary, compact ellipsis).  Pure draw:
    colors arrive as parameters; the only ambient reads are the active font's metrics.  The
    label GRAMMAR (label_vis_len -- where the visible span ends) stays authored in the
    core-side grammar block and is consumed here through its seam.

==============================================================================================*/
// clang-format off

/* Baseline y to vertically center one line of glyphs in a row of height h starting at y.
   Used by every labeled widget and the window title so the centering math lives in one place.
   (The text_center_y( y, h ) form is the VCENTER case of rect_align (rect kit), kept as a
   scalar convenience because most labeled widgets only need the y and already own their x.) */
f32 text_center_y( f32 y, f32 h ) { return y + ( h - font_char_h() ) * 0.5f; }

/* The rect-taking paint floor for the widget tier: a solid fill and a border outline over a
   gui_rect_t.  Widgets speak rects; only the render server's emit layer (draw_push_*) speaks
   scalar x/y/w/h with UV + texture arguments -- these two carry the untextured white-quad
   defaults (uv 0,0,1,1, texture 0) so no widget repeats them.  Shapes beyond fill/outline are
   the symbol palette (gui_symbol.c). */

void draw_fill   ( gui_rect_t r, u32 col )        { draw_push_rect_filled ( r.x, r.y, r.w, r.h, 0.0f, 0.0f, 1.0f, 1.0f, 0, col ); }
void draw_outline( gui_rect_t r, f32 t, u32 col ) { draw_push_rect_outline( r.x, r.y, r.w, r.h, t, 0, col ); }

/* The WIDENED paint floor: fill a rect with a brush instead of a colour.

   draw_fill above is this function's SOLID case and stays the spelling for it -- a widget that
   only ever fills with a colour should not have to say so in a struct.  What the brush adds is
   that "fill this rect" and "with what" are finally two separate questions, so a caller can hand a
   gradient or authored art to code that was written before either existed.  A NULL brush, or an
   unknown kind, falls back to a solid col_a: an unrecognised brush must still paint something,
   because the alternative is a widget that silently vanishes.

   Deliberately thin.  Every branch is one push, no branch inspects style, and nothing here is
   stateful -- the brush arrived as a parameter, exactly like the colour it replaced, so this stays
   inside the draw unit's parameter-purity rule. */
void
draw_fill_brush( gui_rect_t r, const gui_brush_t* b )
{
    if ( !b )
        return;

    switch ( b->kind )
    {
        case GUI_BRUSH_GRADIENT:
            draw_push_rect_gradient( r.x, r.y, r.w, r.h, b->col_a, b->col_b,
                                     ( b->flags & GUI_BRUSH_VERTICAL ) == 0 );
            break;

        case GUI_BRUSH_SPRITE:
        case GUI_BRUSH_NINE:
            draw_push_sprite( r.x, r.y, r.w, r.h, b->sprite, b->col_a, b->scale, b->flags,
                              b->kind == GUI_BRUSH_NINE );
            break;

        case GUI_BRUSH_SOLID:
        default:
            draw_fill( r, b->col_a );
            break;
    }
}

/* Width / draw of a label's visible span (markers stripped; label_vis_len is the grammar's
   seam -- authored core-side so the rule cannot drift between units). */
f32 label_width( const char* s )
{
    return font_text_w_n( s, label_vis_len( s ) );
}

/* draw label convenience wrapper for the text label length */
void draw_label ( f32 x, f32 y, u32 c, const char* s )
{
    draw_push_text_n( x, y, c, s, label_vis_len( s ) );
}

/* Compact truncation ellipsis -- three baseline dots packed into ~1.2 glyph advances instead of
   three full '.' glyph cells.  A literal "..." spends three whole character advances on the cut
   marker, stealing space from the text and forcing the truncation earlier than necessary; these
   filled discs read the same but reserve far less, so more of the string survives.  Sized and
   seated from the active font's glyph box so they track font/scale changes, and rounded like a
   '.' (filled discs, not squares) to blend with the text.  ellipsis_w reports the advance the
   caller must reserve; draw_ellipsis paints it -- kept adjacent so the two never drift. */
static f32
ellipsis_dot_r( void )
{
    f32 r = font_char_h() * 0.065f;             /* dot radius scales with glyph height */
    return r < 0.75f ? 0.75f : r;               /* never sub-pixel -- stay visible at tiny fonts */
}

/* Reserved advance: a leading gap (2r) + three dots on a 3.5r center pitch (7r) = 9r total. */
static f32 ellipsis_w( void ) { return ellipsis_dot_r() * 9.0f; }

static void
draw_ellipsis( f32 x, f32 y, u32 c )
{
    f32 r  = ellipsis_dot_r();
    /* Seat the dot bottom on the baseline (~0.8 of the glyph box; the lower 0.2 is descent space)
       so it rests where a font '.' does, not down in the descender region. */
    f32 cy = y + font_char_h() * 0.8f - r;

    /* Per-dot alpha fade: each subsequent dot is dimmer, so the run trails off rather than
       stopping flat -- it reads as "text continues" the way a fading tail suggests. */
    static const f32 fade[ 3 ] = { 1.0f, 0.7f, 0.45f };
    u32              a0         = ( c >> 24 ) & 0xFFu;     /* source alpha (ABGR high byte) */
    u32              rgb        = c & 0x00FFFFFFu;

    /* Leading gap of 2r separates the dots from the truncated glyph; centers then step by 3.5r
       (a dot diameter plus a gap a touch over its width) so the run breathes like real periods. */
    for ( u32 i = 0; i < 3; ++i )
    {
        u32 a   = (u32)( (f32)a0 * fade[ i ] + 0.5f );
        u32 col = rgb | ( a << 24 );
        draw_push_circle_filled( x + r * 2.0f + (f32)i * r * 3.5f, cy, r, 10u, col );
    }
}

/* Draw at most `len` bytes of s left-anchored at x, fitted into max_w: when the run is wider than
   max_w, truncate on a glyph boundary and mark the cut with a compact ellipsis so a compressed
   widget reads as deliberately clipped rather than bleeding mid-glyph under the region clip.  When
   not even the ellipsis fits, the leading glyphs that do are drawn and the rest dropped -- never
   worse than a hard clip.  max_w <= 0 draws nothing.  Cheap: one width walk, no extra clip command
   (so draw batching is untouched).  draw_label_fit is the label-grammar wrapper; callers with a
   raw string (the window title) pass the whole length through here directly. */
void
draw_text_fit_n( f32 x, f32 y, u32 c, const char* s, u32 len, f32 max_w )
{
    if ( max_w <= 0.0f ) return;

    /* Fits whole -- the common path: draw the span as-is. */
    if ( font_text_w_n( s, len ) <= max_w )
    {
        draw_push_text_n( x, y, c, s, len );
        return;
    }

    /* Too wide: reserve the compact ellipsis (dropped if even it will not fit), then take the
       longest glyph prefix that fits in the remaining budget. */
    f32  ell    = ellipsis_w();
    bool dots   = ( ell <= max_w );
    f32  budget = dots ? max_w - ell : max_w;

    f32 w = 0.0f;
    u32 n = 0;
    while ( n < len && s[ n ] )
    {
        f32 adv = font_char_advance( (u8)s[ n ] );
        if ( w + adv > budget ) break;
        w += adv;
        ++n;
    }

    draw_push_text_n( x, y, c, s, n );
    if ( dots )
        draw_ellipsis( x + w, y, c );
}

/* Clean-shrink companion to draw_label: fit a label's visible span (markers stripped) into max_w,
   ellipsizing when a cell squeezes it below its natural width.  Used by the labeled widgets. */
void
draw_label_fit( f32 x, f32 y, u32 c, const char* s, f32 max_w )
{
    draw_text_fit_n( x, y, c, s, label_vis_len( s ), max_w );
}

// clang-format on
/*============================================================================================*/
