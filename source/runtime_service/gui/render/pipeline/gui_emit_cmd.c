/*==============================================================================================
    gui/render/pipeline/gui_emit_cmd.c -- The command record: claim, stamp, hash, seal.

    The machinery every draw_push_* shares. draw_cmd_claim spends a slot and stamps the 
    ambient (clip_idx, vp) pair; draw_cmd_open runs the gates that decide whether a slot
    may be spent at all; draw_cmd_seal bakes the retained-cache hash while the payload 
    is still L1-hot.

    gui_emit_state.c (included before this file) owns s_draw and the fnv1a helpers this
    folds through.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    command hashing -- FNV-1a hash helper and per-command hash used by the retained cache.

    draw_hash_cmd hashes a fully-filled gui_cmd_t at emit time while the data is still
    L1-hot. The hash is stored in s_draw.cmd_hashes and folded per window by
    cache_diff_windows (gui_build_cache.c) to detect frame-to-frame changes without
    re-scanning the command buffer after tessellation.

    TEXT, POLYLINE and RECT_LIST skip the pool-offset fields (text.off / polyline.pt_offset /
    rect_list.offset) because those values shift whenever an earlier-emitted window changes its
    pool volume, which would falsely dirty an unrelated window.  Their content bytes are folded
    directly instead.

==============================================================================================*/

/* Payload byte count per command type -- the ONE table both draw_hash_cmd's default fold and
   draw_cmd_claim_ext's pool allocation read from.  For most types that is the whole story: one
   fnv1a fold of the union member at hash time, and the exact bytes draw_cmd_claim_ext reserves
   in s_draw.cmd_pool at push time.  Five entries (TEXT, TEXT_XF, TEXT_SHADOW, POLYLINE,
   RECT_LIST) still take their own switch case in draw_hash_cmd below despite having an entry
   here: their struct carries a pool-offset field (text.off / polyline.pt_offset /
   rect_list.offset) into a SEPARATE content pool, which shifts whenever an earlier-emitted
   window changes that pool's volume and would falsely dirty an unrelated window if folded raw
   -- so hashing must skip the offset and fold the pointed-to content instead.  Their entry here
   still names the right allocation size; only the hash needs the special case. */

