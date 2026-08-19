/*==============================================================================================

    runtime_service/gui/render/gui_step_capture.c -- Command stepper: frozen-frame capture + replay.

    Freeze one frame's semantic command list and replay a PREFIX of it, so the UI's generation
    can be stepped command by command.  The core trick: there is NO replay-specific tessellation.
    The restore pre-loads s_draw with the frozen prefix, so the ordinary pipeline -- hash diff,
    retained cache, per-window slots, z-sorted dispatch -- builds and renders it unchanged.  A
    cursor change alters the restored commands, the diff flags exactly the affected windows, and
    only those re-tessellate; an untouched frozen frame hash-matches and idle-skips like any
    clean frame.

    The feature splits into four tiers.  Each tier only ADDS state; a lower tier never reads an
    upper tier's data, so each can be understood (or deleted) top-down:

        T0  freeze / replay   cmds + hashes + segments + side pools; step_capture_build and
                              step_restore_emit (the two pipeline hooks), g_step_frozen
                              (the emit-suppression flag).  This alone is a working freezer.
        T1  display order     order / disp_segs / disp_pos: the SAME frozen data viewed in emit
                              order (generation) or paint order (z).  The cursor, the restore,
                              and every shell query live in whichever order is active.
        T2  inspection        step_cmd_bounds + the cmd/seg info queries: read-only decode of
                              one frozen command or segment for the stepper window.
        T3  picking           paint_seq + cmd_owner: topmost-command-under-point resolved in
                              TRUE paint order regardless of the display mode.

    Contracts the tiers share:

      - Requests LATCH and apply only at frame seams -- capture at the build, release / cursor /
        order at the next draw_reset -- so a frame is never half live, half frozen.  Every
        latched request raises s_step_pending, which frame_begin folds into frame_dirty
        (STEP_FRAME_PENDING): per-context wants_redraw is NOT reliable for this, since any later
        ctx_begin of the same frame wipes it and the request would stall behind the emit skip.

      - The capture copies band-0 segments only (the debug band is the stepper's own tooling and
        must never appear in its own replay), compacting commands/hashes/owners contiguously and
        rebasing each segment's [lo, hi).  Side pools are copied WHOLE so every pool offset
        baked into a command (text.off, pt_offset, rect_list.offset, clip_idx) stays valid
        verbatim.

      - While frozen, live band-0 pushes are suppressed at the source (STEP_EMIT_SUPPRESSED in
        gui_emit_draw.c), so the app keeps running underneath without disturbing the replay or
        the shared pools; only debug-band emission (this feature's own window, the dashboard,
        the overlays) lands on top.

      - Volatile widgets: cmd_volatile_id is NOT captured -- draw_reset zeroes it, so frozen
        volatile content replays as plain static commands and (unlike live frames) folds into
        its window's hash.  That first-freeze hash shift retessellates volatile-carrying windows
        once; from then on the frozen frame is static.  The live registry's rows go stale
        meanwhile (their tess_gen matches no slot, every patch refuses by the generation check)
        and self-heal on release, when live emit re-tags and forces a recapture.

    Included by gui_render.c LAST (after gui_dash_capture.c) so every emit static it copies
    (s_draw and its pools) is in scope.  Compiled out unless GUI_CMD_STEPPER (gui_render.h);
    the pipeline hooks compile to (void)0 then.

==============================================================================================*/
// clang-format off

#ifdef GUI_CMD_STEPPER

/*==============================================================================================
    State.

    Split by lifetime: request latches (written any time, consumed at a frame seam), the frozen
    frame (written ONCE per capture, read-only until release), and the derived order views
    (rebuilt from the frozen frame at capture / on a mode toggle).
==============================================================================================*/

/* Read by the emit hot path (STEP_EMIT_SUPPRESSED, via step_frozen()) to suppress live band-0
   pushes per frame. */
static bool g_step_frozen;

/* A latched request (capture / release / seek / order toggle) needs one more emit to take
   effect; frame_begin reads this through step_pending().  Cleared when the emit runs
   (step_restore_emit -- the restore consumes the cursor, and a same-frame capture request is
   taken by the build that follows). */
static bool s_step_pending;

static struct
{
    /* --- request latches + mode ------------------------------------------------------------ */
    bool want_capture;   /* latched by step_capture(); taken at the next cache_build_frame  */
    bool want_release;   /* latched by step_release(); applied at the next draw_reset       */
    u32  cursor;         /* replay shows the first `cursor` commands of the ACTIVE order        */
    bool paint_order;    /* display/replay order: false = emit (generation), true = paint (z)   */

