/*==============================================================================================

    runtime_service/gui/backend/gui_build_volatile.c -- Volatile widgets, BUILD-unit half.

    An inline-emit callback replayed in place on frames the UI build is skipped entirely
    (gui_frame_dirty() false) -- see gui.h (gui_volatile_fn) for the full contract and
    widgets/gui_volatile.c for the UI-unit half of this seam.

    Real emit: gui_volatile_cb (widgets/gui_volatile.c) brackets one inline invocation of the
    caller's callback with gui_volatile_cb_open/_close (this file), which record the command
    range it produced and fold a cheap topology hash over it; gui_volatile_stamp (this file,
    called from inside the callback by gui_volatile_begin) records the window/z/vp/font/clip
    context and the layout cursor position at the moment the callback started.  Once BUILD
    tessellates the window, tess_dispatch (gui_build_tess.c) calls volatile_capture with the
    resulting absolute vertex/index span and the vertex base RELATIVE to the window's slot
    (local_vert_base) -- the piece needed to reproduce correctly-relative indices when the span
    is re-tessellated in isolation during replay.

    Replay (gui_update_volatile, called by the host in place of ctx_begin/emit/ctx_end on a clean
    frame): reconstructs the minimal context the callback needs (gui_replay_scope_enter, the one
    reverse call into the UI unit), re-invokes it, and checks the newly appended commands against
    what real emit recorded.  A match is tessellated into scratch space at the current (unused, on
    a clean frame) tail of s_tess and memcpy'd into the original span; a mismatch retires the row
    (it re-registers itself good as new the next time its window real-emits) and asks for one more
    real frame via gui_replay_scope_exit's force_redraw so nothing stays visibly stale.

    s_volatile_reflow_gen is the one staleness guard needed for the *real-emit* path: a row is
    only trusted if it was (re)captured at or after the last non-stable rebuild.  An earlier row
    belongs to a window that vanished or reordered without re-emitting its volatile widget, so it
    is retired instead of being patched at a vertex offset that may now belong to something else.
    It is bumped from cache_build_frame (gui_build_cache.c) whenever a rebuild is not set_stable --
    that one line is the only piece of this feature still living outside these two files.

    Included by gui_backend.c after gui_build_tess.c (needs the s_tess struct + tess_dispatch) and
    before gui_build_cache.c (cache_build_frame bumps s_volatile_reflow_gen, and gui_render_flush
    uploads s_tess unconditionally every frame -- both existing pieces this feature rides for
    free, described in gui_build_cache.c's own header comment).

==============================================================================================*/
// clang-format off

#define GUI_MAX_VOLATILE 16

/* Field order/width chosen to pack this small (GUI_MAX_VOLATILE-sized) table tightly rather than
   however the struct reads best -- fn is the one 8-byte (pointer) member, so it anchors the
   layout; everything below it is u16 or smaller.  u16 is safe for cmd/vert/idx indices because
   GUI_MAX_CMDS/GUI_MAX_VERTS/GUI_MAX_IDX (gui.h) are all comfortably under 65536, and for
   vp/font because GUI_MAX_VIEWPORTS and the font registry are tiny; z is a window paint order
   among RENDER_MAX_WIN (32) windows, likewise nowhere near 65536.  cap_gen truncates the u32
   s_volatile_reflow_gen counter, so it can in principle wrap after 65536 non-stable rebuilds in
   one run; the only consequence is a row looking one generation older/newer than it is, which at
   worst costs the same one-frame-stale fallback as a topology mismatch -- never corruption. */
typedef struct
{
    gui_id_t         id, win;
    f32              x, y, w;          // layout cursor stamp at gui_volatile_begin
    u32              topo_hash;        // folded {type, clip, vp, (rect)tex/(text)len} over that range
    gui_volatile_fn  fn;
    u16              cmd_lo, cmd_hi;   // command range this callback produced at real emit
    u16              vert_base,       vert_count;
    u16              idx_base,        idx_count;
    u16              local_vert_base;  // vert_base - (window slot's vert_base at capture time)
    u16              z, vp, font;
    u16              cap_gen;          // s_volatile_reflow_gen as of the last real capture
    u8               clip_idx;
    bool             active;           // false once retired by staleness/mismatch, or never captured

} gui_volatile_slot_t;

