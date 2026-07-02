/*==============================================================================================

    runtime_service/gui/backend/gui_emit_draw.c -- Draw list accumulation.

    All geometry goes through draw_push_rect_filled or draw_push_triangle.
    draw_ensure_cmd opens a new draw command when the texture or clip rect changes.
    draw_push_text emits glyph quads from the font atlas.

    Included by gui_backend.c after gui_font.c so font_glyph / s_atlas_idx are in scope.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    gui_gpu_cmd_t -- backend-private GPU draw command.

    One bounded range of indices sharing a texture slot and scissor rect -- the unit the GPU
    sees.  Not exposed in gui.h.  The public gui_cmd_t carries semantic shapes; the render
    backend (gui_render.c) tessellates those into these at flush time.
==============================================================================================*/

typedef struct
{
    u32          elem_count; // number of indices to emit
    u32          tex_idx;    // bindless texture slot
    gui_rect_t   clip_rect;  // scissor rect (pixels)

} gui_gpu_cmd_t;

/*==============================================================================================
    Draw list -- the per-frame command buffer and segment table.

    Commands are pushed into cmds[] by the widget layer.  Whenever (win, z, vp, font) changes,
    the current open span is closed and a new one is opened, so the buffer is partitioned into
    contiguous per-(win,z,vp,font) segments.  cache_tess_window walks these segments per window
    rather than re-scanning the full command buffer.  [lo, hi) is a half-open range; the final
    segment's hi is closed at build time.  win=0 is the background (non-window) draw layer.
==============================================================================================*/

typedef struct
{
    gui_id_t win;
    u32      z, vp, font;
    u32      lo, hi;   /* half-open command range into s_draw.cmds[] */

} gui_cmd_seg_t;

static struct
{
    gui_cmd_t       cmds        [ GUI_MAX_CMDS   ];     // semantic command list; one entry per shape
    u32             cmd_hashes  [ GUI_MAX_CMDS   ];     // per-command hash baked at emit (for cache diff)
    gui_vec2_t      points      [ GUI_MAX_PATH_PTS ];   // point pool for CMD_POLYLINE data; indexed by pt_offset

    gui_cmd_seg_t   segs[ GUI_MAX_SEGS ];       /* per-(z,vp) command spans, in emit order */
    u32             seg_count;                /* spans open this frame (>= 1; segs[0] is z=0,vp=0) */

    /* Flat string pool: draw_push_text_n copies every string here so that stack-local buffers
       (textf, snprintf labels) remain valid until gui_render_flush consumes them. */

    char            text_pool[ GUI_MAX_TEXT_POOL ];
    u32             text_pool_used;

    u32             cmd_count;      /* commands in the list this frame  */
    u32             pt_count;       /* points in the pool this frame */

    gui_id_t        cur_win;        /* owning window id stamped onto new commands (set by begin/window_end) */
    u32             cur_z;          /* sort key tracked per-segment (draw_seg_retag; NOT baked per command) */
    u32             cur_vp;         /* viewport index stamped onto new commands (set by begin/window_end)  */
    u32             cur_font;       /* active font id tracked per-segment (draw_set_font); the segment is the
                                       font/atlas batch context -- text glyphs, fills (white texel) and dashed
                                       lines all resolve from it at tessellation time.  NOT baked per command. */

    /* Clip table: append-only per-frame pool of distinct scissor rects.  clip_push_clip_rect
       appends each intersected rect and records its index in clip_idx_stack so the active
       index (cur_clip_idx) is available O(1) at emit time -- no per-emit search. */

    gui_rect_t      clip_table      [ GUI_MAX_CLIP_RECTS ];     /* flat pool of all clip rects this frame   */
    u32             clip_hash_cache [ GUI_MAX_CLIP_RECTS ];     /* fnv1a of clip_table[i], baked when added */
    u32             clip_table_n;                               /* entries used this frame                  */
    u8              clip_idx_stack  [ GUI_CLIP_DEPTH ];  /* parallel to clip_stack: index per level  */
    u8              cur_clip_idx;                        /* top-of-stack index, stamped on each emit */

    gui_rect_t      clip_stack[ GUI_CLIP_DEPTH ];        /* intersected rects, mirrors clip_table   */
    u32             clip_depth;

