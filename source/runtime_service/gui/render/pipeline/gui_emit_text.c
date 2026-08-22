/*==============================================================================================

    gui/render/pipeline/gui_emit_text.c -- Glyph runs.

    The pool-backed pushes: every run is copied into the frame text pool before a command slot is
    spent, so callers may pass stack-local buffers and the command stores a byte offset rather than
    a pointer.  That ordering is the contract -- the copy must succeed first, or a failed pool
    append would leave a slot pointing at nothing.

    Three commands, not one with feathers: the plain run is the hot path every chrome label goes
    through, and the shadow and transformed forms would otherwise put unused fields on all of them.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    draw_push_text -- emit a glyph-run semantic command.

    str is copied into the frame text pool, so stack-local buffers (textf, snprintf labels) are
    fine; nothing about the caller's string needs to outlive the call.
    n == 0xFFFFFFFF means "entire NUL-terminated string"; a smaller n limits the glyph count
    (used to skip "##label" suffixes).
==============================================================================================*/

/* Copy one run into the frame text pool -- the single body behind both text pushes.  Returns
   false (loudly, once) when the pool is exhausted; *out_off is the stored offset.  The pool copy
   is what lets callers pass stack-local buffers (textf, snprintf labels): nothing about the
   caller's string needs to outlive the call. */

static bool
draw_text_pool_copy( const char* str, u32 len, u32* out_off )
{
    if ( s_draw.text_pool_used + len + 1 > GUI_MAX_TEXT_POOL )
    {
        /* Drop the label rather than store a dangling pointer -- but never silently.  Text
           vanishing with rects still painting reads as a font bug, not a pool cap, so name the
           real cause. */
        GUI_WARN_ONCE( "frame text pool full (%u bytes) -- further text this frame "
                       "is dropped. Raise GUI_MAX_TEXT_POOL (gui.h).\n", (unsigned)GUI_MAX_TEXT_POOL );
        ORB_ASSERT_MSG_ONCE( false, "gui text pool exhausted -- labels dropped; raise "
                                    "GUI_MAX_TEXT_POOL (gui.h)" );
        return false;
    }
    u32   off = s_draw.text_pool_used;   /* offset stored in the cmd; pointer stays local */
    char* dst = s_draw.text_pool + off;
    memcpy( dst, str, len );
    dst[ len ]            = '\0';
    s_draw.text_pool_used += len + 1;
    *out_off = off;
    return true;
}

void
draw_push_text_clip_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    if ( !str || draw_emit_blocked( k_cmd_hash_len[ GUI_CMD_TEXT ] ) )
        return;

    /* Transparent drop (the draw_cmd_open rule): a run whose folded fill alpha is 0 lights no
       pixel, so alpha doubles as a free visibility toggle -- a hidden label costs no command
       slot, no pool copy, no hash.  The one exception is a visible TEXT_EDGE: the edge band paints
       OUTSIDE the glyph boundary, so outline-only text over a transparent fill is a real shape and
       must survive. */
    u32 col = draw_apply_alpha( abgr );
    if ( ( col >> 24 ) == 0u && !draw_text_edge_visible() )
        return;

    /* Vertical cull: a glyph run lights pixels within roughly one line height of y, so if that band
       sits fully above or below the current clip the run is invisible -- a list row scrolled out of
       its box.  Padded a full line each way so ascenders / descenders are never wrongly dropped;
       horizontal overflow is left to the GPU scissor and the per-glyph clip in tess_text_n.  Done
       before the pool copy so a culled run costs no string-pool space either. */
    {
        gui_rect_t cc = clip_current();
        f32          lh = font_line_h();
        if ( rect_empty( cc ) || y + 2.0f * lh <= cc.y || y - lh >= cc.y + cc.h )
            return;
    }

    /* Resolve length at push time (sentinel means NUL-terminated), then pool the bytes. */
    u32 len = ( n == 0xFFFFFFFFu ) ? (u32)strlen( str ) : n;
    u32 off;
    if ( !draw_text_pool_copy( str, len, &off ) )
        return;

    gui_cmd_t*     c = draw_cmd_claim( GUI_CMD_TEXT );
    gui_cmd_ext_t* e = draw_cmd_claim_ext( c );
    e->text.x        = x;
    e->text.y        = y;
    e->text.off      = off;
    e->text.len      = len;   /* always an explicit byte count; never 0xFFFFFFFF after this point */
    e->text.clip_x0  = clip_x0;
    e->text.clip_x1  = clip_x1;
    e->text.abgr     = col;
    e->text.edge_w   = s_draw.text_edge_w;
    e->text.edge_col = s_draw.text_edge_col;
    e->text.font     = (u16)s_draw.cur_font;
    draw_cmd_seal();   /* text bytes are L1-hot here */
}