static gui_volatile_slot_t s_volatile[ GUI_MAX_VOLATILE ];
static u32                 s_volatile_count;
static u32                 s_volatile_reflow_gen;

/* The row currently mid-callback during real emit (between gui_volatile_cb_open and _close).
   Only one gui_volatile_cb invocation is ever in flight at a time -- nesting is not supported. */
static gui_id_t s_open_id    = GUI_ID_NONE;
static u32      s_open_cmd_lo;

static gui_volatile_slot_t*
volatile_find_or_add( gui_id_t id )
{
    for ( u32 i = 0; i < s_volatile_count; ++i )
        if ( s_volatile[ i ].id == id )
            return &s_volatile[ i ];
    if ( s_volatile_count >= GUI_MAX_VOLATILE )
        return NULL;
    gui_volatile_slot_t* row = &s_volatile[ s_volatile_count++ ];
    *row = ( gui_volatile_slot_t ){ .id = id };
    return row;
}

/* Coarse per-command topology fingerprint over [lo, hi) -- type, clip, viewport, plus the one
   field per command type that would change vertex/index COUNT if it changed (a rect's texture
   slot affects its UV path only, not count, so it is not folded; a text run's length directly
   changes glyph-quad count, so it is).  Not exhaustive -- see the open risk in the plan doc --
   but cheap, and a false pass only ever costs one stale-looking frame, never corrupt geometry
   (the vertex/index COUNT check in gui_update_volatile is what actually gates the memcpy). */
static u32
volatile_topo_fold( u32 lo, u32 hi )
{
    u32 h = 2166136261u;
    for ( u32 i = lo; i < hi; ++i )
    {
        const gui_cmd_t* c = &s_draw.cmds[ i ];
        h = fnv1a_u32( h, (u32)c->type );
        h = fnv1a_u32( h, c->clip_idx );
        h = fnv1a_u32( h, c->vp );
        if ( c->type == GUI_CMD_TEXT )
            h = fnv1a_u32( h, c->text.len );
    }
    return h;
}

/* Called by gui_volatile_cb (widgets/gui_volatile.c) right before it invokes the callback
   inline during real emit -- opens the command-range bracket for `id`. */
void
gui_volatile_cb_open( gui_id_t id )
{
    s_open_id     = id;
    s_open_cmd_lo = s_draw.cmd_count;
}

/* Called by gui_volatile_begin (widgets/gui_volatile.c), from inside the callback body during
   real emit -- stamps the emit context (window/z/vp/font/clip) and the layout cursor position
   (x, y, w) the callback started at, so replay can reconstruct a matching scope later. */
void
gui_volatile_stamp( f32 x, f32 y, f32 w )
{
    if ( s_open_id == GUI_ID_NONE ) return;
    gui_volatile_slot_t* row = volatile_find_or_add( s_open_id );
    if ( !row ) return;
    row->win      = s_draw.cur_win;
    row->z        = (u16)s_draw.cur_z;
    row->vp       = (u16)s_draw.cur_vp;
    row->font     = (u16)s_draw.cur_font;
    row->clip_idx = s_draw.cur_clip_idx;
    row->x = x; row->y = y; row->w = w;
}

/* Called by gui_volatile_cb right after the callback returns during real emit -- closes the
   command-range bracket, tags every command in it with `id` (cmd_volatile_id is a range tag, not
   a single-command tag), folds the topology hash, and stores the callback pointer. */
void
gui_volatile_cb_close( gui_volatile_fn fn )
{
    if ( s_open_id == GUI_ID_NONE ) return;
    gui_volatile_slot_t* row = volatile_find_or_add( s_open_id );
    if ( row )
    {
        u32 lo = s_open_cmd_lo, hi = s_draw.cmd_count;
        for ( u32 i = lo; i < hi; ++i )
            s_draw.cmd_volatile_id[ i ] = s_open_id;
        row->cmd_lo    = (u16)lo;
        row->cmd_hi    = (u16)hi;
        row->topo_hash = volatile_topo_fold( lo, hi );
        row->fn        = fn;
    }
    s_open_id = GUI_ID_NONE;
}

