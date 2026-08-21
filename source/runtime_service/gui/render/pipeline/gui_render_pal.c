/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_render_pal.c -- the style palette.

    The style arena is carved per window slot, so a style index a quad bakes is meaningful only
    against the slot that emitted it (gui_build_tess_state.c, slot_prim_base).  That is what makes the
    dedup memo slot-scoped: fifty windows drawing the same panel background hold fifty copies of
    one record, and no walk can reach past the boundary to find the others.

    The palette is the other half of the address space.  It holds records that belong to no window
    -- the theme's own vocabulary, the shapes every widget draws -- once, in a block past every
    region, named by a style index in the range the arena can never produce (gui.h, GUI_PAL_FIRST).
    A quad naming one resolves it against pc.pal_base instead of pc.prim_base, and gets a record
    that cost the arena nothing.

    This file owns the CPU-side table and its upload.  What goes IN it is decided in
    gui_render_intern.c; this unit takes a set of records and makes them addressable.

    UPLOAD.  One block per frame-in-flight (GUI_PAL_REGION_COUNT), because publish can land at any
    time -- a theme switch, a DPI change -- while earlier frames are still reading the old table.
    Each block tracks the generation it holds and re-uploads at the top of the first flush of a
    frame that finds itself stale, so a publish costs two writes total and a steady frame costs
    none.

==============================================================================================*/
// clang-format off

static struct
{
    gui_prim_t rec  [ GUI_PAL_MAX ];              // the published table, ready for the GPU
    u32        count;                             // entries published (0 = palette unused)
    u32        gen;                               // bumped by publish; what the blocks chase
    u32        block_gen[ GUI_PAL_REGION_COUNT ]; // generation each frame's block holds

} s_pal;

/*==============================================================================================
    render_pal_publish -- install a table, replacing whatever was there.

    Replaces rather than patches: every record in a table was learned against one style grid, so a
    partial update would leave records from two different themes addressable at once.  Entries past
    `count` keep whatever they held -- nothing names them, since an index is only ever handed out
    for an entry of the live table.

    Safe to call every frame; identical content still costs the two uploads, so the caller invokes
    it when the style epoch moves, not on a timer.
==============================================================================================*/

static void
render_pal_publish( const gui_prim_t* recs, u32 count )
{
    if ( count > GUI_PAL_MAX )
    {
        GUI_WARN_ONCE( "style palette overflow: %u entries published, cap %u -- the tail is "
                       "dropped and its styles fall back to per-slot records.\n",
                       count, (u32)GUI_PAL_MAX );
        count = GUI_PAL_MAX;
    }

    if ( count )
        memcpy( s_pal.rec, recs, count * sizeof( gui_prim_t ) );
    s_pal.count = count;

    /* A fresh generation every publish.  The blocks compare rather than clear a flag, so a publish
       that lands between two surfaces of the same frame still reaches both frames' blocks. */
    ++s_pal.gen;
}

/*==============================================================================================
    render_pal_extend -- grow the table without disturbing what is already in it.

    The publish above replaces, and a replace invalidates every palette index in cached
    geometry. This one only APPENDS: entries below the current count keep their bytes and
    their meaning, so a cached quad naming entry 12 still means entry 12 and no re-place is
    owed.  That is the whole reason interning can run during tessellation while an epoch
    reset cannot (gui_render_intern.c).

    Still a generation bump, because the blocks have to carry the new tail before a draw
    names it.  The bytes it writes sit past what any in-flight frame was uploaded with, so an
    earlier frame reading its own block cannot be reading them.
==============================================================================================*/

static void
render_pal_extend( const gui_prim_t* recs, u32 count )
{
    if ( count > GUI_PAL_MAX )
        count = GUI_PAL_MAX;
    if ( count <= s_pal.count )
        return;

    memcpy( &s_pal.rec[ s_pal.count ], &recs[ s_pal.count ],
            ( count - s_pal.count ) * sizeof( gui_prim_t ) );
    s_pal.count = count;
    ++s_pal.gen;
}

/*  Where this frame's block starts, in RECORDS -- what goes out as pc.pal_base.  Flush-constant:
    unlike prim_base it does not move as the dispatch walk crosses window slots, because the whole
    point of a palette entry is that every slot resolves it the same way. */

static u32
render_pal_base( u32 frame )
{
    return (u32)GUI_PAL_ORIGIN + frame * (u32)GUI_PAL_MAX;
}

/*  Bring this frame's block up to date.  Called at the top of every flush; uploads only when the
    block is behind the published generation, which after a steady publish is the first flush of
    each in-flight frame and nothing after. */

static void
render_pal_upload( u32 frame )
{
    if ( frame >= (u32)GUI_PAL_REGION_COUNT || s_pal.block_gen[ frame ] == s_pal.gen )
        return;
    s_pal.block_gen[ frame ] = s_pal.gen;

    if ( s_pal.count == 0 )
        return;   // nothing published: no quad can name an entry, so there is nothing to write

    rhi()->buffer_write( s_render.prim_buf, s_pal.rec, s_pal.count * (u32)GUI_PRIM_BYTES,
                         render_pal_base( frame ) * (u32)GUI_PRIM_BYTES );
}

// clang-format on
/*============================================================================================*/
