/*==============================================================================================

    runtime_service/gui/debug/gui_step_window.c -- Command stepper: control window shell.

    The interactive front end of the command stepper (render/gui_step_capture.c): Capture /
    Release, a transport row ( |<  <  >  >| ), a scrub slider over the frozen command prefix,
    an inspector decoding the current command (type, owning window, clip, per-type fields,
    color swatches), and the frozen segment list -- click a row to seek to its start (shift:
    its end).  An ORDINARY window drawn through the normal pipeline with the standard widgets --
    what keeps it usable over a frozen scene is GUI_WIN_DEBUG_BAND: the capture never snapshots
    the debug band and live debug-band emission is never suppressed, so the controls stay fully
    interactive while the band-0 replay under them holds still.

    The highlight outlines the current command's bounds (or a hovered segment row's union) over
    the frozen scene itself: the draw escapes this window's clip to the display root
    (draw_push_clip_root) and routes to the command's viewport, but stays in THIS window's
    command segment -- debug band, this window's z -- so it paints above the frozen band-0
    content with no extra window or z machinery.

    Every control LATCHES its effect (capture at the next build, release / cursor at the next
    frame's restore -- see gui_render.h), so each mutating branch raises wants_redraw or the
    clean-frame emit skip would sit on the stale request (the deferred-update rule).

    Emitted internally (debug_overlays_emit, gui_frame_overlay.c) at the default context's
    ctx_end while debug_enable is on.  The F8 hotkey only shows/hides this window -- opening it
    leaves the scene live; the Capture button freezes and Release lets go.  The X button only
    hides the window -- hiding does NOT release an active freeze (Release does).
    The , . step hotkeys keep working alongside (they scrub with key repeat; the buttons do not).
    Compiled out unless GUI_CMD_STEPPER (gui_render.h); step_window stays a no-op stub then.

==============================================================================================*/
// clang-format off

#ifdef GUI_CMD_STEPPER

#define STEP_SHELL_TITLE "Command Stepper"

#define STEP_COL_DIM        GUI_COLOR( 0x90, 0x90, 0x90, 0xFF )
#define STEP_COL_HL_CMD     GUI_COLOR( 0x50, 0xE0, 0xF0, 0xFF )   /* current command outline  */
#define STEP_COL_HL_SEG     GUI_COLOR( 0xF0, 0xE0, 0x40, 0xFF )   /* hovered segment outline  */

/* View toggle: outline the current command / hovered segment over the frozen scene. */
static bool s_step_highlight = true;

/* Armed picker: while on, any mouse press over the frozen scene seeks to the topmost visible
   command under it ("what drew this pixel").  A toggle rather than a hotkey on purpose: a key
   fought the focused window's keyboard nav / type-ahead the moment this window had focus. */
static bool s_step_pick_arm;

/* Play transport: auto-advance the cursor by `rate` commands per second while playing.  The
   fractional accumulator carries the sub-command remainder across frames so low rates still
   move; playing pins wants_redraw every frame (the frame loop must keep pumping). */
static bool s_step_play;
static i32  s_step_rate = 20;   /* capped at 60: the whole drag range stays in the usable band */
static f32  s_step_accum;

/* Indexed by gui_cmd_type_t -- one name per enum value, in enum order. */
static const char* k_step_type_name[] = {
    "rect_fill", "rect_tex", "rect_outline", "triangle", "bezier", "text", "text_xf", "text_shadow",
    "line", "polyline", "dashed_line", "rect_gradient", "rect_list",
    "sprite", "fx_box", "round_rect_ex", "arc", "pie",
    "arc_dash", "arc_grad", "image_xf", "checker", "grid",
    "ngon", "box_dash", "frame", "round_frame_ex", "repeat", "repeat_polar", "box_cut",
};

/* id -> registered source string (debug overlay's registry) or hex.  buf must hold >= 12.
   0 is the background draw layer (no window), not an unnamed id.  A leading "##" (an id-only
   window title: the main menu bar) is skipped -- inside a widget label the parser hides
   everything from the first "##", which rendered those rows completely blank. */