    /* Global opacity multiplier applied to every pushed shape.  1.0 normally; lowered for the
       span of a disabled item so it dims with no per-widget code; reset by item / chrome seams. */
    f32 alpha;

    /* Ambient corner radius folded into every filled / outlined rect.  Set from the resolved
       rounding category (window / widget / grab) at the item / chrome seams and at the few sites
       that draw a different category; 0 emits square shapes (the fast path). */
    f32 rounding;

    /* Ambient horizontal text-clip window: glyph-level [x0, x1] hard-clip folded into every pushed
       text run that does not carry its own explicit window.  The sentinel (-/+ GUI_TEXT_NO_CLIP)
       means unclipped (the common path).  A seam that draws text into a bounded slot -- a table cell
       at the scroll/viewport edge -- sets it for the span so glyphs terminate cleanly at the slot
       edge instead of bleeding to the enclosing scissor (the self-fit rule at the glyph boundary). */
    f32 text_clip_x0;
    f32 text_clip_x1;

} s_draw;

/*----------------------------------------------------------------------------------------------
    FNV-1a hash helpers -- defined here (before draw_reset) so clip pre-hashing can use them.
    draw_hash_cmd below also uses them; fnv1a_u32 is visible in gui_build_cache.c (included
    after this file by gui_backend.c).
----------------------------------------------------------------------------------------------*/

static inline u32
fnv1a( u32 h, const void* p, u32 n )
{
    const u8* b = (const u8*)p;
    for ( u32 i = 0; i < n; ++i ) h = ( h ^ b[ i ] ) * 16777619u;
    return h;
}

/* fnv1a_u32 -- fold one u32 into h: 4 explicit byte mixes, no loop or function-call overhead.
   Used wherever a u32 value is the unit being folded (segment metadata, pre-baked clip hashes,
   per-command hash accumulation) so the inner loops in cache_diff_windows stay branch-free. */
static inline u32
fnv1a_u32( u32 h, u32 v )
{
    h = ( h ^ (u8)( v       ) ) * 16777619u;
    h = ( h ^ (u8)( v >>  8 ) ) * 16777619u;
    h = ( h ^ (u8)( v >> 16 ) ) * 16777619u;
    h = ( h ^ (u8)( v >> 24 ) ) * 16777619u;
    return h;
}

/*----------------------------------------------------------------------------------------------
    draw_reset -- call at the top of the frame (frame_begin)
----------------------------------------------------------------------------------------------*/

void
draw_reset( i32 display_w, i32 display_h )
{
    s_draw.cmd_count       = 0;
    s_draw.pt_count        = 0;
    s_draw.text_pool_used  = 0;
    s_draw.cur_win         = 0;   /* background; windows tag it via draw_set_window */
    s_draw.cur_z           = 0;   /* background; windows raise it via draw_set_sort_key */
    s_draw.cur_vp          = 0;   /* main viewport; windows route via draw_set_viewport */
    s_draw.cur_font        = font_active_id();   /* background segment inherits whatever font is active now */

    /* Open the first command segment: background (win 0, z 0, main viewport, active font), at command 0. */
    s_draw.seg_count       = 1;
    s_draw.segs[ 0 ]       = ( gui_cmd_seg_t ){ 0, 0, 0, s_draw.cur_font, 0, 0 };

    /* Seed the clip table: slot 0 = full display rect.  clip_idx_stack[0] and cur_clip_idx both
       start at 0 so every emitter finds the root clip without a push being required first. */
    s_draw.clip_table[ 0 ]      = ( gui_rect_t ){ 0.0f, 0.0f, (f32)display_w, (f32)display_h };
    s_draw.clip_hash_cache[ 0 ] = fnv1a( 2166136261u, &s_draw.clip_table[ 0 ], sizeof( gui_rect_t ) );
    s_draw.clip_stack[ 0 ]      = s_draw.clip_table[ 0 ];
    s_draw.clip_table_n         = 1;
    s_draw.clip_idx_stack[ 0 ]  = 0;
    s_draw.cur_clip_idx         = 0;
    s_draw.clip_depth           = 1;

    s_draw.alpha        = 1.0f;
    s_draw.rounding     = 0.0f;            /* square until a seam sets the resolved radius */
    s_draw.text_clip_x0 = -GUI_TEXT_NO_CLIP;  /* unclipped until a seam sets a window */
    s_draw.text_clip_x1 =  GUI_TEXT_NO_CLIP;
}