/* Called from tess_dispatch (gui_build_tess.c) once a volatile-tagged command RANGE's vertices
   and indices are fully written.  local_vert_base lets replay re-tessellate the same range into a
   scratch location and still produce indices relative to the ORIGINAL window slot (see
   gui_update_volatile).  Stamps cap_gen so a row can be told apart from one left over from before
   the last disruptive rebuild. */
static void
volatile_capture( gui_id_t id, gui_id_t win, u32 vert_base, u32 vert_count,
                  u32 idx_base, u32 idx_count )
{
    gui_volatile_slot_t* row = volatile_find_or_add( id );
    if ( !row ) return;
    (void)win;   /* row->win is already set by gui_volatile_stamp; not otherwise needed here */
    row->vert_base       = (u16)vert_base;
    row->vert_count      = (u16)vert_count;
    row->idx_base        = (u16)idx_base;
    row->idx_count       = (u16)idx_count;
    row->local_vert_base = (u16)( vert_base - s_tess.slot_vert_base );
    row->cap_gen         = (u16)s_volatile_reflow_gen;
    row->active          = true;
}

/* Host entry point for a clean frame (gui_frame_dirty() == false): replay every still-valid
   volatile row's callback standalone and patch its geometry span in place if the replay
   reproduces the exact topology real emit recorded.  A row whose last capture predates the most
   recent non-stable rebuild is retired instead -- it was not re-emitted during the rebuild that
   could have moved its geometry, so its vert_base is no longer trustworthy. */