    /* --- the frozen frame (T0) -- written once per capture ---------------------------------- */
    gui_cmd_t      cmds      [ GUI_MAX_CMDS ];          u32 cmd_count;
    u32            cmd_hashes[ GUI_MAX_CMDS ];          /* emit-time hashes, for the cache diff  */
    gui_id_t       cmd_owner [ GUI_MAX_CMDS ];          /* emitting widget (0 = chrome) -- T3    */
    gui_cmd_seg_t  segs      [ GUI_MAX_SEGS ];          u32 seg_count;   /* rebased [lo, hi)     */

    /* Side pools, copied whole (see the banner contract).  clip_hash mirrors s_draw's
       clip_hash_cache so the restore can hand both back without rehashing. */
    gui_rect_t     clip_table[ GUI_MAX_CLIP_RECTS ];
    u32            clip_hash [ GUI_MAX_CLIP_RECTS ];    u32 clip_n;
    gui_vec2_t     points    [ GUI_MAX_PATH_PTS ];      u32 pt_count;
    gui_rect_col_t rect_pool [ GUI_MAX_RECT_ENTRIES ];  u32 rect_count;
    char           text_pool [ GUI_MAX_TEXT_POOL ];     u32 text_used;

    /* --- derived order views (T1/T3) -- rebuilt, never captured ----------------------------- */

    /* The ACTIVE display order (step_build_order): order[k] is the frozen index of the k-th
       shown command; disp_segs[] are the frozen segments re-sequenced to match, [lo, hi)
       rebased into the display domain; disp_pos[] is order's inverse (frozen index -> display
       position).  Emit order is the identity; paint order concatenates segments sorted by z
       ascending (stable), approximating dispatch -- the true order also groups by clip within
       a window. */
    u32            order    [ GUI_MAX_CMDS ];
    gui_cmd_seg_t  disp_segs[ GUI_MAX_SEGS ];
    u32            disp_pos [ GUI_MAX_CMDS ];

    /* Paint order held SEPARATELY and built once at capture: the picker always resolves
       "topmost under the point" in paint order, whatever the display mode -- in emit order,
       later-emitted is not later-painted (the menu bar emits first yet sits on top). */
    u32            paint_seq[ GUI_MAX_CMDS ];

} s_step;

/*==============================================================================================
    T1 -- ordering.

    One expansion routine serves both sequences: pick the segment order (emit = identity,
    paint = stable z-sort), then lay the segments' commands out contiguously.  Both orders keep
    every segment's commands together, so the restore's segment walk is order-agnostic.
==============================================================================================*/

static void
step_expand_order( bool paint, u32* out_order, gui_cmd_seg_t* out_segs )
{
    /* Segment sequence: emit order (identity), or z-ascending (stable insertion sort). */
    u32 sidx[ GUI_MAX_SEGS ];
    for ( u32 i = 0; i < s_step.seg_count; ++i )
        sidx[ i ] = i;
    if ( paint )
        for ( u32 a = 1; a < s_step.seg_count; ++a )
        {
            u32 key = sidx[ a ];
            u32 b   = a;
            while ( b > 0 && s_step.segs[ sidx[ b - 1 ] ].z > s_step.segs[ key ].z )
            {
                sidx[ b ] = sidx[ b - 1 ];
                --b;
            }
            sidx[ b ] = key;
        }

    /* Expand into the command permutation (+ the rebased segment table, when wanted). */
    u32 w = 0;
    for ( u32 k = 0; k < s_step.seg_count; ++k )
    {
        const gui_cmd_seg_t* sg = &s_step.segs[ sidx[ k ] ];
        u32                  n  = sg->hi - sg->lo;
        if ( out_segs )
        {
            out_segs[ k ]    = *sg;
            out_segs[ k ].lo = (u16)w;
            out_segs[ k ].hi = (u16)( w + n );
        }
        for ( u32 j = 0; j < n; ++j )
            out_order[ w + j ] = sg->lo + j;
        w += n;
    }
}

/* (Re)build the ACTIVE display view: the order, its segment table, and its inverse map. */
static void
step_build_order( void )
{
    step_expand_order( s_step.paint_order, s_step.order, s_step.disp_segs );
    for ( u32 k = 0; k < s_step.cmd_count; ++k )
        s_step.disp_pos[ s_step.order[ k ] ] = k;
}

/*==============================================================================================
    Shell seam (gui_render.h) -- the request setters and state getters the stepper window and
    the debug hotkeys drive.  Setters only latch (+ raise s_step_pending); the pipeline hooks
    below apply them at the frame seams.
==============================================================================================*/

