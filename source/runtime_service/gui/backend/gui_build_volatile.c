/*==============================================================================================

    runtime_service/gui/backend/gui_build_volatile.c -- Volatile widgets, BUILD-unit half.

    An inline-emit callback replayed in place on frames the UI build is skipped entirely
    (gui_frame_dirty() false) -- see gui.h (gui_volatile_fn) for the full contract and
    widgets/gui_volatile.c for the UI-unit half of this seam.

    Design: every volatile block owns a RESERVED, PADDED sub-region of its window's geometry
    slot -- a vertex span, an index span, and a run of GPU commands, each allocated with headroom
    past what the live emit actually produced -- and every update is a plain re-tessellation into
    that reservation.  There is no requirement that a replay reproduce the exact topology of the
    original emit: text may grow or shrink, rounding categories may flip, glyph counts may change.
    The only way an update can fail is by outgrowing the reservation, and that failure is
    self-healing: the row records what it actually needed, the owning window is invalidated
    (cache_invalidate_window) so the next real frame re-tessellates it, and the recapture reserves
    the larger size.  Reservations are grow-only per id.

    Position integrity: a row never stores an absolute buffer address.  It stores its position
    RELATIVE to the owning window's slot (local_vert_base / local_idx_base / local_cmd_base) plus
    the slot's tessellation generation at capture time (tess_gen, bumped by cache_build_frame on
    every retess of the window).  At patch time the window's CURRENT slot is resolved by id from
    the live slot table (cache_slot_lookup) and the absolute address computed fresh; the patch
    proceeds only if the slot's generation still matches the capture.  A window whose slot moved
    (sibling reflow) resolves correctly automatically; a window re-tessellated without the widget
    re-emitting (content branch hid it) fails the generation check and the patch is skipped --
    a stale address physically cannot be produced.

    Real emit: gui_volatile_cb (widgets/gui_volatile.c) brackets one inline invocation of the
    caller's callback with gui_volatile_cb_open/_close (this file), which record the command range
    it produced and tag it with the row id; gui_volatile_stamp (called from inside the callback by
    gui_volatile_begin) records the window/z/vp/font/clip context, the ambient
    alpha/rounding/text-clip scalars a raw draw_ call reads directly, and the layout cursor
    position.  When the window tessellates, tess_dispatch (gui_build_tess.c) calls
    volatile_range_close (this file), which reserves the padded region, pads the slot's GPU
    command run with dormant commands, and stamps the slot generation.

    Retained-cache interaction: a volatile-tagged command NEVER participates in its window's
    retained hash (cache_diff_windows excludes it unconditionally) -- the block is presentation-
    only by contract and updated out of band, so its ever-drifting bytes must not force the window
    to re-tessellate, and the hash behaves identically whether the retained skip is on or off.
    The one thing the diff does check is volatile_row_needs_capture: a tagged command whose row
    has no live capture for the window's current slot generation forces the window CHANGED this
    frame so tessellation runs and captures it (first appearance, post-retirement, or re-shown
    after being branch-hidden).

    Updates run on two paths, both through volatile_patch (re-tessellate into scratch at the tail
    of s_tess, capacity-check against the reservation, copy in, rewrite the block's GPU commands):

        gui_update_volatile          -- idle frames: the host calls it in place of
                                        ctx_begin/emit/ctx_end; each row's callback is re-invoked
                                        standalone inside gui_replay_scope_enter/_exit and patched.
        volatile_patch_reused_window -- real frames where the window's slot is reused: patches from
                                        the commands this frame's live emit already produced.

    Either way, cache_count_volatile_patch (gui_build_cache.c) tallies the patch into
    gui_render_stats_t.volatile_patched -- reported separately from win_retained precisely so a
    window with an animating volatile widget still correctly counts as retained.

    Included by gui_backend.c after gui_build_tess.c (needs s_tess, tess_dispatch, and
    s_volatile_patching, defined there) and before gui_build_cache.c (which defines the
    cache_* helpers forward-declared below and calls volatile_row_needs_capture /
    volatile_patch_reused_window; gui_render_flush uploads the patched spans for free since a
    slot's upload range covers its reservations).

==============================================================================================*/
// clang-format off

#define GUI_MAX_VOLATILE  16
#define VOL_VERT_PAD      128u   /* vertex headroom reserved past a block's live geometry        */
#define VOL_IDX_PAD       192u   /* index headroom (~1.5x vertices for quad-heavy content)       */
#define VOL_CMD_PAD       2u     /* dormant GPU-command slots reserved past the block's live run */