static const u8 k_cmd_hash_len[ GUI_CMD_COUNT ] = {

    [GUI_CMD_RECT_FILL]     = sizeof(( (gui_cmd_ext_t*)0 )->rect_fill ),

    [GUI_CMD_TEXT]          = sizeof(( (gui_cmd_ext_t*)0 )->text ),
    [GUI_CMD_TEXT_XF]       = sizeof(( (gui_cmd_ext_t*)0 )->text_xf ),
    [GUI_CMD_TEXT_SHADOW]   = sizeof(( (gui_cmd_ext_t*)0 )->text_shadow ),
    [GUI_CMD_POLYLINE]      = sizeof(( (gui_cmd_ext_t*)0 )->polyline ),
    [GUI_CMD_RECT_LIST]     = sizeof(( (gui_cmd_ext_t*)0 )->rect_list ),

    [GUI_CMD_RECT_TEX]      = sizeof(( (gui_cmd_ext_t*)0 )->rect_tex ),
    [GUI_CMD_RECT_OUTLINE]  = sizeof(( (gui_cmd_ext_t*)0 )->rect_outline ),
    [GUI_CMD_FRAME]         = sizeof(( (gui_cmd_ext_t*)0 )->frame ),
    [GUI_CMD_TRIANGLE]      = sizeof(( (gui_cmd_ext_t*)0 )->tri ),
    [GUI_CMD_BEZIER]        = sizeof(( (gui_cmd_ext_t*)0 )->bezier ),
    [GUI_CMD_LINE]          = sizeof(( (gui_cmd_ext_t*)0 )->line ),
    [GUI_CMD_DASHED_LINE]   = sizeof(( (gui_cmd_ext_t*)0 )->dash ),
    [GUI_CMD_RECT_GRADIENT] = sizeof(( (gui_cmd_ext_t*)0 )->gradient ),
    [GUI_CMD_SPRITE]        = sizeof(( (gui_cmd_ext_t*)0 )->sprite ),

    /* Folds rate whole, so a pulse hashes stable frame-to-frame (it animates in the FRAGMENT off
       pc.time -- the geometry never changes, which is the entire point of the mode). */
    [GUI_CMD_FX_BOX]        = sizeof(( (gui_cmd_ext_t*)0 )->fx_box ),
    [GUI_CMD_ROUND_RECT_EX] = sizeof(( (gui_cmd_ext_t*)0 )->round_rect ),
    [GUI_CMD_ROUND_FRAME_EX] = sizeof(( (gui_cmd_ext_t*)0 )->round_frame_ex ),

    /* Both sectors fold the same member.  A spinner's start angle moves every frame, so this
       dirties every frame -- honestly, since the geometry really does rotate.  (A spinner that
       wanted free animation would be a shader-clock mode like PULSE, not a re-emit.) */

    [GUI_CMD_ARC]           = sizeof(( (gui_cmd_ext_t*)0 )->arc ),
    [GUI_CMD_PIE]           = sizeof(( (gui_cmd_ext_t*)0 )->arc ),
    [GUI_CMD_ARC_DASH]      = sizeof(( (gui_cmd_ext_t*)0 )->arc_dash ),
    [GUI_CMD_ARC_GRAD]      = sizeof(( (gui_cmd_ext_t*)0 )->arc_grad ),
    [GUI_CMD_IMAGE_XF]      = sizeof(( (gui_cmd_ext_t*)0 )->image_xf ),
    [GUI_CMD_CHECKER]       = sizeof(( (gui_cmd_ext_t*)0 )->checker ),
    [GUI_CMD_GRID]          = sizeof(( (gui_cmd_ext_t*)0 )->grid ),
    [GUI_CMD_NGON]          = sizeof(( (gui_cmd_ext_t*)0 )->ngon ),

    /* Folds rate/phase whole like FX_BOX: the ants scroll in the fragment off pc.time, so the
       command hashes stable frame-to-frame while the pattern moves. */

    [GUI_CMD_BOX_DASH]      = sizeof(( (gui_cmd_ext_t*)0 )->box_dash ),
    [GUI_CMD_REPEAT]        = sizeof(( (gui_cmd_ext_t*)0 )->repeat ),

    /* Folds rate/phase whole like FX_BOX: the ring spins in the fragment off pc.time, so the
       command hashes stable frame-to-frame while it turns. */

    [GUI_CMD_REPEAT_POLAR]  = sizeof(( (gui_cmd_ext_t*)0 )->repeat_polar ),
    [GUI_CMD_BOX_CUT]       = sizeof(( (gui_cmd_ext_t*)0 )->box_cut ),
};