static const char*
step_name( gui_id_t id, char* buf, u32 bufsz )
{
    if ( id == 0 )
        return "(background)";
    const char* n = gui_debug_name( id );
    if ( n )
    {
        if ( n[ 0 ] == '#' && n[ 1 ] == '#' )
            n += 2;
        return n;
    }
    fmt_snprintf( buf, bufsz, "%08X", id );
    return buf;
}

/* Seek + the wants_redraw the latched cursor needs to reach the next frame's restore. */
static void
step_seek_dirty( u32 cursor )
{
    step_seek( cursor );
    redraw_request();
}

/* One swatch square + its labelled packed value, drawn into row `r` at pen `x`; returns the pen
   past it, so a row can chain several (the gradient's col_a / col_b). */
static f32
step_swatch( gui_rect_t r, f32 x, const char* label, u32 abgr )
{
    gui_draw_rect( x, r.y + 1.0f, r.h - 2.0f, r.h - 2.0f, abgr | 0xFF000000u );
    gui_draw_round_rect( ( gui_rect_t ){ x, r.y + 1.0f, r.h - 2.0f, r.h - 2.0f },
                         0.0f, 0.0f, 0.0f, 0.0f, 1.0f, STEP_COL_DIM );
    char buf[ 48 ];
    fmt_snprintf( buf, sizeof( buf ), "%s 0x%08X", label, abgr );
    gui_draw_text( x + r.h + 4.0f, r.y, STEP_COL_DIM, buf );
    return x + r.h + 4.0f + gui_text_size( buf ).x + 16.0f;
}

/* Per-type field decode of the current command.  FIXED SHAPE: every type emits exactly two field
   rows (the second may be blank) and one swatch row, so the inspector -- and everything laid out
   below it -- never changes height as the cursor walks across command kinds. */