/* Field widths: GUI_MAX_VERTS (16K), GUI_MAX_IDX (48K) and GUI_MAX_CMDS (1024) all fit u16, and
   the local_* offsets are bounded by them.  tess_gen is full u32 -- it must never alias across a
   wrap, since it is the sole guard that a patch writes into geometry produced by the exact
   tessellation pass that captured it. */
typedef struct
{
    gui_id_t         id, win;
    f32              x, y, w;          // layout cursor stamp at gui_volatile_begin
    gui_volatile_fn  fn;
    u32              tess_gen;         // owning slot's tessellation generation at capture
    u16              cmd_lo, cmd_hi;   // live s_draw command range from this frame's real emit
    u16              local_vert_base,  vert_count, vert_alloc;   // relative to slot vert_base
    u16              local_idx_base,   idx_count,  idx_alloc;    // relative to slot idx_base
    u16              local_cmd_base,   cmd_count,  cmd_alloc;    // relative to slot cmd_base
    u16              z, vp, font;
    u8               clip_idx;
    bool             active;           // a capture exists (retired on patch failure until recaptured)
    bool             hidden;           // whole range was clip-empty at emit -- nothing on screen

    /* Ambient s_draw scalars in effect at the moment gui_volatile_begin stamped this row --
       alpha, rounding, and the text-clip window are read directly off s_draw by the raw draw_
       calls a callback makes, the same way cur_win/cur_z/cur_vp/cur_font are.  Stamped here and
       reinstalled by gui_update_volatile for the duration of the standalone replay call so the
       callback sees the same ambient values it drew with at real emit, whatever the idle frame's
       leftover s_draw state happens to be. */
    f32              alpha, rounding, text_clip_x0, text_clip_x1;

} gui_volatile_slot_t;

static gui_volatile_slot_t s_volatile[ GUI_MAX_VOLATILE ];
static u32                 s_volatile_count;

/* Defined later in gui_build_cache.c (same TU, included right after this file) where s_stats,
   s_slots and s_cache live -- forward-declared here the same way gui_build_tess.c forward-declares
   volatile_range_close.
     cache_count_volatile_patch -- stats: rows patched in place this frame.
     cache_slot_lookup          -- resolve a window's CURRENT slot position + tessellation
                                   generation by id; false if the window has no live slot.
     cache_invalidate_window    -- corrupt the window's stored hash + raise any_changed so the
                                   next frame re-tessellates it (a failed patch's recovery path).
     cache_slots_extent         -- far edge of every slot's reservation; the debug guard below
                                   asserts scratch is written past it. */
static void cache_count_volatile_patch( u32 n );
static bool cache_slot_lookup( gui_id_t win, u32* vert_base, u32* idx_base, u32* cmd_base,
                               u32* tess_gen );
static void cache_invalidate_window( gui_id_t win );
static void cache_slots_extent( u32* out_vert_end, u32* out_idx_end );

/* The row currently mid-callback during real emit (between gui_volatile_cb_open and _close).
   Only one gui_volatile_cb invocation is ever in flight at a time -- nesting is not supported. */
static gui_id_t s_open_id    = GUI_ID_NONE;
static u32      s_open_cmd_lo;

static gui_volatile_slot_t*
volatile_find( gui_id_t id )
{
    for ( u32 i = 0; i < s_volatile_count; ++i )
        if ( s_volatile[ i ].id == id )
            return &s_volatile[ i ];
    return NULL;
}

static gui_volatile_slot_t*
volatile_find_or_add( gui_id_t id )
{
    gui_volatile_slot_t* row = volatile_find( id );
    if ( row )
        return row;
    if ( s_volatile_count >= GUI_MAX_VOLATILE )
        return NULL;
    row = &s_volatile[ s_volatile_count++ ];
    *row = ( gui_volatile_slot_t ){ .id = id };
    return row;
}

/* Called by gui_volatile_cb (widgets/gui_volatile.c) right before it invokes the callback inline
   during real emit -- opens the command-range bracket for `id`.  A full registry degrades
   gracefully: the bracket never opens, the commands stay untagged, and the widget behaves as a
   plain (hash-participating) widget that animates through ordinary dirty frames. */