/*----------------------------------------------------------------------------------------------
    Clip stack
----------------------------------------------------------------------------------------------*/

/* Append r to the clip table and return its index.  On overflow (table full) the index saturates
   to the second-to-last slot -- commands share a slightly wrong clip rather than writing OOB.
   Slot GUI_MAX_CLIP_RECTS-1 is reserved as an invalid sentinel and is never written. */
static u8
clip_append( gui_rect_t r )
{
    if ( s_draw.clip_table_n < GUI_MAX_CLIP_RECTS - 1u )
    {
        u8 ci = (u8)s_draw.clip_table_n++;
        s_draw.clip_table      [ ci ] = r;
        s_draw.clip_hash_cache [ ci ] = fnv1a( 2166136261u, &r, sizeof( gui_rect_t ) );
        return ci;
    }
    return (u8)( GUI_MAX_CLIP_RECTS - 2u );
}

static gui_rect_t
draw_current_clip( void )
{
    return s_draw.clip_stack[ s_draw.clip_depth - 1 ];
}

/* A rect that bounds no pixels (rect_intersect clamps a missed overlap to zero w/h). */
static bool
rect_empty( gui_rect_t r )
{
    return r.w <= 0.0f || r.h <= 0.0f;
}

/* Reject a shape whose axis-aligned bounds cannot touch the current clip -- it would emit a command
   + geometry the GPU then scissors to nothing.  The cull is exact, not heuristic: cur_clip_idx
   records the active scissor at every emit, so draw_current_clip() IS the scissor the shape renders
   under; a box fully outside it lights no pixel.  This rejects at the source -- a scrolled-out widget
   (or a whole region clipped to zero) costs no command slot, no string-pool / point-pool space, and
   no tessellation, not merely no draw call.  Conservative: only a box fully past an edge is dropped
   (touching counts as visible), and an empty clip rejects everything in it. */
static bool
draw_cull_box( f32 x, f32 y, f32 w, f32 h )
{
    gui_rect_t c = draw_current_clip();
    if ( rect_empty( c ) )                  return true;   /* nothing in an empty clip is visible */
    if ( x + w <= c.x || x >= c.x + c.w )   return true;   /* fully left / right of the clip      */
    if ( y + h <= c.y || y >= c.y + c.h )   return true;   /* fully above / below the clip        */
    return false;
}

void
draw_push_clip_rect( f32 x, f32 y, f32 w, f32 h )
{
    /* Intersect with the enclosing clip so a nested region can never scissor outside its parent.
       The push always happens -- a clipped-out region pushes a zero rect and draws nothing --
       so every push has a matching pop and the stack stays balanced. */
    gui_rect_t c  = rect_intersect( ( gui_rect_t ){ x, y, w, h }, draw_current_clip() );
    u8         ci = clip_append( c );

    if ( s_draw.clip_depth < GUI_CLIP_DEPTH )
    {
        s_draw.clip_stack    [ s_draw.clip_depth ] = c;
        s_draw.clip_idx_stack[ s_draw.clip_depth ] = ci;
        ++s_draw.clip_depth;
    }
    s_draw.cur_clip_idx = ci;

    DBG_CLIP( c, s_draw.clip_depth );
}

void
draw_pop_clip_rect( void )
{
    if ( s_draw.clip_depth > 1 )
    {
        --s_draw.clip_depth;
        s_draw.cur_clip_idx = s_draw.clip_idx_stack[ s_draw.clip_depth - 1 ];
    }
}

/* Set the base clip (clip_stack[0]) -- the rect every window clip intersects against -- to the
   given surface size.  draw_reset seeds it to the main display; window_begin overwrites it with
   its viewport's drawable size (window_end restores the main display).  A fresh clip table entry
   is always appended: commands already emitted reference the old root index, so overwriting it
   would corrupt them.  clip_idx_stack[0] is updated so subsequent pushes inherit the new root. */
void
draw_set_root_clip( f32 w, f32 h )
{
    gui_rect_t r              = ( gui_rect_t ){ 0.0f, 0.0f, w, h };
    s_draw.clip_stack[ 0 ]    = r;
    u8 ci                     = clip_append( r );
    s_draw.clip_idx_stack[ 0 ] = ci;
    s_draw.cur_clip_idx        = ci;
}