void step_capture( void )  { s_step.want_capture = true;  s_step_pending = true; }
void step_release( void )  { s_step.want_release = true;  s_step_pending = true; }
bool step_frozen ( void )  { return g_step_frozen; }
bool step_pending( void )  { return s_step_pending; }
bool step_paint_order( void )  { return s_step.paint_order; }
u32  step_count  ( void )  { return g_step_frozen ? s_step.cmd_count : 0; }
u32  step_cursor ( void )  { return s_step.cursor; }

void
step_seek( u32 cursor )
{
    s_step.cursor  = cursor < s_step.cmd_count ? cursor : s_step.cmd_count;
    s_step_pending = true;   /* set even for a same-value seek: the play transport leans on it */
}

void
step_set_paint_order( bool on )
{
    if ( s_step.paint_order == on )
        return;
    s_step.paint_order = on;
    s_step_pending     = true;
    if ( g_step_frozen )
        step_build_order();   /* cursor keeps its numeric position in the new order */
}

/*==============================================================================================
    T0 -- the two pipeline hooks: capture (build seam) and restore (emit seam).
==============================================================================================*/

/*==============================================================================================
    step_capture_build -- start of cache_build_frame: the final segment is closed, every pool is
    complete, nothing has been diffed or tessellated yet.  Copies the live frame and freezes.
==============================================================================================*/

void
step_capture_build( void )
{
    if ( !s_step.want_capture )
        return;
    s_step.want_capture = false;
    if ( g_step_frozen )
        return;   /* already frozen -- a second capture would snapshot the replay itself */

    /* Band-0 segments only, compacted (see the banner contract). */
    u32 w = 0, ns = 0;
    for ( u32 si = 0; si < s_draw.seg_count; ++si )
    {
        const gui_cmd_seg_t* sg = &s_draw.segs[ si ];
        if ( sg->band != 0 || sg->lo == sg->hi )
            continue;
        if ( ns >= GUI_MAX_SEGS - 1u )
            break;   /* keep one slot free for the restore's live tail span */
        u32 n = sg->hi - sg->lo;
        memcpy( s_step.cmds       + w, s_draw.cmds       + sg->lo, n * sizeof( gui_cmd_t ) );
        memcpy( s_step.cmd_hashes + w, s_draw.cmd_hashes + sg->lo, n * sizeof( u32 ) );
        memcpy( s_step.cmd_owner  + w, s_draw.cmd_owner  + sg->lo, n * sizeof( gui_id_t ) );
        s_step.segs[ ns ]    = *sg;
        s_step.segs[ ns ].lo = (u16)w;
        s_step.segs[ ns ].hi = (u16)( w + n );
        ++ns;
        w += n;
    }
    s_step.cmd_count = w;
    s_step.seg_count = ns;

    /* Side pools whole: pool offsets baked into the commands stay valid verbatim, and the pools
       may hold debug-band content interleaved -- unreferenced entries are inert. */
    memcpy( s_step.clip_table, s_draw.clip_table,      s_draw.clip_table_n * sizeof( gui_rect_t ) );
    memcpy( s_step.clip_hash,  s_draw.clip_hash_cache, s_draw.clip_table_n * sizeof( u32 ) );
    s_step.clip_n = s_draw.clip_table_n;
    memcpy( s_step.points,     s_draw.points,          s_draw.pt_count * sizeof( gui_vec2_t ) );
    s_step.pt_count = s_draw.pt_count;
    memcpy( s_step.rect_pool,  s_draw.rect_pool,       s_draw.rect_count * sizeof( gui_rect_col_t ) );
    s_step.rect_count = s_draw.rect_count;
    memcpy( s_step.text_pool,  s_draw.text_pool,       s_draw.text_pool_used );
    s_step.text_used = s_draw.text_pool_used;

    /* Derived views: the active display order, and the picker's fixed paint sequence. */
    step_build_order();
    step_expand_order( true, s_step.paint_seq, NULL );

    /* Freeze showing the whole frame: the first frozen build is visually identical to the live
       frame it replaces (only volatile-carrying windows retess once -- see the file banner). */
    s_step.cursor     = w;
    g_step_frozen = true;
}

/*==============================================================================================
    step_restore_emit -- end of draw_reset: the frame state is freshly seeded (background segment
    open, root clip at slot 0, volatile tags zeroed).  Applies a pending release, then while
    frozen replaces the empty frame with the frozen prefix.
==============================================================================================*/