void
gui_volatile_cb_open( gui_id_t id )
{
    if ( !volatile_find_or_add( id ) )
    {
        s_open_id = GUI_ID_NONE;
        return;
    }
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
    gui_volatile_slot_t* row = volatile_find( s_open_id );
    if ( !row ) return;
    row->win      = s_draw.cur_win;
    row->z        = (u16)s_draw.cur_z;
    row->vp       = (u16)s_draw.cur_vp;
    row->font     = (u16)s_draw.cur_font;
    row->clip_idx = s_draw.cur_clip_idx;
    row->x = x; row->y = y; row->w = w;
    row->alpha        = s_draw.alpha;
    row->rounding     = s_draw.rounding;
    row->text_clip_x0 = s_draw.text_clip_x0;
    row->text_clip_x1 = s_draw.text_clip_x1;
}

/* Called by gui_volatile_cb right after the callback returns during real emit -- closes the
   command-range bracket, tags every command in it with `id` (cmd_volatile_id is a range tag, not
   a single-command tag), and stores the callback pointer.  Also computes `hidden`: a range whose
   every command sits in an empty clip (scrolled out of a container) produces no geometry when the
   window tessellates, so there is nothing to capture or patch until it becomes visible again --
   which takes a scroll, which is input, which is a real frame. */
void
gui_volatile_cb_close( gui_volatile_fn fn )
{
    if ( s_open_id == GUI_ID_NONE ) return;
    gui_volatile_slot_t* row = volatile_find( s_open_id );
    if ( row )
    {
        u32  lo = s_open_cmd_lo, hi = s_draw.cmd_count;
        bool any_visible = false;
        for ( u32 i = lo; i < hi; ++i )
        {
            s_draw.cmd_volatile_id[ i ] = s_open_id;
            if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
                any_visible = true;
        }
        row->cmd_lo = (u16)lo;
        row->cmd_hi = (u16)hi;
        row->fn     = fn;
        row->hidden = !any_visible;
    }
    s_open_id = GUI_ID_NONE;
}

/* Called from cache_diff_windows (gui_build_cache.c) for every volatile-tagged command -- true
   when the row has no usable capture for its window's CURRENT slot (never captured, retired by a
   failed patch, or the slot was re-tessellated without the range re-capturing), meaning the
   window must be forced CHANGED this frame so tessellation runs and volatile_range_close
   (re)captures it.  Hidden rows return false: they cannot capture while clip-empty, and forcing
   a rebuild for an off-screen widget would defeat the idle skip for nothing. */
static bool
volatile_row_needs_capture( gui_id_t id )
{
    gui_volatile_slot_t* row = volatile_find( id );
    if ( !row || row->hidden )
        return false;
    if ( !row->active )
        return true;
    u32 vb, ib, cb, gen;
    if ( !cache_slot_lookup( row->win, &vb, &ib, &cb, &gen ) )
        return true;   /* window (re)appearing this frame -- it will tessellate anyway */
    (void)vb; (void)ib; (void)cb;
    return gen != row->tess_gen;
}

/* Called from tess_dispatch (gui_build_tess.c) once a tagged command RANGE's vertices, indices
   and GPU commands are fully written into the window slot currently being tessellated.  Records
   the block's slot-relative position, then reserves headroom: the write heads advance past the
   live geometry by the (grow-only) allocation, and the slot's GPU command run is padded with
   dormant commands (elem_count 0, vp GUI_VP_INVALID -- skipped by every surface's flush) so a
   later patch can use more commands than the original emit without shifting its neighbours.
   Reservations are clamped to the shared buffers -- headroom shrinks before correctness does. */