/* Re-tag subsequent commands with (win, z, vp), cutting a new command segment at the boundary.  If
   the current segment is still empty (no command emitted since it opened) its tag is simply rewritten
   in place, so back-to-back set_window / set_sort_key / set_viewport calls before any draw never spawn
   empty spans.  On overflow the open segment is just extended (its tag may then be stale, but only in
   the pathological >1024-segment case, which cache_tess_window already falls back to natural order). */
static void
draw_seg_retag( gui_id_t win, u32 z, u32 vp, u32 font )
{
    if ( win == s_draw.cur_win && z == s_draw.cur_z && vp == s_draw.cur_vp && font == s_draw.cur_font )
        return;   /* no real change -- keep the open segment as is */

    /* No open segment yet -- called outside a frame (e.g. font_use during startup setup, before the
       first draw_reset).  Just track the tag; draw_reset re-seeds segs[0] from it next frame. */
    if ( s_draw.seg_count == 0 )
    {
        s_draw.cur_win  = win;
        s_draw.cur_z    = z;
        s_draw.cur_vp   = vp;
        s_draw.cur_font = font;
        return;
    }

    gui_cmd_seg_t* cur = &s_draw.segs[ s_draw.seg_count - 1 ];
    if ( cur->lo == s_draw.cmd_count )
    {
        cur->win  = win;   /* segment empty so far: retag in place rather than splitting */
        cur->z    = z;
        cur->vp   = vp;
        cur->font = font;
    }
    else if ( s_draw.seg_count < GUI_MAX_SEGS )
    {
        cur->hi                           = s_draw.cmd_count;   /* close the span here */
        s_draw.segs[ s_draw.seg_count++ ] =
            ( gui_cmd_seg_t ){ win, z, vp, font, s_draw.cmd_count, s_draw.cmd_count };
    }

    s_draw.cur_win  = win;
    s_draw.cur_z    = z;
    s_draw.cur_vp   = vp;
    s_draw.cur_font = font;
}

/*----------------------------------------------------------------------------------------------
    draw_set_window -- stamp subsequent commands with the owning window's stable id (the retained-
    cache key).  Set to win->id in window_begin and back to 0 (background) in window_end; the popup
    layer saves/restores it around an overlay just like the sort key.
----------------------------------------------------------------------------------------------*/

void
draw_set_window( gui_id_t win )
{
    draw_seg_retag( win, s_draw.cur_z, s_draw.cur_vp, s_draw.cur_font );
}

/*----------------------------------------------------------------------------------------------
    draw_set_font -- make `font` the atlas batch context for subsequent commands.  Cuts a new
    command segment at the boundary (a font change is a texture switch, hence a draw-batch seam),
    so the tessellator can re-activate the font for that span and every glyph / fill / dashed line
    in it samples the right atlas.  Driven by gui_font_use (push/pop/use_font) alongside the layout
    metric rebuild, so a second font on screen is a per-segment context, not per-command data.
----------------------------------------------------------------------------------------------*/

void
draw_set_font( u32 font )
{
    draw_seg_retag( s_draw.cur_win, s_draw.cur_z, s_draw.cur_vp, font );
}

gui_id_t
draw_window( void )
{
    return s_draw.cur_win;
}

/*----------------------------------------------------------------------------------------------
    draw_set_sort_key -- stamp subsequent commands with this z (window paint order).
    Set to the window's z in window_begin and back to 0 (background) in window_end.
----------------------------------------------------------------------------------------------*/

void
draw_set_sort_key( u32 z )
{
    draw_seg_retag( s_draw.cur_win, z, s_draw.cur_vp, s_draw.cur_font );
}

/* Current sort key -- saved by the popup layer so an overlay window can restore the parent's
   paint order on close (begin/window_end drive cur_z, which is a single global). */
u32
draw_sort_key( void )
{
    return s_draw.cur_z;
}

/*----------------------------------------------------------------------------------------------
    draw_set_viewport -- route subsequent commands to viewport `vp` (the surface a window paints).

    Set to the window's assigned viewport in window_begin and back to 0 (the main swapchain) in
    window_end, exactly as draw_set_sort_key drives the paint order.  flush replays only the
    commands tagged with its own viewport index, so one context can build every window's geometry
    and dispatch each window to the surface hosting it.
----------------------------------------------------------------------------------------------*/

