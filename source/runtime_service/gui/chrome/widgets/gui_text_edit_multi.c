/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_text_edit_multi.c -- Multi-line text editor widget.

    input_text_multiline (the Dear ImGui InputTextMultiline analogue): a text-area box for
    script bodies, notes, and console transcripts.  The entire field behavior -- the
    '\n'-separated buffer, the 2D caret + selection, word motion, undo / redo, clipboard,
    measurement, the mouse selection drag, and the horizontal pan -- lives one layer down as an
    interact mechanism (interact/gui_edit_multi.c), driven here through the single call
    medit_edit().  This widget owns only what an interact mechanism may not reach: the box is a
    CHILD REGION and the caret's VERTICAL scroll is that region's, so the widget chases the
    caret's row by writing the region scroll, then paints the state the engine resolved.

    Structure: the box is a CHILD REGION (the listbox recipe) whose body is one canvas cell
    spanning the full text content -- the region engine owns everything scroll-shaped:
    vertical scrollbar in its reserved gutter, wheel claim (innermost wins), scroll clamping,
    and the view scissor plus the interaction clip that lets the bar win hover over content
    beneath it.  The editor never touches a scrollbar or the wheel; it only writes the
    region's scroll_y to chase the caret (next frame, the standard region settle).  No word
    wrap (like Dear ImGui): long lines pan horizontally inside the cell (the engine's job),
    chasing the caret through the same glyph-level clip window the single-line field uses, so
    the widget adds no scissor of its own.

    Per-id persisted state (caret, anchor, horizontal pan, preferred column, blink) is a
    big-class keyed slot the engine fetches; its type (gui_medit_state_t), the engine entry
    (medit_edit), and the line-geometry readers (medit_sel, medit_line_count, medit_line_end,
    medit_row_start, medit_caret_rowx) come from the engine seam (interact/gui_interact.h).
    Included by gui_chrome.c after chrome/widgets/gui_input.c so the paint helpers are in scope; the
    child bracket resolves through the gui_host.h declarations like every other compound widget.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Paint -- per-row selection highlight, pan-clipped text, and the blinking caret into the
    canvas cell.  Reads the state the engine resolved (cursor / anchor / pan_x / blink_t) and
    measures with the engine's text_x_at + line geometry.

    Painting iterates only the rows intersecting the region view (the child's scissor would clip
    the rest anyway; the walk just skips the work).  Partial rows at the view edges are correct
    because the child clips -- no line snapping needed.  `inner` is the text content rect (the
    cell inset by the widget), matching the space the engine pans and hit-tests in.
==============================================================================================*/

static void
medit_paint( gui_rect_t inner, const char* buf, u32 len, const gui_medit_state_t* es, bool focused )
{
    const f32 line_h = font_line_h();
    const f32 char_h = font_char_h();

    f32 text_x  = inner.x - es->pan_x;
    f32 clip_x0 = inner.x;
    f32 clip_x1 = inner.x + inner.w;

    u32  sel_lo, sel_hi;
    bool has_sel;
    medit_sel( es, &sel_lo, &sel_hi, &has_sel );

    /* Visible row band from the interaction clip (the child view); the scissor makes edge
       rows correct, this walk just skips the fully hidden ones. */
    f32 band_y0 = s_scope.clip.y;
    f32 band_y1 = s_scope.clip.y + s_scope.clip.h;
    f32 rel     = band_y0 - inner.y;
    u32 first   = ( rel > 0.0f ) ? (u32)( rel / line_h ) : 0u;
    u32 ls      = medit_row_start( buf, len, first );

    for ( u32 row = first; ; ++row )
    {
        f32 ry = inner.y + (f32)row * line_h;
        if ( ry > band_y1 ) break;

        u32 le = medit_line_end( buf, len, ls );

        /* Selection highlight for this row's slice of [sel_lo,sel_hi); a selection running
           past the row end covers its newline, shown as a one-space tail (an empty line
           inside the selection stays visible as just that tail). */
        if ( focused && has_sel && sel_lo <= le && sel_hi > ls )
        {
            u32 a   = sel_lo > ls ? sel_lo : ls;
            u32 b   = sel_hi < le ? sel_hi : le;
            f32 sx0 = text_x + text_x_at( buf + ls, a - ls );
            f32 sx1 = text_x + text_x_at( buf + ls, b - ls );
            if ( sel_hi > le && le < len ) sx1 += font_char_advance( ' ' );
            if ( sx0 < clip_x0 ) sx0 = clip_x0;
            if ( sx1 > clip_x1 ) sx1 = clip_x1;
            if ( sx1 > sx0 )
                draw_fill( ( gui_rect_t ){ sx0, ry - 1.0f, sx1 - sx0, char_h + 2.0f },
                           COL_BG_ACTIVE );
        }

        if ( le > ls )
            draw_push_text_clip_n( text_x, ry, COL_TEXT_IDLE, buf + ls, le - ls, clip_x0, clip_x1 );

        /* Blinking caret on its row (visible for the first 0.5 s of each 1 s cycle). */
        if ( focused && es->cursor >= ls && es->cursor <= le )
        {
            bool caret_vis = ( ( (u32)( es->blink_t * 2.0f ) ) & 1u ) == 0u;
            f32  cxp       = text_x + text_x_at( buf + ls, es->cursor - ls );
            if ( caret_vis && cxp >= clip_x0 - 0.5f && cxp <= clip_x1 + 0.5f )
                draw_fill( ( gui_rect_t ){ cxp, ry, (f32)WIN_BORDER, char_h },
                           COL_TEXT_IDLE );
        }

        if ( le >= len ) break;
        ls = le + 1u;
    }
}

/*==============================================================================================
    medit_field_edit -- the editor body inside the open child region.

    Reserves one canvas cell spanning the full text content (at least the view, so a click in
    the empty area below the last line still lands on the editor), claims it with the standard
    item protocol -- the interaction clip and emission order then arbitrate against the
    region's own scrollbar exactly as they do for every widget -- drives the interact editor
    engine (medit_edit) for the whole field-internal frame, then does the one thing the engine
    cannot: chase the caret's row by writing the enclosing region's scroll_y (only on caret
    activity, so wheel / scrollbar scrolling may leave the caret off-screen), and paint.
    Returns true on any buffer modification this frame.
==============================================================================================*/

static bool
medit_field_edit( gui_id_t id, char* buf, u32 bufsz )
{
    u32 len = edit_strlen( buf, bufsz );

    const f32  line_h    = font_line_h();
    f32        content_h = (f32)medit_line_count( buf, len ) * line_h;
    f32        avail_h   = gui_content_avail().y;
    gui_rect_t content   = cell_next( content_h > avail_h ? content_h : avail_h );

    gui_item_state_t st = item_state( id, content, ITEM_FOCUSABLE );

    /* Field-tinted fill under the text: the input-box read on top of the child's own frame. */
    if ( st.focused ) draw_face( content, GUI_ROLE_BG, GUI_PHASE_ACTIVE );
    else              draw_face_field( content, id, st, GUI_ROLE_BG, GUI_PHASE_IDLE, 0u, 0.0f );

    /* Content rect: the cell inset by WIDGET_PAD on left / right (the engine + paint work in this
       space, so neither sees the widget's padding); vertical extent unchanged -- rows start at
       content.y. */
    gui_rect_t inner = { content.x + WIDGET_PAD, content.y,
                         content.w - 2.0f * WIDGET_PAD, content.h };

    u32 vis_rows = (u32)( lf()->view.h / line_h );
    if ( vis_rows < 1u ) vis_rows = 1u;

    /* The engine runs the whole field-internal frame (keys, mouse, hpan, blink, undo) and
       leaves the resolved caret / anchor / pan_x / blink on the keyed editor-state slot. */
    medit_result_t r = medit_edit( id, inner, st, vis_rows, line_h, buf, bufsz );

    len = edit_strlen( buf, bufsz );                       /* keys may have resized buf */
    gui_medit_state_t* es = GUI_STATE( gui_medit_state_t, id );

    /* Vertical caret chase: write the region's scroll target (applied next frame) when the caret
       moved this frame -- wheel / scrollbar scrolling (no caret activity) may leave it off-screen.
       The region is flow-owned (above the interact engine), so this is the widget's to do. */
    if ( r.active )
    {
        layout_frame_t* f = lf();
        if ( f->scroll )
        {
            u32 crow; f32 cx;
            medit_caret_rowx( buf, es->cursor, &crow, &cx );
            f32 view_h = f->view.h;
            f32 cy     = (f32)crow * line_h;
            f32 sy     = f->scroll->scroll_y;
            if ( cy < sy )                   sy = cy;
            if ( cy + line_h > sy + view_h ) sy = cy + line_h - view_h;
            f32 max_sy = (f32)medit_line_count( buf, len ) * line_h - view_h;
            if ( max_sy < 0.0f ) max_sy = 0.0f;
            if ( sy > max_sy )   sy = max_sy;
            if ( sy < 0.0f )     sy = 0.0f;
            f->scroll->scroll_y = sy;
        }
    }

    medit_paint( inner, buf, len, es, st.focused );

    return r.changed;
}

/*==============================================================================================
    input_text_multiline -- public entry: a child region (the listbox recipe) over the engine.

    The child owns the frame, the scrollbar, the wheel, and the clip; the trailing label draws
    past the box's right edge under the parent clip, exactly like listbox_end.
==============================================================================================*/

bool
gui_input_text_multiline( const char* label, char* buf, u32 bufsz, f32 h )
{
    if ( h <= 0.0f )
        h = font_line_h() * 8.0f + 2.0f * WIDGET_PAD;

    /* Box width: fill the line after reserving the trailing label (the listbox sizing).
       view_avail, not content_avail: the box is an opaque interactive surface and must stay
       inside the visible track even when a sibling has widened the content column past the
       view.  view_avail is also scroll-free, so the box keeps a constant width while the
       parent scrolls. */
    f32 lab_w = ( label_vis_len( label ) > 0 ) ? label_width( label ) + WIDGET_PAD : 0.0f;
    f32 w     = gui_view_avail().x - lab_w;
    if ( w < WIDGET_H * 4.0f ) w = WIDGET_H * 4.0f;

    gui_child_begin( label, w, h, GUI_WIN_NONE );
    gui_rect_t box = lf()->outer;                 /* the child's box, for the trailing label */
    gui_stack();

    bool changed = medit_field_edit( item_id( "##medit" ), buf, bufsz );

    gui_child_end();

    if ( label_vis_len( label ) > 0 )
        draw_label( box.x + box.w + WIDGET_PAD, text_center_y( box.y, WIDGET_H ),
                    COL_TEXT_IDLE, label );

    return changed;
}

// clang-format on
/*============================================================================================*/