void
step_restore_emit( void )
{
    s_step_pending = false;   /* the emit is running: every latched request is served this frame */

    if ( s_step.want_release )
    {
        s_step.want_release = false;
        g_step_frozen   = false;   /* this frame emits live again; the hash diff retesses any
                                          window whose live content differs from the frozen prefix */
    }
    if ( !g_step_frozen )
        return;

    u32 cur = s_step.cursor;   /* clamped at seek time */

    /* The visible command prefix THROUGH THE ACTIVE ORDER (emit or paint), with the emit-time
       hashes.  cmd_volatile_id stays zeroed (draw_reset's memset) -- frozen volatile content
       replays as plain static commands. */
    for ( u32 k = 0; k < cur; ++k )
    {
        u32 fi = s_step.order[ k ];
        s_draw.cmds      [ k ] = s_step.cmds      [ fi ];
        s_draw.cmd_hashes[ k ] = s_step.cmd_hashes[ fi ];
    }
    s_draw.cmd_count = cur;

    /* Side pools whole, so every pool offset in the prefix resolves; live debug-band emission
       appends past the frozen counts and the two never collide. */
    memcpy( s_draw.points,    s_step.points,    s_step.pt_count * sizeof( gui_vec2_t ) );
    memcpy( s_draw.rect_pool, s_step.rect_pool, s_step.rect_count * sizeof( gui_rect_col_t ) );
    memcpy( s_draw.text_pool, s_step.text_pool, s_step.text_used );
    s_draw.pt_count       = s_step.pt_count;
    s_draw.rect_count     = s_step.rect_count;
    s_draw.text_pool_used = s_step.text_used;

    /* Frozen clip table at its original indices, then ONE fresh live root appended after it:
       the display rect draw_reset just seeded, so the debug band scissors against the CURRENT
       surface size even if the window was resized while frozen. */
    u32 n = s_step.clip_n;
    memcpy( s_draw.clip_table,      s_step.clip_table, n * sizeof( gui_rect_t ) );
    memcpy( s_draw.clip_hash_cache, s_step.clip_hash,  n * sizeof( u32 ) );
    if ( n < GUI_MAX_CLIP_RECTS - 1u )
    {
        gui_rect_t root             = s_draw.clip_stack[ 0 ];
        s_draw.clip_table[ n ]      = root;
        s_draw.clip_hash_cache[ n ] = fnv1a( 2166136261u, &root, sizeof( gui_rect_t ) );
        s_draw.clip_idx_stack[ 0 ]  = (u8)n;
        s_draw.cur_clip_idx         = (u8)n;
        s_draw.clip_table_n         = n + 1;
    }
    else
    {
        s_draw.clip_idx_stack[ 0 ] = 0;   /* table full: fall back to the frozen root (slot 0) */
        s_draw.cur_clip_idx        = 0;
        s_draw.clip_table_n        = n;
    }

    /* Display-order segments clamped to the cursor (ordered by lo in the display domain by
       construction, so the scan stops at the first span past it), then the live tail: an open
       background-tagged span from `cur`, matching the ambient state draw_reset seeded -- the
       debug band re-tags from here. */
    u32 ns = 0;
    for ( u32 si = 0; si < s_step.seg_count; ++si )
    {
        if ( s_step.disp_segs[ si ].lo >= cur )
            break;
        s_draw.segs[ ns ] = s_step.disp_segs[ si ];
        if ( s_draw.segs[ ns ].hi > cur )
            s_draw.segs[ ns ].hi = (u16)cur;
        ++ns;
    }
    s_draw.segs[ ns++ ] = ( gui_cmd_seg_t ){ .lo = (u16)cur, .hi = (u16)cur };
    s_draw.seg_count    = ns;
}

/*==============================================================================================
    T2 -- inspection: read-only decode of one frozen command / segment for the shell
    (gui_step_window.c).  Lives backend-side because the frozen pools do: polyline points,
    rect-list entries and text bytes are only reachable here.  Query cost is a linear scan or a
    pool walk -- fine for a handful of shell queries per dirty frame, never per emit.
==============================================================================================*/

/* Point-in-rect, half-open on the far edges -- the one hit rule pick uses for clip, bounds and
   the outline hole below, so all three tests agree on boundary pixels. */
static inline bool
step_hit( gui_rect_t r, f32 x, f32 y )
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Pixel bbox of one frozen command -- the highlight box the shell outlines.  Fills/outlines are
   exact; strokes pad by half thickness; TEXT walks the ACTIVE font's advances (a run frozen in
   another font measures approximately -- acceptable for a highlight aid). */
