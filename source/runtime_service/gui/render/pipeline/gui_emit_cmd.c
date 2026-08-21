/*==============================================================================================
    gui/render/pipeline/gui_emit_cmd.c -- The command record: claim, stamp, hash, seal.

    The machinery every draw_push_* shares.  draw_cmd_claim spends a slot and stamps the ambient
    (clip_idx, vp) pair; draw_cmd_open runs the gates that decide whether a slot may be spent at
    all; draw_cmd_seal bakes the retained-cache hash while the payload is still L1-hot.

    Between gui_emit_state.c (owns s_draw and the fnv1a helpers this folds through) and the shape
    files that call it.

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

/* Payload byte count per command type, for the plain POD commands: one fnv1a fold of the union
   member, nothing else.  Only the four pool-backed commands (TEXT, TEXT_XF, POLYLINE, RECT_LIST)
   need code of their own -- they hash pool CONTENT and must skip their pool-offset fields, which
   shift whenever an earlier-emitted window changes its pool volume and would falsely dirty an
   unrelated window.  Every union member starts at the same address, so the fold reads &c->rect
   as the generic payload pointer. */

static const u8 k_cmd_hash_len[] = {
    [GUI_CMD_RECT_FILLED]   = sizeof(( (gui_cmd_t*)0 )->rect ),
    [GUI_CMD_RECT_OUTLINE]  = sizeof(( (gui_cmd_t*)0 )->rect_outline ),
    [GUI_CMD_FRAME]         = sizeof(( (gui_cmd_t*)0 )->frame ),
    [GUI_CMD_TRIANGLE]      = sizeof(( (gui_cmd_t*)0 )->tri ),
    [GUI_CMD_BEZIER]        = sizeof(( (gui_cmd_t*)0 )->bezier ),
    [GUI_CMD_LINE]          = sizeof(( (gui_cmd_t*)0 )->line ),
    [GUI_CMD_DASHED_LINE]   = sizeof(( (gui_cmd_t*)0 )->dash ),
    [GUI_CMD_RECT_GRADIENT] = sizeof(( (gui_cmd_t*)0 )->gradient ),
    [GUI_CMD_SPRITE]        = sizeof(( (gui_cmd_t*)0 )->sprite ),

    /* Folds rate whole, so a pulse hashes stable frame-to-frame (it animates in the FRAGMENT off
       pc.time -- the geometry never changes, which is the entire point of the mode). */
    [GUI_CMD_FX_BOX]        = sizeof(( (gui_cmd_t*)0 )->fx_box ),
    [GUI_CMD_ROUND_RECT_EX] = sizeof(( (gui_cmd_t*)0 )->round_rect ),

    /* Both sectors fold the same member.  A spinner's start angle moves every frame, so this
       dirties every frame -- honestly, since the geometry really does rotate.  (A spinner that
       wanted free animation would be a shader-clock mode like PULSE, not a re-emit.) */

    [GUI_CMD_ARC]           = sizeof(( (gui_cmd_t*)0 )->arc ),
    [GUI_CMD_PIE]           = sizeof(( (gui_cmd_t*)0 )->arc ),
    [GUI_CMD_ARC_DASH]      = sizeof(( (gui_cmd_t*)0 )->arc_dash ),
    [GUI_CMD_ARC_GRAD]      = sizeof(( (gui_cmd_t*)0 )->arc_grad ),
    [GUI_CMD_IMAGE_XF]      = sizeof(( (gui_cmd_t*)0 )->image_xf ),
    [GUI_CMD_CHECKER]       = sizeof(( (gui_cmd_t*)0 )->checker ),
    [GUI_CMD_GRID]          = sizeof(( (gui_cmd_t*)0 )->grid ),
    [GUI_CMD_NGON]          = sizeof(( (gui_cmd_t*)0 )->ngon ),

    /* Folds rate/phase whole like FX_BOX: the ants scroll in the fragment off pc.time, so the
       command hashes stable frame-to-frame while the pattern moves. */

    [GUI_CMD_BOX_DASH]      = sizeof(( (gui_cmd_t*)0 )->box_dash ),
    [GUI_CMD_REPEAT]        = sizeof(( (gui_cmd_t*)0 )->repeat ),

    /* Folds rate/phase whole like FX_BOX: the ring spins in the fragment off pc.time, so the
       command hashes stable frame-to-frame while it turns. */

    [GUI_CMD_REPEAT_POLAR]  = sizeof(( (gui_cmd_t*)0 )->repeat_polar ),
    [GUI_CMD_BOX_CUT]       = sizeof(( (gui_cmd_t*)0 )->box_cut ),
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
            h = fnv1a( h, &c->text.x,           sizeof c->text.x );
            h = fnv1a( h, &c->text.y,           sizeof c->text.y );
            h = fnv1a( h, &c->text.len,         sizeof c->text.len );
            h = fnv1a( h, &c->text.clip_x0,     sizeof c->text.clip_x0 );
            h = fnv1a( h, &c->text.clip_x1,     sizeof c->text.clip_x1 );
            h = fnv1a( h, &c->text.abgr,        sizeof c->text.abgr );
            h = fnv1a( h, &c->text.edge_w,      sizeof c->text.edge_w );
            h = fnv1a( h, &c->text.edge_col,    sizeof c->text.edge_col );
            h = fnv1a( h, &c->text.font,        sizeof c->text.font );
            h = fnv1a( h, s_draw.text_pool + c->text.off, c->text.len );   /* content while L1-hot */
            break;

        /* Folds scale and rot, so a run that spins re-tessellates every frame it moves.  That is
           the honest cost and the difference from PULSE: a pulse animates in the FRAGMENT off
           pc.time and its geometry never changes, while a transform is baked into vertices. */

        case GUI_CMD_TEXT_XF:
            h = fnv1a( h, &c->text_xf.x,        sizeof c->text_xf.x );
            h = fnv1a( h, &c->text_xf.y,        sizeof c->text_xf.y );
            h = fnv1a( h, &c->text_xf.len,      sizeof c->text_xf.len );
            h = fnv1a( h, &c->text_xf.scale,    sizeof c->text_xf.scale );
            h = fnv1a( h, &c->text_xf.rot,      sizeof c->text_xf.rot );
            h = fnv1a( h, &c->text_xf.abgr,     sizeof c->text_xf.abgr );
            h = fnv1a( h, &c->text_xf.edge_w,   sizeof c->text_xf.edge_w );
            h = fnv1a( h, &c->text_xf.edge_col, sizeof c->text_xf.edge_col );
            h = fnv1a( h, &c->text_xf.font,     sizeof c->text_xf.font );
            h = fnv1a( h, s_draw.text_pool + c->text_xf.off, c->text_xf.len );
            break;

        case GUI_CMD_TEXT_SHADOW:
            h = fnv1a( h, &c->text_shadow.x,           sizeof c->text_shadow.x );
            h = fnv1a( h, &c->text_shadow.y,           sizeof c->text_shadow.y );
            h = fnv1a( h, &c->text_shadow.len,         sizeof c->text_shadow.len );
            h = fnv1a( h, &c->text_shadow.clip_x0,     sizeof c->text_shadow.clip_x0 );
            h = fnv1a( h, &c->text_shadow.clip_x1,     sizeof c->text_shadow.clip_x1 );
            h = fnv1a( h, &c->text_shadow.abgr,        sizeof c->text_shadow.abgr );
            h = fnv1a( h, &c->text_shadow.shadow_abgr, sizeof c->text_shadow.shadow_abgr );
            h = fnv1a( h, &c->text_shadow.dx,          sizeof c->text_shadow.dx );
            h = fnv1a( h, &c->text_shadow.dy,          sizeof c->text_shadow.dy );
            h = fnv1a( h, &c->text_shadow.font,        sizeof c->text_shadow.font );
            h = fnv1a( h, s_draw.text_pool + c->text_shadow.off, c->text_shadow.len );
            break;

        case GUI_CMD_POLYLINE:
            h = fnv1a( h, &c->polyline.pt_count,  sizeof c->polyline.pt_count );
            h = fnv1a( h, &c->polyline.thickness, sizeof c->polyline.thickness );
            h = fnv1a( h, &c->polyline.align,     sizeof c->polyline.align );
            h = fnv1a( h, &c->polyline.closed,    sizeof c->polyline.closed );
            h = fnv1a( h, &c->polyline.abgr,      sizeof c->polyline.abgr );
            h = fnv1a( h, &s_draw.points[ c->polyline.pt_offset ],
                       c->polyline.pt_count * (u32)sizeof( gui_vec2_t ) );   /* content while L1-hot */
            break;

        case GUI_CMD_RECT_LIST:
            h = fnv1a_u32( h, c->rect_list.count );
            h = fnv1a( h, &s_draw.rect_pool[ c->rect_list.offset ],
                       c->rect_list.count * (u32)sizeof( gui_rect_col_t ) );   /* content while L1-hot */
            break;

        default:
            /* Every plain POD command has a non-zero entry above; zero means a command type was
               added without registering its payload length, and its bytes would silently never
               fold -- the retained cache would treat every change to it as "unchanged". */
            ORB_ASSERT_MSG( k_cmd_hash_len[ c->type ] != 0,
                            "gui command type missing from k_cmd_hash_len -- its payload "
                            "does not hash and the retained cache cannot see it change" );
            h = fnv1a( h, &c->rect, k_cmd_hash_len[ c->type ] );
            break;
    }
    return h;
}