void
draw_set_viewport( u32 vp )
{
    draw_seg_retag( s_draw.cur_win, s_draw.cur_z, vp, s_draw.cur_font );
}

/* Current viewport -- saved/restored by the popup layer alongside the sort key, so an overlay
   begun mid-window leaves the parent's routing intact (begin/window_end drive cur_vp globally). */
u32
draw_viewport( void )
{
    return s_draw.cur_vp;
}

/* draw_push_clip_root -- push the full-display clip (clip_stack[0]) as a fresh top, WITHOUT
   intersecting the current clip.  A popup is a top-level overlay: it must escape the enclosing
   window's clip, so the popup layer pushes this before opening the popup window (whose own clip
   then intersects the display, not the parent) and pops it after.  Balanced with draw_pop_clip_rect. */
void
draw_push_clip_root( void )
{
    if ( s_draw.clip_depth < GUI_CLIP_DEPTH )
    {
        s_draw.clip_stack    [ s_draw.clip_depth ] = s_draw.clip_stack    [ 0 ];
        s_draw.clip_idx_stack[ s_draw.clip_depth ] = s_draw.clip_idx_stack[ 0 ];
        ++s_draw.clip_depth;
        s_draw.cur_clip_idx = s_draw.clip_idx_stack[ 0 ];
    }
}

/*----------------------------------------------------------------------------------------------
    Global alpha -- a per-item opacity multiplier folded into every quad / triangle.

    draw_set_alpha installs the multiplier (clamped to [0,1]); draw_apply_alpha scales a packed
    color's A byte by it.  The item-flag resolver lowers it for the span of a disabled widget so
    the whole widget dims with no per-widget code, and the frame / chrome seams reset it to 1.0
    (chrome is not an item, so it always paints opaque).  At 1.0 the byte is returned unchanged.
----------------------------------------------------------------------------------------------*/

void
draw_set_alpha( f32 a )
{
    s_draw.alpha = a < 0.0f ? 0.0f : ( a > 1.0f ? 1.0f : a );
}

static u32
draw_apply_alpha( u32 abgr )
{
    if ( s_draw.alpha >= 1.0f ) return abgr;                /* opaque -- the common path */
    u32 a = ( abgr >> 24 ) & 0xFFu;
    a = (u32)( (f32)a * s_draw.alpha + 0.5f );              /* scale + round the alpha byte */
    return ( abgr & 0x00FFFFFFu ) | ( a << 24 );
}

/*----------------------------------------------------------------------------------------------
    Corner rounding -- the ambient radius folded into filled / outlined rects.

    draw_set_rounding installs the radius (clamped non-negative); draw_clamp_rounding fits it to a
    given rect so a corner arc never exceeds half a side, and treats a sub-pixel result as 0 so thin
    bars (separators, 1px frames) stay crisply square.  The item / chrome seams drive the radius from
    the resolved rounding category, exactly as draw_set_alpha is driven; grabs and squared marks set
    it locally for one sub-element via draw_set_rounding / draw_rounding (save + restore).
----------------------------------------------------------------------------------------------*/

void
draw_set_rounding( f32 r )
{
    s_draw.rounding = r < 0.0f ? 0.0f : r;
}

/* Current ambient radius -- so a site can save it, draw a sub-element with a different radius (a
   squared-off mark, a grab), and restore, without re-deriving the category. */
f32
draw_rounding( void )
{
    return s_draw.rounding;
}

/*----------------------------------------------------------------------------------------------
    Ambient text-clip window: a horizontal [x0, x1] pixel window that every subsequent
    draw_push_text / draw_push_text_n hard-clips to at the glyph level (straddling glyphs sliced
    with remapped U, interior glyphs whole, no scissor / no batch split).  A seam that draws text
    into a bounded slot -- a table cell at the scroll viewport edge -- sets the window for the
    span and clears it after, exactly as draw_set_alpha / draw_set_rounding bracket their spans.
    Explicit draw_push_text_clip_n callers (the scrolled text input) bypass this and pass their own.
----------------------------------------------------------------------------------------------*/

void
draw_set_text_clip_x( f32 x0, f32 x1 )
{
    s_draw.text_clip_x0 = x0;
    s_draw.text_clip_x1 = x1;
}

void
draw_clear_text_clip( void )
{
    s_draw.text_clip_x0 = -GUI_TEXT_NO_CLIP;
    s_draw.text_clip_x1 = GUI_TEXT_NO_CLIP;
}