static gui_rect_t
step_cmd_bounds( const gui_cmd_t* c )
{
    switch ( (gui_cmd_type_t)c->type )
    {
        case GUI_CMD_RECT_FILLED:
            return ( gui_rect_t ){ c->rect.x, c->rect.y, c->rect.w, c->rect.h };
        case GUI_CMD_RECT_OUTLINE:
            return ( gui_rect_t ){ c->rect_outline.x, c->rect_outline.y,
                                   c->rect_outline.w, c->rect_outline.h };
        case GUI_CMD_FRAME:
            return ( gui_rect_t ){ c->frame.x, c->frame.y, c->frame.w, c->frame.h };
        case GUI_CMD_RECT_GRADIENT:
            return ( gui_rect_t ){ c->gradient.x, c->gradient.y, c->gradient.w, c->gradient.h };
        /* The soft skirt is real painted area, so the highlight has to cover it -- a shadow
           outlined at its box alone looks like the highlight is the thing that is wrong. */
        case GUI_CMD_FX_BOX:
        {
            f32 g = c->fx_box.feather * 0.5f;
            if ( c->fx_box.rot != 0.0f )
            {
                /* Rotated: the rotated AABB of the grown box, the emit-side cull's arithmetic. */
                f32 cs = cosf( c->fx_box.rot ), sn = sinf( c->fx_box.rot );
                f32 hx = c->fx_box.w * 0.5f + g, hy = c->fx_box.h * 0.5f + g;
                f32 ex = fabsf( hx * cs ) + fabsf( hy * sn );
                f32 ey = fabsf( hx * sn ) + fabsf( hy * cs );
                return ( gui_rect_t ){ c->fx_box.x + c->fx_box.w * 0.5f - ex,
                                       c->fx_box.y + c->fx_box.h * 0.5f - ey,
                                       ex * 2.0f, ey * 2.0f };
            }
            return ( gui_rect_t ){ c->fx_box.x - g, c->fx_box.y - g,
                                   c->fx_box.w + 2.0f * g, c->fx_box.h + 2.0f * g };
        }
        case GUI_CMD_ROUND_RECT_EX:
        {
            /* Grown by the feather like FX_BOX: the soft skirt is real painted area. */
            f32 g = c->round_rect.feather * 0.5f;
            return ( gui_rect_t ){ c->round_rect.x - g, c->round_rect.y - g,
                                   c->round_rect.w + 2.0f * g, c->round_rect.h + 2.0f * g };
        }
        /* The whole circle, not the sector: a highlight that over-covers still points at the right
           shape, and the tight extent would need the rotated local box rebuilt here. */
        case GUI_CMD_ARC:
        case GUI_CMD_PIE:
        {
            f32 g = c->arc.r + c->arc.thickness * 0.5f;
            return ( gui_rect_t ){ c->arc.cx - g, c->arc.cy - g, g * 2.0f, g * 2.0f };
        }
        case GUI_CMD_ARC_DASH:
        {
            f32 g = c->arc_dash.r + c->arc_dash.thickness * 0.5f;
            return ( gui_rect_t ){ c->arc_dash.cx - g, c->arc_dash.cy - g, g * 2.0f, g * 2.0f };
        }
        case GUI_CMD_ARC_GRAD:
        {
            f32 g = c->arc_grad.r + c->arc_grad.thickness * 0.5f;
            return ( gui_rect_t ){ c->arc_grad.cx - g, c->arc_grad.cy - g, g * 2.0f, g * 2.0f };
        }
        /* The pattern quads paint exactly their box -- the tiling is inside it. */
        case GUI_CMD_CHECKER:
            return ( gui_rect_t ){ c->checker.x, c->checker.y, c->checker.w, c->checker.h };
        case GUI_CMD_GRID:
            return ( gui_rect_t ){ c->grid.x, c->grid.y, c->grid.w, c->grid.h };
        case GUI_CMD_NGON:
        {
            f32 g = c->ngon.r;
            return ( gui_rect_t ){ c->ngon.cx - g, c->ngon.cy - g, g * 2.0f, g * 2.0f };
        }
        case GUI_CMD_BOX_DASH:
            return ( gui_rect_t ){ c->box_dash.x, c->box_dash.y, c->box_dash.w, c->box_dash.h };
        /* Sprite / nine-slice paints exactly its box -- the slice expansion happens inside it. */
        case GUI_CMD_SPRITE:
            return ( gui_rect_t ){ c->sprite.x, c->sprite.y, c->sprite.w, c->sprite.h };
        /* The rotated AABB, exactly as the emit-side cull computes it. */
        case GUI_CMD_IMAGE_XF:
        {
            f32 cs = cosf( c->image_xf.rot ), sn = sinf( c->image_xf.rot );
            f32 hx = c->image_xf.w * 0.5f, hy = c->image_xf.h * 0.5f;
            f32 ex = fabsf( hx * cs ) + fabsf( hy * sn );
            f32 ey = fabsf( hx * sn ) + fabsf( hy * cs );
            return ( gui_rect_t ){ c->image_xf.x + hx - ex, c->image_xf.y + hy - ey,
                                   ex * 2.0f, ey * 2.0f };
        }
        case GUI_CMD_TRIANGLE:
        {
            f32 x0 = c->tri.ax, x1 = c->tri.ax, y0 = c->tri.ay, y1 = c->tri.ay;
            if ( c->tri.bx < x0 ) x0 = c->tri.bx;   if ( c->tri.bx > x1 ) x1 = c->tri.bx;
            if ( c->tri.cx < x0 ) x0 = c->tri.cx;   if ( c->tri.cx > x1 ) x1 = c->tri.cx;
            if ( c->tri.by < y0 ) y0 = c->tri.by;   if ( c->tri.by > y1 ) y1 = c->tri.by;
            if ( c->tri.cy < y0 ) y0 = c->tri.cy;   if ( c->tri.cy > y1 ) y1 = c->tri.cy;
            return ( gui_rect_t ){ x0, y0, x1 - x0, y1 - y0 };
        }
        case GUI_CMD_TEXT:
        {
            const char* s = s_step.text_pool + c->text.off;
            f32         w = 0.0f;
            u32         i = 0;
            while ( i < c->text.len && s[ i ] )
            {
                u32 adv_b;
                f32 u0, v0, u1, v1, ox, oy, gw, gh, adv;
                font_glyph( utf8_decode( &s[ i ], &adv_b ),
                            &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &adv );
                w += adv;
                i += adv_b;
            }
            /* Fold the glyph-level hard-clip window in, when one was baked. */
            f32 x0 = c->text.x, x1 = c->text.x + w;
            if ( c->text.clip_x0 > x0 ) x0 = c->text.clip_x0;
            if ( c->text.clip_x1 < x1 ) x1 = c->text.clip_x1;
            return ( gui_rect_t ){ x0, c->text.y, x1 > x0 ? x1 - x0 : 0.0f, font_line_h() };
        }
        /* Same walk, over the run text_shadow points at; the shadow's own dx/dy offset just
           pads the box so the highlight still frames the shadow copy. */
        case GUI_CMD_TEXT_SHADOW:
        {
            const char* s = s_step.text_pool + c->text_shadow.off;
            f32         w = 0.0f;
            u32         i = 0;
            while ( i < c->text_shadow.len && s[ i ] )
            {
                u32 adv_b;
                f32 u0, v0, u1, v1, ox, oy, gw, gh, adv;
                font_glyph( utf8_decode( &s[ i ], &adv_b ),
                            &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &adv );
                w += adv;
                i += adv_b;
            }
            f32 dx = c->text_shadow.dx, dy = c->text_shadow.dy;
            f32 x0 = c->text_shadow.x + ( dx < 0.0f ? dx : 0.0f );
            f32 x1 = c->text_shadow.x + w + ( dx > 0.0f ? dx : 0.0f );
            f32 y0 = c->text_shadow.y + ( dy < 0.0f ? dy : 0.0f );
            f32 y1 = c->text_shadow.y + font_line_h() + ( dy > 0.0f ? dy : 0.0f );
            if ( c->text_shadow.clip_x0 > x0 ) x0 = c->text_shadow.clip_x0;
            if ( c->text_shadow.clip_x1 < x1 ) x1 = c->text_shadow.clip_x1;
            return ( gui_rect_t ){ x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 - y0 };
        }
        /* Transformed run: the same advance walk gives the run's own box, which is then rotated
           about the origin and re-bounded -- the highlight is an AABB over a shape that is not
           one, exactly as it is for a triangle. */
        case GUI_CMD_TEXT_XF:
        {
            const char* s  = s_step.text_pool + c->text_xf.off;
            f32         w  = 0.0f;
            u32         bi = 0;
            while ( bi < c->text_xf.len && s[ bi ] )
            {
                u32 adv_b;
                f32 u0, v0, u1, v1, ox, oy, gw, gh, adv;
                font_glyph( utf8_decode( &s[ bi ], &adv_b ),
                            &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &adv );
                w += adv;
                bi += adv_b;
            }
            f32 cs = cosf( c->text_xf.rot ), sn = sinf( c->text_xf.rot );
            f32 lw = w * c->text_xf.scale, lh = font_line_h() * c->text_xf.scale;
            f32 qx[ 4 ] = { 0.0f, lw, lw,   0.0f };
            f32 qy[ 4 ] = { 0.0f, 0.0f, lh, lh   };
            f32 x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
            for ( u32 i = 0; i < 4; ++i )
            {
                f32 px = qx[ i ] * cs - qy[ i ] * sn;
                f32 py = qx[ i ] * sn + qy[ i ] * cs;
                if ( px < x0 ) x0 = px;   if ( px > x1 ) x1 = px;
                if ( py < y0 ) y0 = py;   if ( py > y1 ) y1 = py;
            }
            return ( gui_rect_t ){ c->text_xf.x + x0, c->text_xf.y + y0, x1 - x0, y1 - y0 };
        }
        case GUI_CMD_LINE:
        case GUI_CMD_DASHED_LINE:
        {
            /* line and dash share the same leading x0..thickness layout. */
            f32 t  = c->line.thickness * 0.5f + 1.0f;
            f32 x0 = c->line.x0 < c->line.x1 ? c->line.x0 : c->line.x1;
            f32 x1 = c->line.x0 < c->line.x1 ? c->line.x1 : c->line.x0;
            f32 y0 = c->line.y0 < c->line.y1 ? c->line.y0 : c->line.y1;
            f32 y1 = c->line.y0 < c->line.y1 ? c->line.y1 : c->line.y0;
            return ( gui_rect_t ){ x0 - t, y0 - t, ( x1 - x0 ) + 2.0f * t, ( y1 - y0 ) + 2.0f * t };
        }
        case GUI_CMD_POLYLINE:
        {
            const gui_vec2_t* p = s_step.points + c->polyline.pt_offset;
            f32 x0 = p[ 0 ].x, x1 = p[ 0 ].x, y0 = p[ 0 ].y, y1 = p[ 0 ].y;
            for ( u32 i = 1; i < c->polyline.pt_count; ++i )
            {
                if ( p[ i ].x < x0 ) x0 = p[ i ].x;   if ( p[ i ].x > x1 ) x1 = p[ i ].x;
                if ( p[ i ].y < y0 ) y0 = p[ i ].y;   if ( p[ i ].y > y1 ) y1 = p[ i ].y;
            }
            f32 t = c->polyline.thickness * 0.5f + 1.0f;
            return ( gui_rect_t ){ x0 - t, y0 - t, ( x1 - x0 ) + 2.0f * t, ( y1 - y0 ) + 2.0f * t };
        }
        case GUI_CMD_RECT_LIST:
        {
            const gui_rect_col_t* r = s_step.rect_pool + c->rect_list.offset;
            f32 x0 = r[ 0 ].x, y0 = r[ 0 ].y, x1 = r[ 0 ].x + r[ 0 ].w, y1 = r[ 0 ].y + r[ 0 ].h;
            for ( u32 i = 1; i < c->rect_list.count; ++i )
            {
                if ( r[ i ].x < x0 )            x0 = r[ i ].x;
                if ( r[ i ].y < y0 )            y0 = r[ i ].y;
                if ( r[ i ].x + r[ i ].w > x1 ) x1 = r[ i ].x + r[ i ].w;
                if ( r[ i ].y + r[ i ].h > y1 ) y1 = r[ i ].y + r[ i ].h;
            }
            return ( gui_rect_t ){ x0, y0, x1 - x0, y1 - y0 };
        }
    }
    return ( gui_rect_t ){ 0.0f, 0.0f, 0.0f, 0.0f };
}