static u32
draw_hash_cmd( const gui_cmd_t* c )
{
    /* Fold type + vp (packed into one u32) then the pre-baked clip hash.  
       The clip value -- not the index -- is what matters so the same rect produces the same
       hash regardless of which table slot it occupies this frame.  
       
       clip_hash_cache[i] is baked at push time (4 bytes folded here vs 16 for the raw rect).
       z is per-segment, folded in cache_diff_windows. */

    u32 h  = 2166136261u;
    u32 tv = (u32)c->type | ( (u32)c->vp << 8 );
    h = fnv1a_u32( h, tv );
    h = fnv1a_u32( h, s_draw.clip_hash_cache[ c->clip_idx ] );

    switch ( c->type )
    {
        case GUI_CMD_TEXT:
        {
            const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
            h = fnv1a( h, &e->text.x,           sizeof e->text.x );
            h = fnv1a( h, &e->text.y,           sizeof e->text.y );
            h = fnv1a( h, &e->text.len,         sizeof e->text.len );
            h = fnv1a( h, &e->text.clip_x0,     sizeof e->text.clip_x0 );
            h = fnv1a( h, &e->text.clip_x1,     sizeof e->text.clip_x1 );
            h = fnv1a( h, &e->text.abgr,        sizeof e->text.abgr );
            h = fnv1a( h, &e->text.edge_w,      sizeof e->text.edge_w );
            h = fnv1a( h, &e->text.edge_col,    sizeof e->text.edge_col );
            h = fnv1a( h, &e->text.font,        sizeof e->text.font );
            h = fnv1a( h, s_draw.text_pool + e->text.off, e->text.len );   /* content while L1-hot */
            break;
        }

        /* Folds scale and rot, so a run that spins re-tessellates every frame it moves.  That is
           the honest cost and the difference from PULSE: a pulse animates in the FRAGMENT off
           pc.time and its geometry never changes, while a transform is baked into vertices. */

        case GUI_CMD_TEXT_XF:
        {
            const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
            h = fnv1a( h, &e->text_xf.x,        sizeof e->text_xf.x );
            h = fnv1a( h, &e->text_xf.y,        sizeof e->text_xf.y );
            h = fnv1a( h, &e->text_xf.len,      sizeof e->text_xf.len );
            h = fnv1a( h, &e->text_xf.scale,    sizeof e->text_xf.scale );
            h = fnv1a( h, &e->text_xf.rot,      sizeof e->text_xf.rot );
            h = fnv1a( h, &e->text_xf.abgr,     sizeof e->text_xf.abgr );
            h = fnv1a( h, &e->text_xf.edge_w,   sizeof e->text_xf.edge_w );
            h = fnv1a( h, &e->text_xf.edge_col, sizeof e->text_xf.edge_col );
            h = fnv1a( h, &e->text_xf.font,     sizeof e->text_xf.font );
            h = fnv1a( h, s_draw.text_pool + e->text_xf.off, e->text_xf.len );
            break;
        }

        case GUI_CMD_TEXT_SHADOW:
        {
            const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
            h = fnv1a( h, &e->text_shadow.x,           sizeof e->text_shadow.x );
            h = fnv1a( h, &e->text_shadow.y,           sizeof e->text_shadow.y );
            h = fnv1a( h, &e->text_shadow.len,         sizeof e->text_shadow.len );
            h = fnv1a( h, &e->text_shadow.clip_x0,     sizeof e->text_shadow.clip_x0 );
            h = fnv1a( h, &e->text_shadow.clip_x1,     sizeof e->text_shadow.clip_x1 );
            h = fnv1a( h, &e->text_shadow.abgr,        sizeof e->text_shadow.abgr );
            h = fnv1a( h, &e->text_shadow.shadow_abgr, sizeof e->text_shadow.shadow_abgr );
            h = fnv1a( h, &e->text_shadow.dx,          sizeof e->text_shadow.dx );
            h = fnv1a( h, &e->text_shadow.dy,          sizeof e->text_shadow.dy );
            h = fnv1a( h, &e->text_shadow.font,        sizeof e->text_shadow.font );
            h = fnv1a( h, s_draw.text_pool + e->text_shadow.off, e->text_shadow.len );
            break;
        }

        case GUI_CMD_POLYLINE:
        {
            const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
            h = fnv1a( h, &e->polyline.pt_count,  sizeof e->polyline.pt_count );
            h = fnv1a( h, &e->polyline.thickness, sizeof e->polyline.thickness );
            h = fnv1a( h, &e->polyline.align,     sizeof e->polyline.align );
            h = fnv1a( h, &e->polyline.closed,    sizeof e->polyline.closed );
            h = fnv1a( h, &e->polyline.abgr,      sizeof e->polyline.abgr );
            h = fnv1a( h, &s_draw.points[ e->polyline.pt_offset ],
                       e->polyline.pt_count * (u32)sizeof( gui_vec2_t ) );   /* content while L1-hot */
            break;
        }

        case GUI_CMD_RECT_LIST:
        {
            const gui_cmd_ext_t* e = draw_cmd_ext_slot( c->offset );
            h = fnv1a_u32( h, e->rect_list.count );
            h = fnv1a( h, &s_draw.rect_pool[ e->rect_list.offset ],
                       e->rect_list.count * (u32)sizeof( gui_rect_col_t ) );   /* content while L1-hot */
            break;
        }

        default:
            /* Every plain POD command has a non-zero entry above; zero means a command type was
               added without registering its payload length, and its bytes would silently never
               fold -- the retained cache would treat every change to it as "unchanged". */
            ORB_ASSERT_MSG( k_cmd_hash_len[ c->type ] != 0,
                            "gui command type missing from k_cmd_hash_len -- its payload "
                            "does not hash and the retained cache cannot see it change" );
            h = fnv1a( h, gui_cmd_payload( c ), k_cmd_hash_len[ c->type ] );
            break;
    }
    return h;
}