static f32
draw_clamp_rounding( f32 w, f32 h )
{
    f32 r  = s_draw.rounding;
    f32 hw = ( w < 0.0f ? -w : w ) * 0.5f;
    f32 hh = ( h < 0.0f ? -h : h ) * 0.5f;
    if ( r > hw ) r = hw;
    if ( r > hh ) r = hh;
    return r < 0.5f ? 0.0f : r;   /* sub-pixel radius -> square fast path */
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_filled -- emit a filled / textured quad semantic command.

    tex_idx == 0 is the solid-color convention (resolved to the atlas white texel at tessellation
    time).  Pixel-grid snapping and GPU batching happen at flush time in the tessellation pass.
----------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------
    FNV-1a hash helper and per-command hash used by the retained cache.

    draw_hash_cmd hashes a fully-filled gui_cmd_t at emit time while the data is still
    L1-hot.  The hash is stored in s_draw.cmd_hashes and folded per window by
    cache_diff_windows (gui_build_cache.c) to detect frame-to-frame changes without
    re-scanning the command buffer after tessellation.

    TEXT and POLYLINE skip the pool-offset fields (text.off / polyline.pt_offset) because
    those values shift whenever an earlier-emitted window changes its pool volume, which would
    falsely dirty an unrelated window.  Their content bytes are folded directly instead.
----------------------------------------------------------------------------------------------*/

static u32
draw_hash_cmd( const gui_cmd_t* c )
{
    /* Fold type+vp (packed into one u32) then the pre-baked clip hash.  The clip value -- not
       the index -- is what matters so the same rect produces the same hash regardless of which
       table slot it occupies this frame.  clip_hash_cache[i] is baked at push time (4 bytes
       folded here vs 16 for the raw rect).  z is per-segment, folded in cache_diff_windows. */
    u32 h  = 2166136261u;
    u32 tv = (u32)c->type | ( (u32)c->vp << 8 );
    h = fnv1a_u32( h, tv );
    h = fnv1a_u32( h, s_draw.clip_hash_cache[ c->clip_idx ] );
    switch ( c->type )
    {
        case GUI_CMD_RECT_FILLED:   h = fnv1a( h, &c->rect,         sizeof c->rect );         break;
        case GUI_CMD_RECT_OUTLINE:  h = fnv1a( h, &c->rect_outline, sizeof c->rect_outline ); break;
        case GUI_CMD_TRIANGLE:      h = fnv1a( h, &c->tri,          sizeof c->tri );          break;
        case GUI_CMD_TEXT:
            h = fnv1a( h, &c->text.x,       sizeof c->text.x );
            h = fnv1a( h, &c->text.y,       sizeof c->text.y );
            h = fnv1a( h, &c->text.len,     sizeof c->text.len );
            h = fnv1a( h, &c->text.clip_x0, sizeof c->text.clip_x0 );
            h = fnv1a( h, &c->text.clip_x1, sizeof c->text.clip_x1 );
            h = fnv1a( h, &c->text.abgr,    sizeof c->text.abgr );
            h = fnv1a( h, s_draw.text_pool + c->text.off, c->text.len );   /* content while L1-hot */
            break;
        case GUI_CMD_CIRCLE_FILLED: h = fnv1a( h, &c->circle, sizeof c->circle ); break;
        case GUI_CMD_LINE:          h = fnv1a( h, &c->line,   sizeof c->line );   break;
        case GUI_CMD_POLYLINE:
            h = fnv1a( h, &c->polyline.pt_count,  sizeof c->polyline.pt_count );
            h = fnv1a( h, &c->polyline.thickness, sizeof c->polyline.thickness );
            h = fnv1a( h, &c->polyline.align,     sizeof c->polyline.align );
            h = fnv1a( h, &c->polyline.closed,    sizeof c->polyline.closed );
            h = fnv1a( h, &c->polyline.abgr,      sizeof c->polyline.abgr );
            h = fnv1a( h, &s_draw.points[ c->polyline.pt_offset ],
                       c->polyline.pt_count * (u32)sizeof( gui_vec2_t ) );   /* content while L1-hot */
            break;
        case GUI_CMD_DASHED_LINE:   h = fnv1a( h, &c->dash,     sizeof c->dash );     break;
        case GUI_CMD_RECT_GRADIENT: h = fnv1a( h, &c->gradient, sizeof c->gradient ); break;
    }
    return h;
}

void
draw_push_rect_filled( f32 x, f32 y, f32 w, f32 h,
                       f32 u0, f32 v0, f32 u1, f32 v1,
                       u32 tex_idx, u32 abgr )
{
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
        return;
    /* Fully transparent fills contribute nothing under alpha blending (src*0 + dst = dst).
       Drop them before they cost a command + triangles -- e.g. a hidden window body whose
       COL_WINDOW_BG was pushed to alpha 0, as the perf overlay does. Also covers a textured
       quad tinted to zero alpha, which is likewise invisible. */
    u32 col = draw_apply_alpha( abgr );
    if ( ( col >> 24 ) == 0u )
        return;
    if ( draw_cull_box( x, y, w, h ) )      /* scissored to nothing -- drop before it costs a slot */
        return;
    gui_cmd_t* c  = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type         = GUI_CMD_RECT_FILLED;
    c->clip_idx     = s_draw.cur_clip_idx;
    c->vp           = (u8)s_draw.cur_vp;
    c->rect.x       = x;
    c->rect.y       = y;
    c->rect.w       = w;
    c->rect.h       = h;
    c->rect.u0      = u0;
    c->rect.v0      = v0;
    c->rect.u1      = u1;
    c->rect.v1      = v1;
    c->rect.tex_idx = tex_idx;
    c->rect.abgr    = col;
    /* Round solid-color fills only; a textured quad (glyph / image) keeps square UVs. */
    c->rect.rounding = ( tex_idx == 0 ) ? draw_clamp_rounding( w, h ) : 0.0f;
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_gradient -- emit a two-color gradient rectangle as one semantic command.

    col_a / col_b sit on opposite edges (horizontal = left->right, else top->bottom); the GPU
    interpolates the per-vertex color between them, so one quad replaces the old banded fill.
    Always square (no rounding) -- the per-vertex blend has no rounded-fan variant.
----------------------------------------------------------------------------------------------*/

void
draw_push_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
        return;
    if ( draw_cull_box( x, y, w, h ) )
        return;
    gui_cmd_t* c          = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type                 = GUI_CMD_RECT_GRADIENT;
    c->clip_idx             = s_draw.cur_clip_idx;
    c->vp                   = (u8)s_draw.cur_vp;
    c->gradient.x           = x;
    c->gradient.y           = y;
    c->gradient.w           = w;
    c->gradient.h           = h;
    c->gradient.col_a       = draw_apply_alpha( col_a );
    c->gradient.col_b       = draw_apply_alpha( col_b );
    c->gradient.horizontal  = horizontal;
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
}

/*----------------------------------------------------------------------------------------------
    draw_push_rect_outline -- emit a hollow rectangle semantic command.
----------------------------------------------------------------------------------------------*/

void
draw_push_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 tex_idx, u32 abgr )
{
    (void)tex_idx;   /* outlines are always solid-color; tessellation uses the white texel */
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
        return;
    /* Skip a fully transparent border (e.g. the perf overlay pushes COL_BORDER to alpha 0). */
    u32 col = draw_apply_alpha( abgr );
    if ( ( col >> 24 ) == 0u )
        return;
    if ( draw_cull_box( x, y, w, h ) )
        return;
    gui_cmd_t* c       = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type              = GUI_CMD_RECT_OUTLINE;
    c->clip_idx          = s_draw.cur_clip_idx;
    c->vp                = (u8)s_draw.cur_vp;
    c->rect_outline.x    = x;
    c->rect_outline.y    = y;
    c->rect_outline.w    = w;
    c->rect_outline.h    = h;
    c->rect_outline.t    = t;
    c->rect_outline.abgr = col;
    c->rect_outline.rounding = draw_clamp_rounding( w, h );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
}