bool
step_cmd_info( u32 index, step_cmd_info_t* out )
{
    if ( !g_step_frozen || index >= s_step.cmd_count )
        return false;

    /* `index` is a DISPLAY position; resolve it through the active order. */
    u32              fi = s_step.order[ index ];
    const gui_cmd_t* c  = &s_step.cmds[ fi ];
    out->cmd    = *c;
    out->bounds = step_cmd_bounds( c );
    out->clip   = s_step.clip_table[ c->clip_idx ];
    out->text   = ( c->type == GUI_CMD_TEXT )        ? s_step.text_pool + c->text.off
                : ( c->type == GUI_CMD_TEXT_XF )     ? s_step.text_pool + c->text_xf.off
                : ( c->type == GUI_CMD_TEXT_SHADOW ) ? s_step.text_pool + c->text_shadow.off
                                                     : NULL;
    out->owner  = s_step.cmd_owner[ fi ];

    /* The font is the COMMAND's own now (gui.h), and only a glyph run has one. */
    out->font = ( c->type == GUI_CMD_TEXT )        ? c->text.font
              : ( c->type == GUI_CMD_TEXT_XF )     ? c->text_xf.font
              : ( c->type == GUI_CMD_TEXT_SHADOW ) ? c->text_shadow.font
                                                   : 0u;

    /* Owning segment tag (display domain). */
    out->win = 0;  out->z = 0;  out->vp = 0;
    for ( u32 si = 0; si < s_step.seg_count; ++si )
        if ( index >= s_step.disp_segs[ si ].lo && index < s_step.disp_segs[ si ].hi )
        {
            out->win  = s_step.disp_segs[ si ].win;
            out->z    = s_step.disp_segs[ si ].z;
            out->vp   = s_step.disp_segs[ si ].vp;
            break;
        }
    return true;
}