void
gui_update_volatile( void )
{
    for ( u32 i = 0; i < s_volatile_count; ++i )
    {
        gui_volatile_slot_t* row = &s_volatile[ i ];
        if ( !row->active )
            continue;
        if ( row->cap_gen < s_volatile_reflow_gen )
        {
            row->active = false;
            continue;
        }

        /* Checkpoint s_draw's transient emit state -- everything the callback appends this call
           is throwaway; nothing about it should outlive this function. */
        u32 cmd_ck  = s_draw.cmd_count;
        u32 seg_ck  = s_draw.seg_count;
        u32 pt_ck   = s_draw.pt_count;
        u32 text_ck = s_draw.text_pool_used;
        gui_cmd_seg_t seg_live = s_draw.segs[ seg_ck - 1 ];

        gui_id_t   win_ck        = s_draw.cur_win;
        u32        z_ck          = s_draw.cur_z;
        u32        vp_ck         = s_draw.cur_vp;
        u32        font_ck       = s_draw.cur_font;
        u8         clip_ck       = s_draw.cur_clip_idx;
        u32        clip_depth_ck = s_draw.clip_depth;
        gui_rect_t clip_top_ck   = s_draw.clip_stack[ 0 ];
        u8         clip_idx0_ck  = s_draw.clip_idx_stack[ 0 ];

        s_draw.cur_win      = row->win;
        s_draw.cur_z        = row->z;
        s_draw.cur_vp       = row->vp;
        s_draw.cur_font     = row->font;
        s_draw.cur_clip_idx = row->clip_idx;
        font_use( row->font );

        /* draw_cull_box (gui_emit_draw.c) tests against clip_stack[clip_depth-1], NOT
           cur_clip_idx -- cur_clip_idx alone is not enough to reproduce the real-emit clip. Force
           a one-deep stack whose top is the captured clip's resolved rect, or a callback whose
           rect happens to sit outside whatever clip was left on the stack by the last real emit
           gets silently culled during replay (0 commands appended instead of the expected count),
           which mismatches and falls back to a real frame -- intermittently, depending on
           whatever else emitted last.  This was the cause of the flaky "wins retained" count. */
        s_draw.clip_stack    [ 0 ] = s_draw.clip_table[ row->clip_idx ];
        s_draw.clip_idx_stack[ 0 ] = row->clip_idx;
        s_draw.clip_depth          = 1;

        gui_replay_scope_enter( row->id, row->x, row->y, row->w );
        row->fn( true );
        u32 cmd_hi = s_draw.cmd_count;

        bool ok = ( cmd_hi - cmd_ck == (u32)( row->cmd_hi - row->cmd_lo ) )
                  && ( volatile_topo_fold( cmd_ck, cmd_hi ) == row->topo_hash );

        if ( ok )
        {
            /* cmd_volatile_id is not zeroed per-frame -- clear any leftover tag on the freshly
               appended range so the scratch tessellation below (which reuses tess_dispatch,
               volatile-capture logic and all) does not misread a stale id left over from
               whatever real commands last occupied these slots. */
            for ( u32 k = cmd_ck; k < cmd_hi; ++k )
                s_draw.cmd_volatile_id[ k ] = GUI_ID_NONE;

            static u32 scratch_order[ GUI_MAX_CMDS ];
            static u32 scratch_font [ GUI_MAX_CMDS ];
            u32 n = cmd_hi - cmd_ck;
            for ( u32 k = 0; k < n; ++k )
            {
                scratch_order[ k ] = cmd_ck + k;
                scratch_font [ k ] = row->font;
            }

            u32  vert_ck      = s_tess.vert_count;
            u32  idx_ck       = s_tess.idx_count;
            u32  tcmd_ck      = s_tess.cmd_count;
            u32  slot_vb_ck   = s_tess.slot_vert_base;
            bool force_new_ck = s_tess.force_new_cmd;

            /* Tessellate into the current (real-frame-unused) tail of s_tess.  slot_vert_base is
               set so the indices this produces come out relative to the ORIGINAL window slot,
               matching what is already at row->vert_base -- see local_vert_base above. */
            s_tess.slot_vert_base = s_tess.vert_count - row->local_vert_base;
            s_tess.force_new_cmd  = true;

            tess_dispatch( s_draw.cmds, scratch_order, scratch_font, n, row->win );

            u32 new_vert_count = s_tess.vert_count - vert_ck;
            u32 new_idx_count  = s_tess.idx_count  - idx_ck;

            if ( new_vert_count == (u32)row->vert_count && new_idx_count == (u32)row->idx_count )
            {
                memcpy( &s_tess.verts  [ row->vert_base ], &s_tess.verts  [ vert_ck ],
                        new_vert_count * sizeof( gui_draw_vert_t ) );
                memcpy( &s_tess.indices[ row->idx_base  ], &s_tess.indices[ idx_ck  ],
                        new_idx_count  * sizeof( u16 ) );
            }
            else
            {
                ok = false;
            }

            /* Roll s_tess back -- as far as this frame's real geometry/dispatch table is
               concerned, the scratch tessellation never happened. */
            s_tess.vert_count     = vert_ck;
            s_tess.idx_count      = idx_ck;
            s_tess.cmd_count      = tcmd_ck;
            s_tess.slot_vert_base = slot_vb_ck;
            s_tess.force_new_cmd  = force_new_ck;
        }

        gui_replay_scope_exit( !ok );

        /* Roll s_draw back -- nothing the callback emitted this call belongs in the real frame's
           persistent state, match or not. */
        s_draw.cmd_count          = cmd_ck;
        s_draw.seg_count          = seg_ck;
        s_draw.pt_count           = pt_ck;
        s_draw.text_pool_used     = text_ck;
        s_draw.segs[ seg_ck - 1 ] = seg_live;

        s_draw.cur_win             = win_ck;
        s_draw.cur_z                = z_ck;
        s_draw.cur_vp               = vp_ck;
        s_draw.cur_font             = font_ck;
        s_draw.cur_clip_idx         = clip_ck;
        s_draw.clip_depth           = clip_depth_ck;
        s_draw.clip_stack    [ 0 ]  = clip_top_ck;
        s_draw.clip_idx_stack[ 0 ]  = clip_idx0_ck;
        font_use( font_ck );

        if ( !ok )
            row->active = false;   /* retire; the next real re-emit of this widget recreates it */
    }
}

// clang-format on
/*============================================================================================*/