/*----------------------------------------------------------------------------------------------
    draw_push_triangle -- emit a solid triangle semantic command.
----------------------------------------------------------------------------------------------*/

void
draw_push_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 tex_idx, u32 abgr )
{
    (void)tex_idx;   /* triangles are always solid-color */
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
        return;
    /* Cull against the bounding box of the three vertices. */
    f32 minx = ax < bx ? ( ax < cx ? ax : cx ) : ( bx < cx ? bx : cx );
    f32 maxx = ax > bx ? ( ax > cx ? ax : cx ) : ( bx > cx ? bx : cx );
    f32 miny = ay < by ? ( ay < cy ? ay : cy ) : ( by < cy ? by : cy );
    f32 maxy = ay > by ? ( ay > cy ? ay : cy ) : ( by > cy ? by : cy );
    if ( draw_cull_box( minx, miny, maxx - minx, maxy - miny ) )
        return;
    gui_cmd_t* c = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type        = GUI_CMD_TRIANGLE;
    c->clip_idx    = s_draw.cur_clip_idx;
    c->vp          = (u8)s_draw.cur_vp;
    c->tri.ax      = ax; c->tri.ay = ay;
    c->tri.bx      = bx; c->tri.by = by;
    c->tri.cx      = cx; c->tri.cy = cy;
    c->tri.abgr    = draw_apply_alpha( abgr );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
}