static void
step_cmd_detail( const step_cmd_info_t* ci )
{
    const gui_cmd_t*      c    = &ci->cmd;
    const gui_cmd_ext_t*  e    = step_cmd_ext( c );   /* every type's payload, including RECT_FILL/TEXT */
    const char*           row2 = NULL;   /* NULL = blank second row */
    char                  b2[ 96 ];

    switch ( (gui_cmd_type_t)c->type )
    {
        case GUI_CMD_RECT_FILL:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f   round %.1f",
                       e->rect_fill.x, e->rect_fill.y, e->rect_fill.w, e->rect_fill.h,
                       e->rect_fill.rounding );
            fmt_snprintf( b2, sizeof( b2 ), "solid" );
            row2 = b2;
            break;
        case GUI_CMD_RECT_TEX:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f   round %.1f",
                       e->rect_tex.x, e->rect_tex.y, e->rect_tex.w, e->rect_tex.h,
                       e->rect_tex.rounding );
            /* Naming the sampling model matters here: a glyph run that landed in the wrong one is
               invisible in the geometry and obvious in this label.  The mode field is 4 bits, so
               an unnamed value prints numerically rather than indexing off a table's end. */
            {
                static const char* const k_tex_mode[ 3 ] = { "", "  (rgba)", "  (sdf)" };
                u32  tmode = (u32)gui_tex_mode( e->rect_tex.tex_idx );
                char msfx[ 16 ];
                if ( tmode >= 3 )
                    fmt_snprintf( msfx, sizeof( msfx ), "  (mode %u)", tmode );
                fmt_snprintf( b2, sizeof( b2 ), "tex %u%s", gui_tex_index( e->rect_tex.tex_idx ),
                              tmode < 3 ? k_tex_mode[ tmode ] : msfx );
            }
            row2 = b2;
            break;
        case GUI_CMD_RECT_OUTLINE:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->rect_outline.x, e->rect_outline.y,
                       e->rect_outline.w, e->rect_outline.h );
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f   round %.1f", e->rect_outline.t,
                      e->rect_outline.rounding );
            row2 = b2;
            break;
        case GUI_CMD_FRAME:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->frame.x, e->frame.y,
                       e->frame.w, e->frame.h );
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f   round %.1f", e->frame.t,
                      e->frame.rounding );
            row2 = b2;
            break;
        case GUI_CMD_ROUND_FRAME_EX:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->round_frame_ex.x, e->round_frame_ex.y,
                       e->round_frame_ex.w, e->round_frame_ex.h );
            /* Listed in quadrant order, same as ROUND_RECT_EX. */
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f   r tl %.1f  tr %.1f  br %.1f  bl %.1f",
                          e->round_frame_ex.t, e->round_frame_ex.rtl, e->round_frame_ex.rtr,
                          e->round_frame_ex.rbr, e->round_frame_ex.rbl );
            row2 = b2;
            break;
        case GUI_CMD_TRIANGLE:
            gui_textf( "a %.0f,%.0f   b %.0f,%.0f", e->tri.ax, e->tri.ay, e->tri.bx, e->tri.by );
            fmt_snprintf( b2, sizeof( b2 ), "c %.0f,%.0f", e->tri.cx, e->tri.cy );
            row2 = b2;
            break;
        case GUI_CMD_TEXT:
            gui_textf( "pos %.0f,%.0f   len %u%s", e->text.x, e->text.y, e->text.len,
                       ( e->text.clip_x1 < GUI_TEXT_NO_CLIP ) ? "   (glyph-clipped)" : "" );
            fmt_snprintf( b2, sizeof( b2 ), "\"%.60s\"", ci->text ? ci->text : "" );
            row2 = b2;
            break;
        case GUI_CMD_TEXT_XF:
            gui_textf( "pos %.0f,%.0f   len %u   scale %.2f   rot %.0f deg",
                       e->text_xf.x, e->text_xf.y, e->text_xf.len, e->text_xf.scale,
                       gui_degrees( e->text_xf.rot ) );
            fmt_snprintf( b2, sizeof( b2 ), "\"%.60s\"", ci->text ? ci->text : "" );
            row2 = b2;
            break;
        case GUI_CMD_TEXT_SHADOW:
            gui_textf( "pos %.0f,%.0f   len %u   shadow %.0f,%.0f",
                       e->text_shadow.x, e->text_shadow.y, e->text_shadow.len,
                       e->text_shadow.dx, e->text_shadow.dy );
            fmt_snprintf( b2, sizeof( b2 ), "\"%.60s\"", ci->text ? ci->text : "" );
            row2 = b2;
            break;
        case GUI_CMD_LINE:
            gui_textf( "%.0f,%.0f -> %.0f,%.0f", e->line.x0, e->line.y0, e->line.x1, e->line.y1 );
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f", e->line.thickness );
            row2 = b2;
            break;
        case GUI_CMD_POLYLINE:
            gui_textf( "%u pts   %s", e->polyline.pt_count,
                       e->polyline.closed ? "closed" : "open" );
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f   align %u", e->polyline.thickness,
                      (u32)e->polyline.align );
            row2 = b2;
            break;
        case GUI_CMD_DASHED_LINE:
            gui_textf( "%.0f,%.0f -> %.0f,%.0f", e->dash.x0, e->dash.y0, e->dash.x1, e->dash.y1 );
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f   period %.1f   duty %.2f", e->dash.thickness,
                      e->dash.period, e->dash.duty );
            row2 = b2;
            break;
        case GUI_CMD_RECT_GRADIENT:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->gradient.x, e->gradient.y,
                       e->gradient.w, e->gradient.h );
            fmt_snprintf( b2, sizeof( b2 ), "%s", e->gradient.horizontal ? "horizontal" : "vertical" );
            row2 = b2;
            break;
        case GUI_CMD_RECT_LIST:
            gui_textf( "%u rects   pool offset %u", e->rect_list.count, e->rect_list.offset );
            break;
        case GUI_CMD_FX_BOX:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->fx_box.x, e->fx_box.y,
                       e->fx_box.w, e->fx_box.h );
            fmt_snprintf( b2, sizeof( b2 ), "round %.1f   feather %.1f   rate %.2f Hz   depth %.2f   swell %.1f%s",
                          e->fx_box.rounding, e->fx_box.feather, e->fx_box.rate, e->fx_box.depth,
                          e->fx_box.swell,
                          ( e->fx_box.variant == GUI_FX_BOX_SKIRT ) ? "   skirt"
                          : ( e->fx_box.variant == GUI_FX_BOX_INSET ) ? "   inset"
                          : ( e->fx_box.variant == GUI_FX_BOX_GLOW )  ? "   glow"
                          : ( e->fx_box.variant == GUI_FX_BOX_RING )  ? "   ring" : "" );
            row2 = b2;
            break;
        case GUI_CMD_ROUND_RECT_EX:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->round_rect.x, e->round_rect.y,
                       e->round_rect.w, e->round_rect.h );
            /* Listed in quadrant order, which is also the order they tessellate in. */
            fmt_snprintf( b2, sizeof( b2 ), "r tl %.1f  tr %.1f  br %.1f  bl %.1f",
                          e->round_rect.rtl, e->round_rect.rtr,
                          e->round_rect.rbr, e->round_rect.rbl );
            row2 = b2;
            break;
        /* Angles in degrees: the command stores radians, but nobody debugs a sweep in radians. */
        case GUI_CMD_ARC:
        case GUI_CMD_PIE:
            gui_textf( "centre %.0f,%.0f   r %.1f", e->arc.cx, e->arc.cy, e->arc.r );
            fmt_snprintf( b2, sizeof( b2 ), "%.1f -> %.1f deg   sweep %.1f   t %.1f",
                          e->arc.a0 * 57.2957795f, e->arc.a1 * 57.2957795f,
                          ( e->arc.a1 - e->arc.a0 ) * 57.2957795f, e->arc.thickness );
            row2 = b2;
            break;
        case GUI_CMD_ARC_DASH:
            gui_textf( "centre %.0f,%.0f   r %.1f   t %.1f", e->arc_dash.cx, e->arc_dash.cy,
                       e->arc_dash.r, e->arc_dash.thickness );
            fmt_snprintf( b2, sizeof( b2 ), "%.1f -> %.1f deg   period %.1f deg   duty %.2f",
                          e->arc_dash.a0 * 57.2957795f, e->arc_dash.a1 * 57.2957795f,
                          e->arc_dash.period * 57.2957795f, e->arc_dash.duty );
            row2 = b2;
            break;
        case GUI_CMD_ARC_GRAD:
            gui_textf( "centre %.0f,%.0f   r %.1f   t %.1f", e->arc_grad.cx, e->arc_grad.cy,
                       e->arc_grad.r, e->arc_grad.thickness );
            fmt_snprintf( b2, sizeof( b2 ), "%.1f -> %.1f deg",
                          e->arc_grad.a0 * 57.2957795f, e->arc_grad.a1 * 57.2957795f );
            row2 = b2;
            break;
        case GUI_CMD_IMAGE_XF:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f   rot %.0f deg", e->image_xf.x, e->image_xf.y,
                       e->image_xf.w, e->image_xf.h, gui_degrees( e->image_xf.rot ) );
            fmt_snprintf( b2, sizeof( b2 ), "tex %u (mode %u)",
                          gui_tex_index( e->image_xf.tex_idx ),
                          (u32)gui_tex_mode( e->image_xf.tex_idx ) );
            row2 = b2;
            break;
        case GUI_CMD_SPRITE:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f   scale %.2f", e->sprite.x, e->sprite.y,
                       e->sprite.w, e->sprite.h, e->sprite.scale );
            fmt_snprintf( b2, sizeof( b2 ), "sprite %u   flags 0x%04X   nine %u",
                          e->sprite.sprite, (u32)e->sprite.flags, (u32)e->sprite.nine );
            row2 = b2;
            break;
        case GUI_CMD_CHECKER:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->checker.x, e->checker.y,
                       e->checker.w, e->checker.h );
            fmt_snprintf( b2, sizeof( b2 ), "cell %.1f", e->checker.cell );
            row2 = b2;
            break;
        case GUI_CMD_GRID:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->grid.x, e->grid.y,
                       e->grid.w, e->grid.h );
            fmt_snprintf( b2, sizeof( b2 ), "cell %.1f   t %.1f   origin %.0f,%.0f",
                          e->grid.cell, e->grid.thickness, e->grid.ox, e->grid.oy );
            row2 = b2;
            break;
        case GUI_CMD_NGON:
            gui_textf( "centre %.0f,%.0f   r %.1f   sides %u", e->ngon.cx, e->ngon.cy,
                       e->ngon.r, e->ngon.sides );
            fmt_snprintf( b2, sizeof( b2 ), "round %.1f   t %.1f   rot %.0f deg   star %.2f",
                          e->ngon.rounding, e->ngon.thickness, gui_degrees( e->ngon.rot ),
                          e->ngon.star );
            row2 = b2;
            break;
        case GUI_CMD_BOX_DASH:
            gui_textf( "rect %.0f,%.0f  %.0f x %.0f", e->box_dash.x, e->box_dash.y,
                       e->box_dash.w, e->box_dash.h );
            fmt_snprintf( b2, sizeof( b2 ), "t %.1f   dash %.1f/%.1f   rate %.0f px/s",
                          e->box_dash.t, e->box_dash.dash, e->box_dash.gap, e->box_dash.rate );
            row2 = b2;
            break;
    }
    gui_text( row2 ? row2 : " " );

    /* Swatch row -- always reserved, so the section height is constant. */
    gui_rect_t r = gui_canvas( font_line_h() );
    switch ( (gui_cmd_type_t)c->type )
    {
        case GUI_CMD_RECT_GRADIENT:
        {
            f32 x = step_swatch( r, r.x, "col_a", e->gradient.col_a );
            step_swatch( r, x, "col_b", e->gradient.col_b );
            break;
        }
        case GUI_CMD_ARC_GRAD:
        {
            f32 x = step_swatch( r, r.x, "col_a", e->arc_grad.col_a );
            step_swatch( r, x, "col_b", e->arc_grad.col_b );
            break;
        }
        case GUI_CMD_CHECKER:
        {
            f32 x = step_swatch( r, r.x, "col_a", e->checker.col_a );
            step_swatch( r, x, "col_b", e->checker.col_b );
            break;
        }
        case GUI_CMD_FRAME:
        {
            f32 x = step_swatch( r, r.x, "fill", e->frame.abgr );
            step_swatch( r, x, "border", e->frame.col_border );
            break;
        }
        case GUI_CMD_ROUND_FRAME_EX:
        {
            f32 x = step_swatch( r, r.x, "fill", e->round_frame_ex.abgr );
            step_swatch( r, x, "border", e->round_frame_ex.col_border );
            break;
        }
        case GUI_CMD_RECT_LIST:
            gui_draw_text( r.x, r.y, STEP_COL_DIM, "per-entry colors (rect pool)" );
            break;
        default:
            /* Every remaining variant carries one abgr; text/rect/line unions place it last but
               it is read through the matching member, not positionally. */
            switch ( (gui_cmd_type_t)c->type )
            {
                case GUI_CMD_RECT_FILL:     step_swatch( r, r.x, "color", e->rect_fill.abgr );    break;
                case GUI_CMD_RECT_TEX:      step_swatch( r, r.x, "color", e->rect_tex.abgr );     break;
                case GUI_CMD_RECT_OUTLINE:  step_swatch( r, r.x, "color", e->rect_outline.abgr ); break;
                case GUI_CMD_TRIANGLE:      step_swatch( r, r.x, "color", e->tri.abgr );          break;
                case GUI_CMD_TEXT:          step_swatch( r, r.x, "color", e->text.abgr );         break;
                case GUI_CMD_TEXT_XF:       step_swatch( r, r.x, "color", e->text_xf.abgr );      break;
                case GUI_CMD_TEXT_SHADOW:
                {
                    f32 sx = step_swatch( r, r.x, "color", e->text_shadow.abgr );
                    step_swatch( r, sx, "shadow", e->text_shadow.shadow_abgr );
                    break;
                }
                case GUI_CMD_LINE:          step_swatch( r, r.x, "color", e->line.abgr );         break;
                case GUI_CMD_POLYLINE:      step_swatch( r, r.x, "color", e->polyline.abgr );     break;
                case GUI_CMD_DASHED_LINE:   step_swatch( r, r.x, "color", e->dash.abgr );         break;
                case GUI_CMD_FX_BOX:        step_swatch( r, r.x, "color", e->fx_box.abgr );       break;
                case GUI_CMD_ROUND_RECT_EX:
                    step_swatch( r, r.x, "color", e->round_rect.abgr );
                    if ( e->round_rect.col_b != e->round_rect.abgr )
                        step_swatch( r, r.x, "col_b", e->round_rect.col_b );
                    break;
                case GUI_CMD_ARC:
                case GUI_CMD_PIE:           step_swatch( r, r.x, "color", e->arc.abgr );          break;
                case GUI_CMD_ARC_DASH:      step_swatch( r, r.x, "color", e->arc_dash.abgr );     break;
                case GUI_CMD_IMAGE_XF:      step_swatch( r, r.x, "color", e->image_xf.abgr );     break;
                case GUI_CMD_GRID:          step_swatch( r, r.x, "color", e->grid.abgr );         break;
                case GUI_CMD_NGON:          step_swatch( r, r.x, "color", e->ngon.abgr );         break;
                case GUI_CMD_BOX_DASH:      step_swatch( r, r.x, "color", e->box_dash.abgr );     break;
                default:                                                                          break;
            }
            break;
    }
}