static void
volatile_range_close( gui_id_t id, u32 vb_open, u32 ib_open, u32 cmd_open )
{
    gui_volatile_slot_t* row = volatile_find( id );
    if ( !row ) return;

    u32 nv = s_tess.vert_count - vb_open;
    u32 ni = s_tess.idx_count  - ib_open;
    u32 nc = s_tess.cmd_count  - cmd_open;

    /* Grow-only reservation: never smaller than the last one (or than what a failed patch
       recorded it actually needed -- see volatile_patch), so a block that once grew keeps its
       room and the overflow -> real frame -> recapture cycle cannot repeat for the same size. */
    u32 res_v = nv + VOL_VERT_PAD; if ( res_v < row->vert_alloc ) res_v = row->vert_alloc;
    u32 res_i = ni + VOL_IDX_PAD;  if ( res_i < row->idx_alloc  ) res_i = row->idx_alloc;
    u32 res_c = nc + VOL_CMD_PAD;  if ( res_c < row->cmd_alloc  ) res_c = row->cmd_alloc;

    if ( vb_open  + res_v > GUI_MAX_VERTS ) res_v = GUI_MAX_VERTS - vb_open;
    if ( ib_open  + res_i > GUI_MAX_IDX   ) res_i = GUI_MAX_IDX   - ib_open;
    if ( cmd_open + res_c > GUI_MAX_CMDS  ) res_c = GUI_MAX_CMDS  - cmd_open;

    /* Advance the write heads over the reservation; pad the command run with dormant commands so
       the slot's [cmd_base, cmd_base + cmd_count) range stays dense.  The gap vertices/indices
       are never referenced: draw calls read elem_count indices from each command's own cmd_ibase. */
    s_tess.vert_count = vb_open + res_v;
    s_tess.idx_count  = ib_open + res_i;
    for ( u32 k = nc; k < res_c; ++k )
    {
        u32 ci = cmd_open + k;
        s_tess.cmds     [ ci ] = ( gui_gpu_cmd_t ){ .elem_count = 0, .tex_idx = 0,
                                                    .clip_rect = s_tess.cur_clip };
        s_tess.cmd_vp   [ ci ] = GUI_VP_INVALID;
        s_tess.cmd_vbase[ ci ] = s_tess.vert_count;
        s_tess.cmd_ibase[ ci ] = s_tess.idx_count;
    }
    s_tess.cmd_count     = cmd_open + res_c;
    s_tess.force_new_cmd = true;   /* the next window primitive must not merge into a dormant slot */

    row->local_vert_base = (u16)( vb_open  - s_tess.slot_vert_base );
    row->local_idx_base  = (u16)( ib_open  - s_tess.slot_idx_base  );
    row->local_cmd_base  = (u16)( cmd_open - s_tess.slot_cmd_base  );
    row->vert_count      = (u16)nv;  row->vert_alloc = (u16)res_v;
    row->idx_count       = (u16)ni;  row->idx_alloc  = (u16)res_i;
    row->cmd_count       = (u16)nc;  row->cmd_alloc  = (u16)res_c;
    row->tess_gen        = s_tess.slot_tess_gen;
    row->active          = true;
}

/* The one update primitive, shared by both patch paths.  Re-tessellates the LIVE commands at
   s_draw.cmds[lo, hi) into scratch space at the current tail of s_tess (rolled back afterward,
   success or not), and -- if the result fits the row's reservation -- copies the geometry into
   the block's region of the owning window's CURRENT slot (resolved by id, generation-checked)
   and rewrites the block's GPU commands in place: new clip/texture/elem_count, vbase/ibase
   re-based from scratch coordinates to the block's absolute position, unused reserved command
   slots left dormant.  On an overflow the row records the size it actually needed so the forced
   recapture reserves enough, and returns false -- the caller retires the row and invalidates the
   window. */