/*==============================================================================================

    draw_cmd_open / draw_cmd_seal -- the shared preamble and postamble of every shape push.

    Open runs the four gates every push goes through, in the one order that is correct.

    1. Exceeds command limit (just stops drawing commands)
    2. Frozen by the command stepper debug tool (skips non-debug band)
    3. A fully transparent shape contributes nothing under alpha blending.
    4. Then a cull test, which is the only gate that can be expensive comes lat.
    
    The slot and stamps the header. 
    
    ALREADY folded (a multi-color shape pass the OR of its folded colours -- visible
    if any end is). `pad` grows the cull box on every side for shapes whose geometry 
    reaches past the authored rect (the SDF AA skirt, a shadow's feather).  
    
    Returns NULL when the shape must not spend a slot; otherwise the caller fills the 
    payload and calls seal, which bakes the retained-cache hash while the bytes are L1-hot.

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

    gui_cmd_t* c = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type      = type;
    c->clip_idx  = s_draw.cur_clip_idx;
    c->vp        = (u8)s_draw.cur_vp;

    return c;
}

static gui_cmd_t*
draw_cmd_open( u8 type, u32 vis_col, f32 x, f32 y, f32 w, f32 h, f32 pad )
{
    if ( draw_emit_blocked() )
        return NULL;

    if ( ( vis_col >> 24 ) == 0u )
        return NULL;

    if ( draw_cull_box( x - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad ) )
        return NULL;

    return draw_cmd_claim( type );
}

static void
draw_cmd_seal( void )
{
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] =
        draw_hash_cmd( &s_draw.cmds[ s_draw.cmd_count - 1 ] );
}

/*============================================================================================*/
// clang-format on