u32
step_seg_count( void )
{
    return g_step_frozen ? s_step.seg_count : 0;
}

bool
step_seg_info( u32 index, step_seg_info_t* out )
{
    if ( !g_step_frozen || index >= s_step.seg_count )
        return false;

    /* Display-domain segment: the list shows (and seeks in) the active order. */
    const gui_cmd_seg_t* sg = &s_step.disp_segs[ index ];
    out->win  = sg->win;
    out->z    = sg->z;
    out->vp   = sg->vp;
    out->lo   = sg->lo;
    out->hi   = sg->hi;

    /* Bounds: union of the member commands' bboxes (empty boxes contribute nothing). */
    gui_rect_t u = ( gui_rect_t ){ 0.0f, 0.0f, 0.0f, 0.0f };
    for ( u32 i = sg->lo; i < sg->hi; ++i )
    {
        gui_rect_t b = step_cmd_bounds( &s_step.cmds[ s_step.order[ i ] ] );
        if ( b.w <= 0.0f || b.h <= 0.0f )
            continue;
        if ( u.w <= 0.0f )
            u = b;
        else
        {
            f32 x1 = u.x + u.w > b.x + b.w ? u.x + u.w : b.x + b.w;
            f32 y1 = u.y + u.h > b.y + b.h ? u.y + u.h : b.y + b.h;
            if ( b.x < u.x ) u.x = b.x;
            if ( b.y < u.y ) u.y = b.y;
            u.w = x1 - u.x;
            u.h = y1 - u.y;
        }
    }
    out->bounds = u;
    return true;
}