static bool
volatile_patch( gui_volatile_slot_t* row, u32 lo, u32 hi )
{
    u32 slot_vb, slot_ib, slot_cb, slot_gen;
    if ( !cache_slot_lookup( row->win, &slot_vb, &slot_ib, &slot_cb, &slot_gen ) )
        return false;
    if ( slot_gen != row->tess_gen )
        return false;

    /* Natural-order permutation with the same empty-clip filter cache_tess_window applies, so
       the patch tessellates exactly the command set a real capture would. */
    static u32 scratch_order[ GUI_MAX_CMDS ];
    static u32 scratch_font [ GUI_MAX_CMDS ];
    u32 n = 0;
    for ( u32 i = lo; i < hi; ++i )
        if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
        {
            scratch_font [ n ] = row->font;
            scratch_order[ n++ ] = i;
        }

    u32  vert_ck    = s_tess.vert_count;
    u32  idx_ck     = s_tess.idx_count;
    u32  tcmd_ck    = s_tess.cmd_count;
    u32  slot_vb_ck = s_tess.slot_vert_base;
    bool force_ck   = s_tess.force_new_cmd;
    bool ovf_ck     = s_tess.overflow;

    /* Debug guard: the scratch tessellation below writes at vert_ck / idx_ck and its byte content
       survives the count rollback.  If that is not past every live slot's reservation, it scribbles
       through another window's geometry (the tooltip-vs-pulse collision).  Callers must arrange the
       true tail: update_volatile runs on idle frames where vert_count already is it;
       volatile_patch_reused_window is deferred until after the whole slot loop for the same reason. */
#if !RELEASE
    {
        u32 vend, iend;
        cache_slots_extent( &vend, &iend );
        ORB_ASSERT_MSG( vert_ck >= vend && idx_ck >= iend,
                        "gui volatile: patch scratch would overlap live slot geometry" );
    }
#endif

    /* slot_vert_base is faked so the scratch indices come out relative to the ORIGINAL window
       slot -- index value = (scratch position - fake base) = local_vert_base + offset -- and can
       be memcpy'd into place unmodified.  s_volatile_patching keeps tess_dispatch's range
       tracking inert (a patch must never look like a fresh capture). */
    s_tess.slot_vert_base = vert_ck - row->local_vert_base;
    s_tess.force_new_cmd  = true;
    s_volatile_patching   = true;

    tess_dispatch( s_draw.cmds, scratch_order, scratch_font, n, row->win );

    s_volatile_patching = false;

    u32  nv          = s_tess.vert_count - vert_ck;
    u32  ni          = s_tess.idx_count  - idx_ck;
    u32  nc          = s_tess.cmd_count  - tcmd_ck;
    bool scratch_ovf = s_tess.overflow && !ovf_ck;   /* scratch itself hit the buffer cap */

    bool ok = !scratch_ovf
            && nv <= (u32)row->vert_alloc
            && ni <= (u32)row->idx_alloc
            && nc <= (u32)row->cmd_alloc;

    if ( ok )
    {
        u32 abs_vb = slot_vb + row->local_vert_base;
        u32 abs_ib = slot_ib + row->local_idx_base;
        u32 abs_cb = slot_cb + row->local_cmd_base;

        memcpy( &s_tess.verts  [ abs_vb ], &s_tess.verts  [ vert_ck ], nv * sizeof( gui_draw_vert_t ) );
        memcpy( &s_tess.indices[ abs_ib ], &s_tess.indices[ idx_ck  ], ni * sizeof( u16 ) );

        for ( u32 k = 0; k < row->cmd_alloc; ++k )
        {
            u32 dst = abs_cb + k;
            if ( k < nc )
            {
                u32 src = tcmd_ck + k;
                s_tess.cmds     [ dst ] = s_tess.cmds  [ src ];
                s_tess.cmd_vp   [ dst ] = s_tess.cmd_vp[ src ];
                s_tess.cmd_vbase[ dst ] = abs_vb + ( s_tess.cmd_vbase[ src ] - vert_ck );
                s_tess.cmd_ibase[ dst ] = abs_ib + ( s_tess.cmd_ibase[ src ] - idx_ck  );
            }
            else
            {
                s_tess.cmds  [ dst ].elem_count = 0;
                s_tess.cmd_vp[ dst ]            = GUI_VP_INVALID;
            }
        }
        row->vert_count = (u16)nv;
        row->idx_count  = (u16)ni;
        row->cmd_count  = (u16)nc;
    }
    else if ( !scratch_ovf )
    {
        /* Outgrew the reservation: remember the real need (plus fresh headroom) so the forced
           recapture reserves enough.  volatile_range_close clamps to the shared buffers. */
        if ( nv > (u32)row->vert_alloc ) row->vert_alloc = (u16)( nv + VOL_VERT_PAD );
        if ( ni > (u32)row->idx_alloc  ) row->idx_alloc  = (u16)( ni + VOL_IDX_PAD  );
        if ( nc > (u32)row->cmd_alloc  ) row->cmd_alloc  = (u16)( nc + VOL_CMD_PAD  );
    }

    /* Roll s_tess back -- as far as this frame's real geometry/dispatch table is concerned, the
       scratch tessellation never happened. */
    s_tess.vert_count     = vert_ck;
    s_tess.idx_count      = idx_ck;
    s_tess.cmd_count      = tcmd_ck;
    s_tess.slot_vert_base = slot_vb_ck;
    s_tess.force_new_cmd  = force_ck;
    s_tess.overflow       = ovf_ck;
    return ok;
}

/* Called from cache_build_frame (gui_build_cache.c) right after a window's slot is set up via
   reuse_geo (the window's non-volatile content matched, so it is being fully reused this real
   frame) -- patch every live row belonging to `win` from the commands this frame's real emit
   already produced.  A failure retires the row and invalidates the window so the next frame
   re-tessellates and recaptures it at the recorded larger size.  Returns the count patched, for
   the volatile_patched stat. */