/*==============================================================================================

    draw_cmd_open / draw_cmd_seal -- the shared preamble and postamble of every shape push.

    Open runs the four gates every push goes through, in the one order that is correct:

    1. Command list / pool full (emission simply stops, loudly, once).
    2. Frozen by the command stepper debug tool (main-band pushes suppressed).
    3. A fully transparent shape contributes nothing under alpha blending.  `vis_col` arrives
       with the global alpha ALREADY folded; a multi-color shape passes the OR of its folded
       colours, so it survives if any end is visible.
    4. The clip cull -- last, because it is the only gate that costs arithmetic.  `pad` grows
       the cull box on every side for shapes whose geometry reaches past the authored rect
       (the SDF AA skirt, a shadow's feather).

    On pass, open claims the slot, stamps the header, and returns the payload to fill; the
    caller then calls seal, which bakes the retained-cache hash while the bytes are L1-hot.
    Returns NULL when the shape must not spend a slot.

    The four pool-backed pushes (text, text_xf, polyline via gui_emit_path.c, rect_list) 
    keep their own preambles: each has a pool copy that must succeed BEFORE a slot may be 
    spent, and a cull that is not an axis-aligned box test.  
    
    They still owe the same transparent drop this preamble runs -- alpha 0 is the free 
    visibility toggle everywhere, with one text nuance: a visible TEXT_EDGE keeps a 
    transparent-fill run alive (the outline paints outside the glyph).

==============================================================================================*/

static gui_cmd_t*
draw_cmd_claim( u8 type )
{
/*  Claim the next command slot and stamp the ambient (clip_idx, vp) pair onto it. 
    vp is the batch key; clip_idx names the rect the tessellator resolves into the 
    slot's local clip table (the vertex clip band). Stamping both is the one thing 
    every command must do and no command may get wrong.

    Split out of draw_cmd_open below because the pool-backed pushes cannot use that 
    function's preamble (their pool copy has to succeed before a slot is spent, and
    their cull is not an axis-aligned box test) but they owe the identical stamp. */

    gui_cmd_t* c   = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type        = type;
    c->clip_idx    = s_draw.cur_clip_idx;
    c->vp          = (u8)s_draw.cur_vp;
    c->clip_empty  = rect_empty( s_draw.clip_table[ c->clip_idx ] );

    return c;
}

/* Claim `c`'s payload from the pool -- the shared body behind every push whose preamble can't
   go through draw_cmd_open below (a non-axis-aligned cull, or a pool-backed push whose OWN
   content copy -- text_pool / points / rect_pool -- must land first).  Callers already ran
   draw_emit_blocked with this same k_cmd_hash_len[type] size, so the claim here cannot overflow. */
static gui_cmd_ext_t*
draw_cmd_claim_ext( gui_cmd_t* c )
{
    c->offset         = s_draw.pool_used;
    s_draw.pool_used += draw_cmd_align4( k_cmd_hash_len[ c->type ] );
    return draw_cmd_ext_slot( c->offset );
}

/* The shared preamble for every plain shape push (an axis-aligned box cull): claims both the
   envelope slot and the type's pool payload, returning the payload pointer to fill.  The four
   pool-backed pushes (text, text_xf, polyline, rect_list) keep their own preambles instead --
   see draw_cmd_claim_ext. */
static gui_cmd_ext_t*
draw_cmd_open( u8 type, u32 vis_col, f32 x, f32 y, f32 w, f32 h, f32 pad )
{
    if ( draw_emit_blocked( k_cmd_hash_len[ type ] ) )
        return NULL;

    if ( ( vis_col >> 24 ) == 0u )
        return NULL;

    if ( draw_cull_box( x - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad ) )
        return NULL;

    gui_cmd_t* c = draw_cmd_claim( type );
    return draw_cmd_claim_ext( c );
}

static void
draw_cmd_seal( void )
{
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] =
        draw_hash_cmd( &s_draw.cmds[ s_draw.cmd_count - 1 ] );
}

/*============================================================================================*/
// clang-format on