/* Outline `r` over the frozen scene: escape this window's clip to the display root and route to
   the command's viewport.  The segment stays in THIS window's slot (debug band), so the outline
   paints above the frozen band-0 content with the window. */
static void
step_highlight_rect( gui_rect_t r, i32 vp, u32 abgr )
{
    if ( r.w <= 0.0f || r.h <= 0.0f )
        return;
    gui_draw_scope_t save = draw_scope();
    gui_draw_scope_t hl   = save;
    hl.viewport           = vp;
    draw_scope_set( hl );
    draw_push_clip_root();
    gui_draw_round_rect( ( gui_rect_t ){ r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f },
                         0.0f, 0.0f, 0.0f, 0.0f, 2.0f, abgr );
    draw_pop_clip_rect();
    draw_scope_set( save );
}

void
step_window( bool* open )
{
    if ( !( open && *open ) )
        return;

    /* The host said open: reopen the pool entry if the X button hid it on an earlier run. */
    gui_window_set_open( STEP_SHELL_TITLE, true );

    gui_window_set_next_size( 400.0f, 560.0f, GUI_COND_ONCE );
    if ( gui_window_begin( STEP_SHELL_TITLE, GUI_WIN_CLOSEABLE | GUI_WIN_DEBUG_BAND ) )
    {
        gui_stack();

        /* State row: the freeze toggle plus the cursor readout. */
        bool frozen = step_frozen();
        if ( gui_button( frozen ? "Release" : "Capture" ) )
        {
            if ( frozen ) step_release();
            else          step_capture();
            s_step_play  = false;
            s_step_accum = 0.0f;
            redraw_request();
        }
        gui_same_line( -1.0f );
        if ( frozen )
            gui_textf( "frozen   %u / %u", step_cursor(), step_count() );
        else
            gui_text( "live -- Capture freezes this frame's command list" );

        if ( frozen )
        {
            u32 cnt = step_count();

            /* Play: advance by rate * dt with a fractional carry, stop at the frame's end.
               Runs before the controls read the cursor so the row shows this frame's position. */
            if ( s_step_play )
            {
                s_step_accum += (f32)s_step_rate * s_io.dt;
                u32 step = (u32)s_step_accum;
                if ( step > 0 )
                {
                    s_step_accum -= (f32)step;
                    step_seek( step_cursor() + step );   /* clamps to cnt */
                }
                if ( step_cursor() >= cnt )
                    s_step_play = false;
                redraw_request();   /* keep pumping frames while playing */
            }

            u32 cur = step_cursor();

            /* Transport: seek to start / one back / one forward / seek to end, then Play.
               Single steps -- the , . hotkeys cover held scrubbing (key repeat), the slider
               covers long jumps.  Play from the end restarts at the frame start. */
            if ( gui_button( "|<" ) )
                step_seek_dirty( 0 );
            gui_same_line( -1.0f );
            if ( gui_button( "<" ) && cur > 0 )
                step_seek_dirty( cur - 1 );
            gui_same_line( -1.0f );
            if ( gui_button( ">" ) )
                step_seek_dirty( cur + 1 );   /* seek clamps to the frozen count */
            gui_same_line( -1.0f );
            if ( gui_button( ">|" ) )
                step_seek_dirty( cnt );
            gui_same_line( -1.0f );
            if ( gui_button( s_step_play ? "Pause" : "Play" ) )
            {
                s_step_play = !s_step_play;
                if ( s_step_play && cur >= cnt )
                    step_seek( 0 );
                s_step_accum = 0.0f;
                redraw_request();
            }
            gui_same_line( -1.0f );
            gui_checkbox( "Highlight", &s_step_highlight );

            /* View row: playback rate + the display/replay order.  Toggling the order re-seats
               the same numeric cursor in the other sequence -- the scene recomposes accordingly. */
            gui_drag_int( "rate##step_rate", &s_step_rate, 0.25f, 1, 60, "%d cmd/s" );

            bool paint = step_paint_order();
            if ( gui_checkbox( "Paint order", &paint ) )
            {
                step_set_paint_order( paint );
                redraw_request();
            }
            gui_same_line( -1.0f );
            gui_checkbox( "Pick", &s_step_pick_arm );

            /* Armed picker: a mouse press anywhere over the frozen scene seeks to the topmost
               visible command under it.  Presses on the visible debug tooling (any DEBUG_BAND
               window: this one, the dashboard) keep their normal meaning -- but an INVISIBLE
               live band-0 window hovering under the cursor must not shield the frozen scene,
               so only the debug-band flag exempts, not hover itself. */
            if ( s_step_pick_arm && s_io.mouse_pressed[ 0 ] )
            {
                gui_window_t* hw = window_find( s_interaction.hover_win );
                if ( !hw || !( hw->flags & GUI_WIN_DEBUG_BAND ) )
                {
                    u32 idx;
                    if ( step_pick( s_io.mouse_x, s_io.mouse_y, s_io.mouse_viewport, &idx ) )
                        step_seek_dirty( idx + 1 );   /* the picked command becomes current */
                }
            }

            /* Scrubber over the whole frozen prefix, with a tick at every segment boundary. */
            i32 v = (i32)cur;
            if ( gui_slider_int( "##step_cursor", &v, 0, (i32)cnt, NULL ) )
                step_seek_dirty( v < 0 ? 0u : (u32)v );
            if ( cnt > 0 )
            {
                gui_rect_t sr   = gui_get_item_rect();
                u32        nseg = step_seg_count();
                for ( u32 si = 1; si < nseg; ++si )   /* boundary 0 is the track start; skip it */
                {
                    step_seg_info_t sg;
                    if ( !step_seg_info( si, &sg ) )
                        break;
                    f32 x = sr.x + sr.w * ( (f32)sg.lo / (f32)cnt );
                    gui_draw_rect( x, sr.y + sr.h - 4.0f, 1.0f, 4.0f, STEP_COL_DIM );
                }
            }

            /* Inspector: the current command -- the LAST visible one, cursor - 1. */
            gui_separator_text( "Command" );
            step_cmd_info_t ci = { 0 };
            bool have_cmd = cur > 0 && step_cmd_info( cur - 1, &ci );
            if ( !have_cmd )
            {
                /* Same fixed shape as a decoded command (5 text rows + the swatch row), so the
                   section height never jumps when the cursor crosses the frame start. */
                gui_text( "(cursor at frame start)" );
                gui_text( " " );
                gui_text( " " );
                gui_text( " " );
                gui_text( " " );
                gui_canvas( font_line_h() );
            }
            else
            {
                char nb[ 12 ], ob[ 12 ];
                gui_textf( "#%u  %s   in %s", cur - 1,
                           ci.cmd.type < ARRAY_COUNT( k_step_type_name )
                               ? k_step_type_name[ ci.cmd.type ] : "?",
                           step_name( ci.win, nb, sizeof( nb ) ) );
                gui_textf( "widget %s", ci.owner ? step_name( ci.owner, ob, sizeof( ob ) )
                                                 : "(chrome)" );
                /* The font is the COMMAND's own now, so it reads "-" on everything that is not a
                   glyph run rather than reporting whatever its segment happened to be tagged. */
                char fb[ 12 ];
                if ( ci.cmd.type == GUI_CMD_TEXT || ci.cmd.type == GUI_CMD_TEXT_XF )
                    fmt_snprintf( fb, sizeof( fb ), "%u", ci.font );
                else
                    fmt_snprintf( fb, sizeof( fb ), "-" );
                gui_textf( "z %u   vp %d   font %s   clip %.0f,%.0f %.0fx%.0f",
                           ci.z, ci.vp, fb, ci.clip.x, ci.clip.y, ci.clip.w, ci.clip.h );
                step_cmd_detail( &ci );
            }

            /* Segment list: every frozen span in emit order.  Click seeks to the segment start
               (shift: its end); the row containing the cursor reads selected; hovering outlines
               the segment's union bounds over the scene. */
            gui_separator_text( "Segments" );
            gui_rect_t hover_bounds   = ( gui_rect_t ){ 0.0f, 0.0f, 0.0f, 0.0f };
            u32        hover_vp       = 0;
            bool       have_hover     = false;

            if ( gui_child_begin( "##step_segs", 0.0f, 9.0f * ( font_line_h() + 8.0f ),
                                  GUI_WIN_NONE ) )
            {
                gui_stack();   /* the child is a fresh layout frame: declare its mode first */
                char nb[ 12 ], lbl[ 96 ];
                u32  nseg = step_seg_count();
                for ( u32 si = 0; si < nseg; ++si )
                {
                    step_seg_info_t sg;
                    if ( !step_seg_info( si, &sg ) )
                        break;
                    /* Fields first, name LAST: a "##" inside a window title (an instance suffix)
                       hides the rest of the label, so it may only ever eat the name's tail. */
                    fmt_snprintf( lbl, sizeof( lbl ), "z%-3u vp%d  [%4u..%4u)  %.24s##seg%u",
                              sg.z, sg.vp, sg.lo, sg.hi,
                              step_name( sg.win, nb, sizeof( nb ) ), si );
                    bool in_seg = ( cur > sg.lo && cur <= sg.hi );
                    /* Land ON the segment: lo + 1 makes its FIRST command current (a seek to lo
                       would sit just before it -- the previous segment's last command).  Shift:
                       hi, the segment fully built (its last command current). */
                    if ( gui_selectable( lbl, &in_seg ) )
                        step_seek_dirty( gui_is_key_down( APP_KEY_LSHIFT )
                                         || gui_is_key_down( APP_KEY_RSHIFT ) ? sg.hi : sg.lo + 1 );
                    if ( gui_is_item_hovered() )
                    {
                        hover_bounds = sg.bounds;
                        hover_vp     = sg.vp;
                        have_hover   = true;
                    }
                }
            }
            gui_child_end();

            /* Highlight over the frozen scene: a hovered segment wins over the current command. */
            if ( s_step_highlight )
            {
                if ( have_hover )
                    step_highlight_rect( hover_bounds, hover_vp, STEP_COL_HL_SEG );
                else if ( have_cmd )
                    step_highlight_rect( ci.bounds, ci.vp, STEP_COL_HL_CMD );
            }
        }
    }
    gui_window_end();

    /* The X button closed it this frame: report back so the hotkey toggle stays in sync. */
    if ( !gui_window_is_open( STEP_SHELL_TITLE ) )
        *open = false;
}

#else  /* !GUI_CMD_STEPPER */

/* No-op stub so the emit site (debug_overlays_emit) needs no build guard of its own. */
void
step_window( bool* open )
{
    (void)open;
}

#endif /* GUI_CMD_STEPPER */

// clang-format on
/*============================================================================================*/