static u32
volatile_patch_reused_window( gui_id_t win )
{
    u32  patched = 0;
    bool failed  = false;
    for ( u32 i = 0; i < s_volatile_count; ++i )
    {
        gui_volatile_slot_t* row = &s_volatile[ i ];
        if ( row->win != win || !row->active || row->hidden )
            continue;

        if ( volatile_patch( row, row->cmd_lo, row->cmd_hi ) )
            ++patched;
        else
        {
            row->active = false;
            failed      = true;
        }
    }
    if ( failed )
        cache_invalidate_window( win );
    return patched;
}

/* Host entry point for a clean frame (gui_frame_dirty() == false): re-invoke every live row's
   callback standalone inside a minimal replay scope and patch its reserved region in place.
   Rows whose window has no live slot, or whose slot was re-tessellated without them (generation
   mismatch), or that are clip-hidden, are skipped silently -- they are not on screen, and the
   real frame that changes that also recaptures them.  A patch FAILURE (reservation overflow)
   retires the row, invalidates the window, and asks for one real frame via
   gui_replay_scope_exit's force_redraw so nothing stays visibly stale. */
void
gui_update_volatile( void )
{
    if ( s_draw.seg_count == 0 )
        return;   /* no frame has ever emitted -- nothing to patch, and no segment to checkpoint */

    for ( u32 i = 0; i < s_volatile_count; ++i )
    {
        gui_volatile_slot_t* row = &s_volatile[ i ];
        if ( !row->active || row->hidden || !row->fn )
            continue;

        /* Resolve the owning window's current slot BEFORE replaying -- a row that cannot be
           patched (window gone, or rebuilt without it) skips the callback entirely. */
        u32 slot_vb, slot_ib, slot_cb, slot_gen;
        if ( !cache_slot_lookup( row->win, &slot_vb, &slot_ib, &slot_cb, &slot_gen )
             || slot_gen != row->tess_gen )
            continue;
        (void)slot_vb; (void)slot_ib; (void)slot_cb;

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
        f32        alpha_ck      = s_draw.alpha;
        f32        rounding_ck   = s_draw.rounding;
        f32        tclip_x0_ck   = s_draw.text_clip_x0;
        f32        tclip_x1_ck   = s_draw.text_clip_x1;

        s_draw.cur_win      = row->win;
        s_draw.cur_z        = row->z;
        s_draw.cur_vp       = row->vp;
        s_draw.cur_font     = row->font;
        s_draw.cur_clip_idx = row->clip_idx;
        font_use( row->font );

        /* Same reasoning as the clip-stack force below: alpha/rounding/text-clip are ambient
           scalars a raw draw_ call reads directly, not part of the minimal scope
           gui_replay_scope_enter reconstructs, so without this the callback would draw with
           whatever an unrelated idle frame's leftover s_draw state happens to be. */
        s_draw.alpha        = row->alpha;
        s_draw.rounding     = row->rounding;
        s_draw.text_clip_x0 = row->text_clip_x0;
        s_draw.text_clip_x1 = row->text_clip_x1;

        /* draw_cull_box (gui_emit_draw.c) tests against clip_stack[clip_depth-1], NOT
           cur_clip_idx -- cur_clip_idx alone is not enough to reproduce the real-emit clip. Force
           a one-deep stack whose top is the captured clip's resolved rect, or a callback whose
           rect happens to sit outside whatever clip was left on the stack by the last real emit
           gets silently culled during replay. */
        s_draw.clip_stack    [ 0 ] = s_draw.clip_table[ row->clip_idx ];
        s_draw.clip_idx_stack[ 0 ] = row->clip_idx;
        s_draw.clip_depth          = 1;

        gui_replay_scope_enter( row->id, row->x, row->y, row->w );
        row->fn( true );
        u32 cmd_hi = s_draw.cmd_count;

        bool ok = volatile_patch( row, cmd_ck, cmd_hi );
        if ( ok )
            cache_count_volatile_patch( 1 );
        else
        {
            /* Reservation overflow (the resolve above already ruled out a stale slot): retire and
               force one real frame -- the recapture reserves the larger size volatile_patch just
               recorded, so this cannot repeat for the same content. */
            row->active = false;
            cache_invalidate_window( row->win );
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
        s_draw.alpha                = alpha_ck;
        s_draw.rounding             = rounding_ck;
        s_draw.text_clip_x0         = tclip_x0_ck;
        s_draw.text_clip_x1         = tclip_x1_ck;
        font_use( font_ck );
    }
}

// clang-format on
/*============================================================================================*/