/*----------------------------------------------------------------------------------------------
    draw_push_circle_filled -- emit a filled disc semantic command.
----------------------------------------------------------------------------------------------*/

void
draw_push_circle_filled( f32 cx, f32 cy, f32 r, u32 segments, u32 abgr )
{
    if ( segments < 3 ) segments = 3;
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
        return;
    if ( draw_cull_box( cx - r, cy - r, 2.0f * r, 2.0f * r ) )
        return;
    gui_cmd_t* c = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type        = GUI_CMD_CIRCLE_FILLED;
    c->clip_idx    = s_draw.cur_clip_idx;
    c->vp          = (u8)s_draw.cur_vp;
    c->circle.cx   = cx;
    c->circle.cy   = cy;
    c->circle.r    = r;
    c->circle.segs = segments;
    c->circle.abgr = draw_apply_alpha( abgr );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
}

/*----------------------------------------------------------------------------------------------
    draw_push_text -- emit a glyph-run semantic command.

    str must remain valid until gui_render_flush (same-frame caller-owned lifetime).
    n == 0xFFFFFFFF means "entire NUL-terminated string"; a smaller n limits the glyph count
    (used to skip "##label" suffixes).
----------------------------------------------------------------------------------------------*/

void
draw_push_text_clip_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    if ( !str || s_draw.cmd_count >= GUI_MAX_CMDS )
        return;

    /* Vertical cull: a glyph run lights pixels within roughly one line height of y, so if that band
       sits fully above or below the current clip the run is invisible -- a list row scrolled out of
       its box.  Padded a full line each way so ascenders / descenders are never wrongly dropped;
       horizontal overflow is left to the GPU scissor and the per-glyph clip in tess_text_n.  Done
       before the pool copy so a culled run costs no string-pool space either. */
    {
        gui_rect_t cc = draw_current_clip();
        f32          lh = font_line_h();
        if ( rect_empty( cc ) || y + 2.0f * lh <= cc.y || y - lh >= cc.y + cc.h )
            return;
    }

    /* Resolve length at push time (sentinel means NUL-terminated). */
    u32 len = ( n == 0xFFFFFFFFu ) ? (u32)strlen( str ) : n;

    /* Copy into the text pool so callers can use stack-local buffers (textf, snprintf labels).
       The pool pointer is valid until draw_reset clears it at the top of the next frame_begin. */
    if ( s_draw.text_pool_used + len + 1 > GUI_MAX_TEXT_POOL )
        return;   /* pool exhausted: drop the label rather than store a dangling pointer */
    u32   off = s_draw.text_pool_used;   /* offset stored in the cmd; pointer stays local */
    char* dst = s_draw.text_pool + off;
    memcpy( dst, str, len );
    dst[ len ]            = '\0';
    s_draw.text_pool_used += len + 1;

    gui_cmd_t* c  = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type         = GUI_CMD_TEXT;
    c->clip_idx     = s_draw.cur_clip_idx;
    c->vp           = (u8)s_draw.cur_vp;
    c->text.x       = x;
    c->text.y       = y;
    c->text.off     = off;
    c->text.len     = len;   /* always an explicit byte count; never 0xFFFFFFFF after this point */
    c->text.clip_x0 = clip_x0;
    c->text.clip_x1 = clip_x1;
    c->text.abgr    = draw_apply_alpha( abgr );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );   /* text bytes are L1-hot here */
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

// clang-format on
/*============================================================================================*/