/* Text that inherits the ambient text-clip window: the common path for widget content.  Normally
   the window is the no-clip sentinel and the tessellator skips the per-glyph clip test entirely; a
   seam (table cell at the viewport edge) can set a real window so this run hard-clips at the slot
   edge without any call-site change. */
void
draw_push_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n )
{
    draw_push_text_clip_n( x, y, abgr, str, n, s_draw.text_clip_x0, s_draw.text_clip_x1 );
}

void
draw_push_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text_n( x, y, abgr, str, 0xFFFFFFFFu );
}

/* draw_push_text_shadow -- one command carrying both the shadow and the main glyph run; see
   text_shadow (gui.h) for why this is not two more fields on draw_push_text. */
void
draw_push_text_shadow( f32 x, f32 y, u32 abgr, u32 shadow_abgr, f32 dx, f32 dy, const char* str )
{
    if ( !str || draw_emit_blocked( k_cmd_hash_len[ GUI_CMD_TEXT_SHADOW ] ) )
        return;

    u32 col        = draw_apply_alpha( abgr );
    u32 shadow_col = draw_apply_alpha( shadow_abgr );
    if ( ( col >> 24 ) == 0u && ( shadow_col >> 24 ) == 0u )
        return;

    /* Same vertical cull as draw_push_text_clip_n, padded to cover the shadow's own offset. */
    {
        gui_rect_t cc = clip_current();
        f32        lh = font_line_h();
        f32        sy = ( dy < 0.0f ) ? y + dy : y;
        f32        sh = lh + ( dy < 0.0f ? -dy : dy );
        if ( rect_empty( cc ) || sy + lh + sh <= cc.y || sy - sh >= cc.y + cc.h )
            return;
    }

    u32 len = (u32)strlen( str );
    u32 off;
    if ( !draw_text_pool_copy( str, len, &off ) )
        return;

    gui_cmd_t*     c   = draw_cmd_claim( GUI_CMD_TEXT_SHADOW );
    gui_cmd_ext_t* e   = draw_cmd_claim_ext( c );
    e->text_shadow.x           = x;
    e->text_shadow.y           = y;
    e->text_shadow.off         = off;
    e->text_shadow.len         = len;
    e->text_shadow.clip_x0     = s_draw.text_clip_x0;
    e->text_shadow.clip_x1     = s_draw.text_clip_x1;
    e->text_shadow.abgr        = col;
    e->text_shadow.shadow_abgr = shadow_col;
    e->text_shadow.dx          = dx;
    e->text_shadow.dy          = dy;
    e->text_shadow.font        = (u16)s_draw.cur_font;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_text_xf -- emit a TRANSFORMED glyph run: scaled uniformly and rotated about (x, y).

    Everything the 1:1 push does with the string is done identically here (pool copy, so a stack
    buffer is fine).  What is deliberately NOT done is the vertical band cull: that test assumes a
    run lights pixels within a line height of y, which is exactly the assumption a scale and a
    rotation break -- a run rotated 90 degrees reaches its own WIDTH away from y.  Computing the
    true footprint would mean measuring the string here, so a transformed run simply relies on the
    GPU scissor, the same way every non-text shape does.  Only the empty-clip case still cuts, and
    that one is free.
==============================================================================================*/

void
draw_push_text_xf( f32 x, f32 y, u32 abgr, const char* str, f32 scale, f32 rot )
{
    if ( !str || scale <= 0.0f || draw_emit_blocked( k_cmd_hash_len[ GUI_CMD_TEXT_XF ] ) )
        return;

    /* Transparent drop, with the same TEXT_EDGE exception as draw_push_text_clip_n. */
    u32 col = draw_apply_alpha( abgr );
    if ( ( col >> 24 ) == 0u && !draw_text_edge_visible() )
        return;
    if ( rect_empty( clip_current() ) )
        return;

    u32 len = (u32)strlen( str );
    u32 off;
    if ( !draw_text_pool_copy( str, len, &off ) )
        return;

    gui_cmd_t*     c   = draw_cmd_claim( GUI_CMD_TEXT_XF );
    gui_cmd_ext_t* e   = draw_cmd_claim_ext( c );
    e->text_xf.x     = x;
    e->text_xf.y     = y;
    e->text_xf.off   = off;
    e->text_xf.len   = len;
    e->text_xf.scale = scale;
    e->text_xf.rot   = rot;
    e->text_xf.abgr  = col;
    e->text_xf.edge_w   = s_draw.text_edge_w;
    e->text_xf.edge_col = s_draw.text_edge_col;
    e->text_xf.font  = (u16)s_draw.cur_font;
    draw_cmd_seal();
}

// clang-format on
/*============================================================================================*/