/*==============================================================================================
    T3 -- picking: "what drew this pixel".

    Always walks in PAINT order (paint_seq, top-down), whatever the display mode: in emit order
    the last-emitted command is not the last-painted one (the menu bar emits first yet sits on
    top), so a display-order walk picked whatever full-screen fill happened to emit later.

    The walk applies four rules, in order, per candidate:
      1. visible   -- its display position is under the cursor (disp_pos < cursor);
      2. scissored -- its frozen clip must contain the point (a clipped-away shape never claims);
      3. shaped    -- its bounds contain the point, where a hollow RECT_OUTLINE owns only its
                      border band (a window border must not swallow every click in the window);
      4. decisive  -- the FIRST hit ends the walk.  Widget-owned commands pick; unattributed
                      chrome/background (owner 0) REFUSES rather than tunnelling to content
                      underneath, so a click that misses a widget is a no-op, not a seek onto
                      the window's body fill that hides everything painted after it.  Chrome
                      stays inspectable via the scrubber and the segment list.
==============================================================================================*/

bool
step_pick( f32 x, f32 y, i32 vp, u32* out_index )
{
    if ( !g_step_frozen )
        return false;

    for ( u32 t = s_step.cmd_count; t-- > 0; )
    {
        u32 fi = s_step.paint_seq[ t ];
        u32 k  = s_step.disp_pos[ fi ];
        if ( k >= s_step.cursor )
            continue;   /* rule 1: not visible at the current cursor */

        const gui_cmd_t* c = &s_step.cmds[ fi ];
        if ( c->vp != vp )
            continue;
        if ( !step_hit( s_step.clip_table[ c->clip_idx ], x, y ) )
            continue;   /* rule 2: the frozen scissor excludes the point */

        gui_rect_t b = step_cmd_bounds( c );
        if ( b.w <= 0.0f || !step_hit( b, x, y ) )
            continue;   /* rule 3: outside the shape */

        /* rule 3, hollow case: a point inside an outline's hole falls through to whatever it
           frames.  The band is padded by 1px so a thin ring is still clickable. */
        if ( c->type == GUI_CMD_RECT_OUTLINE )
        {
            f32 band = c->rect_outline.t + 1.0f;
            gui_rect_t hole = ( gui_rect_t ){ b.x + band, b.y + band,
                                              b.w - 2.0f * band, b.h - 2.0f * band };
            if ( hole.w > 0.0f && step_hit( hole, x, y ) )
                continue;
        }

        /* rule 4: first hit decides. */
        if ( s_step.cmd_owner[ fi ] == 0 )
            return false;   /* topmost hit is chrome/background: refuse, do not tunnel */
        *out_index = k;
        return true;
    }
    return false;
}

#endif /* GUI_CMD_STEPPER */

// clang-format on
/*============================================================================================*/
