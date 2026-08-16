/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_volatile.c -- Volatile widgets, render half.

    An inline-emit callback replayed in place on frames the UI build is skipped entirely
    (gui_frame_dirty() false) -- see gui.h (gui_volatile_fn) for the full contract and
    chrome/widgets/gui_volatile.c for the frontend half of this seam.

    Design: every volatile block owns a RESERVED, PADDED sub-region of its window's geometry
    slot -- a vertex span, an index span, and a run of GPU commands, each allocated with headroom
    past what the live emit actually produced -- and every update is a plain re-tessellation into
    that reservation.  There is no requirement that a replay reproduce the exact topology of the
    original emit: text may grow or shrink, rounding categories may flip, glyph counts may change.
    The only way an update can fail is by outgrowing the reservation, and that failure is
    self-healing: the row records what it actually needed, the owning window is invalidated
    (cache_invalidate_window) so the next real frame re-tessellates it, and the recapture reserves
    the larger size.  Reservations are grow-only per id.

    Layout integrity: the reservation above bounds what a block may DRAW; the second, independent
    check bounds the space it may OCCUPY.  gui_volatile_cb measures the layout extent every real
    emit claims (volatile_footprint), each replay measures its own the same way
    (replay_scope_measure), and a disagreement forces one real frame -- a block that grows past
    its cell overlaps neighbours that are frozen cached geometry and cannot move until layout runs
    again, so the fixed-footprint contract in gui.h is now enforced rather than merely documented.
    volatile_footprint_reflow carries the strike counter that keeps a callback which NEVER agrees
    from buying a real frame every frame forever.

    Position integrity: a row never stores an absolute buffer address.  It stores its position
    RELATIVE to the owning window's slot (local_vert_base / local_idx_base / local_cmd_base) plus
    the slot's tessellation generation at capture time (tess_gen, bumped by cache_build_frame on
    every retess of the window).  At patch time the window's CURRENT slot is resolved by id from
    the live slot table (cache_slot_lookup) and the absolute address computed fresh; the patch
    proceeds only if the slot's generation still matches the capture.  A window whose slot moved
    (sibling reflow) resolves correctly automatically; a window re-tessellated without the widget
    re-emitting (content branch hid it) fails the generation check and the patch is skipped --
    a stale address physically cannot be produced.

    Real emit: gui_volatile_cb (chrome/widgets/gui_volatile.c) brackets one inline invocation of the
    caller's callback with volatile_cb_open/_close (this file), which record the command range
    it produced and tag it with the row id; volatile_stamp (called from inside the callback by
    gui_volatile_begin) records the window/z/vp/font/clip context, the ambient
    alpha/rounding/text-clip scalars a raw draw_ call reads directly, the layout cursor
    position, and the owning region's view/pad (reinstalled on the replay layout frame).  
    When the window tessellates, tess_dispatch (gui_build_tess.c) calls
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

        volatile_update          -- idle frames: gui_frame_end calls it when s_frame_dirty is
                                        false (a host never wires it); each row's callback is
                                        re-invoked standalone inside replay_scope_enter/_exit
                                        and patched.
        volatile_patch_reused_window -- real frames where the window's slot is reused: patches from
                                        the commands this frame's live emit already produced.

    Either way, cache_count_volatile_patch (gui_build_cache.c) tallies the patch into
    gui_render_stats_t.volatile_patched -- reported separately from win_retained precisely so a
    window with an animating volatile widget still correctly counts as retained.

    Included by gui_render.c after gui_build_tess.c (needs s_tess, tess_dispatch, and
    s_volatile_patching, defined there) and before gui_build_cache.c (which defines the
    cache_* helpers forward-declared below and calls volatile_row_needs_capture /
    volatile_row_confined / volatile_patch_reused_window; gui_render_flush uploads the patched spans for free since a
    slot's upload range covers its reservations).

==============================================================================================*/
// clang-format off

/* GUI_MAX_VOLATILE lives in gui_render.h (the dashboard snapshot types are sized by it). */
#define VOL_VERT_PAD      128u   /* vertex headroom reserved past a block's live geometry        */
#define VOL_IDX_PAD       192u   /* index headroom (~1.5x vertices for quad-heavy content)       */
#define VOL_CMD_PAD       2u     /* dormant GPU-command slots reserved past the block's live run */

/* Layout-footprint reflow check (volatile_footprint_reflow).  EPS absorbs the sub-pixel noise of
   two independent layout passes agreeing.  STRIKES bounds one specific pathology -- see
   volatile_footprint: a block whose footprint keeps CHANGING is legitimate (it buys a real
   frame per change, which is the point), but a callback that simply does not lay out the same way
   under the replay scope would buy one every single frame forever, silently retiring the idle skip
   with no way to reach agreement.  The two are told apart by whether the real emit that follows a
   forced frame agrees with what the replay drew. */
#define VOL_FOOT_EPS      0.5f
#define VOL_FOOT_STRIKES  4u

/* Largest value a slot's u16 count / slot-local offset can carry (see the field-width note on
   gui_volatile_slot_t).  Reservations clamp to it; live geometry past it is not captured. */
#define VOL_LOCAL_MAX     0xFFFFu

/* Field widths: every count and slot-local offset below is u16, so a captured block addresses at
   most 64K of each resource.  GUI_MAX_VERTS and GUI_MAX_CMDS sit under that in every build
   (stress included); GUI_MAX_IDX does NOT, so volatile_range_close refuses to capture a
   block whose indices would not fit rather than truncating the cast -- unreachable for a real
   volatile block (an animating readout is a handful of quads) and independent of how the pools
   are sized later.  tess_gen is full u32 -- it must never alias across a wrap, since it is the
   sole guard that a patch writes into geometry produced by the exact tessellation pass that
   captured it. */
typedef struct
{
    gui_id_t         id, win;
    f32              x, y, w;          // layout cursor stamp at gui_volatile_begin
    gui_rect_t       view;             // owning region's view rect at stamp -- the replay frame's view
    gui_pad_t        pad;              // owning region's pad at stamp (text self-fit reads it)
    bool             stamped;          // gui_volatile_begin ran -- without it the row cannot replay
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
    bool             confined;         // volatile_cb_close scissored the range to its cell -- the
                                       //   licence cache_tess_window needs to emit it at the tail

    /* Layout footprint: the extent the block claimed at its last REAL emit, measured by the probe
       in gui_volatile_cb (chrome/widgets/gui_volatile.c).  Idle replays measure their own the same
       way and compare --
       the geometry reservation guards what the block DRAWS, this guards the space it OCCUPIES. */
    f32              foot_w, foot_h;     // real emit's extent (the reference a replay is checked against)
    f32              rfoot_w, rfoot_h;   // extent of the replay that forced the pending real frame
    u8               foot_strikes;       // real frames that disagreed with the replay that forced them
    bool             foot_valid;         // a real emit measured it (the probe ran)
    bool             foot_pending;       // a reflow-forced real frame is owed a verdict
    bool             foot_unstable;      // latched after VOL_FOOT_STRIKES: stop buying frames for it

    /* Ambient s_draw scalars in effect at the moment gui_volatile_begin stamped this row --
       alpha, rounding, the text-clip window, and the packed text-edge word are read directly off
       s_draw by the raw draw_ calls a callback makes, the same way cur_win/cur_z/cur_vp/cur_font
       are.  Stamped here and reinstalled by volatile_update for the duration of the standalone
       replay call so the callback sees the same ambient values it drew with at real emit,
       whatever the idle frame's leftover s_draw state happens to be. */
    f32              alpha, rounding, text_clip_x0, text_clip_x1;
    u32              text_edge;

} gui_volatile_slot_t;

static gui_volatile_slot_t s_volatile[ GUI_MAX_VOLATILE ];
static u32                 s_volatile_count;

/* Defined later in gui_build_cache.c (same TU, included right after this file) where s_stats,
   s_slots and s_cache live -- forward-declared here the same way gui_build_tess.c forward-declares
   volatile_range_close.
     cache_count_volatile_patch -- stats: rows patched in place this frame.
     cache_slot_lookup          -- resolve a window's CURRENT slot position + tessellation
                                   generation by id; false if the window has no live slot.
     cache_slot_vp              -- the window's current viewport, tagging a patch's dirty spans
                                   (patch_span_union) with the flush that must re-upload them.
     cache_slot_clips_bind      -- point s_tess at the window slot's LOCAL clip table so a
                                   patch's scratch tessellation resolves (and, for genuinely new
                                   rects, appends) the same local clip indices the capture baked.
     cache_invalidate_window    -- corrupt the window's stored hash + raise any_changed so the
                                   next frame re-tessellates it (a failed patch's recovery path).
     cache_slots_extent         -- far edge of every slot's reservation; the debug guard below
                                   asserts scratch is written past it (!RELEASE only). */
static void cache_count_volatile_patch( u32 n );
static bool cache_slot_lookup( gui_id_t win, u32* vert_base, u32* idx_base, u32* cmd_base,
                               u32* tess_gen );
static u8   cache_slot_vp( gui_id_t win, u8* out_band );
static bool cache_slot_clips_bind( gui_id_t win );
static void cache_invalidate_window( gui_id_t win );
#if !RELEASE
static void cache_slots_extent( u32* out_vert_end, u32* out_idx_end );
#endif

/* The row currently mid-callback during real emit (between volatile_cb_open and _close).
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

/* Called by gui_volatile_cb (chrome/widgets/gui_volatile.c) right before it invokes the callback inline
   during real emit -- opens the command-range bracket for `id`.  A full registry degrades
   gracefully: the bracket never opens, the commands stay untagged, and the widget behaves as a
   plain (hash-participating) widget that animates through ordinary dirty frames. */
void
volatile_cb_open( gui_id_t id )
{
    if ( !volatile_find_or_add( id ) )
    {
        s_open_id = GUI_ID_NONE;
        return;
    }
    s_open_id     = id;
    s_open_cmd_lo = s_draw.cmd_count;
}

/* Called by gui_volatile_begin (chrome/widgets/gui_volatile.c), from inside the callback body during
   real emit -- stamps the emit context (window/z/vp/font/clip), the layout cursor position
   (x, y, w) the callback started at, and the owning region's view/pad, so replay can reconstruct
   a matching scope later (the replay layout frame installs view/pad verbatim -- widgets read them
   mid-emit for self-fit decisions). */
void
volatile_stamp( f32 x, f32 y, f32 w, const gui_rect_t* view, gui_pad_t pad )
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
    row->view     = *view;
    row->pad      = pad;
    row->stamped  = true;
    row->alpha        = s_draw.alpha;
    row->rounding     = s_draw.rounding;
    row->text_clip_x0 = s_draw.text_clip_x0;
    row->text_clip_x1 = s_draw.text_clip_x1;
    row->text_edge    = s_draw.text_edge;
}

static bool
volatile_foot_eq( f32 aw, f32 ah, f32 bw, f32 bh )
{
    return fabsf( aw - bw ) <= VOL_FOOT_EPS && fabsf( ah - bh ) <= VOL_FOOT_EPS;
}

/* Called by gui_volatile_cb (chrome/widgets/gui_volatile.c) right after the callback returns
   during real emit, before the bracket closes -- records the layout extent the block just claimed,
   measured relative to its cell origin.  This is the reference every idle replay checks itself
   against (volatile_footprint_reflow), re-measured on EVERY real emit so a block that legitimately
   changes size settles immediately: the replay that spotted the change bought this frame, this
   frame laid the neighbours out around the new size, and the next replay agrees again.
     This is also where the two ways a footprint can disagree are told apart, because only here is
   the answer known.  A real frame forced by a reflow arrives owing a verdict (foot_pending): if it
   lays out to the size the replay drew, the replay was RIGHT -- the content genuinely changed, the
   forced frame did its job, strikes clear.  If it lays out to some other size, the replay and the
   real emit simply disagree about the same content, and no number of forced frames will close the
   gap; count a strike, and once they run out stop buying frames for this row (a permanent
   full-rate redraw is a worse failure than the overlap it was trying to fix). */

void
volatile_footprint( f32 w, f32 h )
{
    if ( s_open_id == GUI_ID_NONE ) return;
    gui_volatile_slot_t* row = volatile_find( s_open_id );
    if ( !row ) return;

    if ( row->foot_pending )
    {
        row->foot_pending = false;
        if ( volatile_foot_eq( w, h, row->rfoot_w, row->rfoot_h ) )
            row->foot_strikes = 0;
        else if ( ++row->foot_strikes >= VOL_FOOT_STRIKES )
        {
            row->foot_unstable = true;   /* set first: the assert below reads it, and a runtime
                                            expression keeps MSVC's constant-conditional warning off */
            ORB_ASSERT_MSG( !row->foot_unstable,
                            "gui volatile: callback lays out to a different size on replay than at "
                            "real emit -- not reproducible under the replay scope (gui.h: flow "
                            "layouts only, no ambient layout modifiers)" );
        }
    }

    row->foot_w     = w;
    row->foot_h     = h;
    row->foot_valid = true;
}

/* True when a replay's layout extent disagrees with what the last real emit claimed -- the block
   grew (or shrank) out of the cell the retained neighbours were laid out against, so what is on
   screen right now is wrong: the neighbours are frozen cached geometry and cannot move until a
   real frame re-runs layout.  Buying that frame is the entire remedy; the caller folds the result
   into replay_scope_exit's force_redraw, exactly as it does a reservation overflow.  The
   replay's own measurement is kept so the frame it just bought can be graded (volatile_footprint). */
static bool
volatile_footprint_reflow( gui_volatile_slot_t* row, f32 w, f32 h )
{
    if ( !row->foot_valid || row->foot_unstable )
        return false;

    if ( volatile_foot_eq( w, h, row->foot_w, row->foot_h ) )
        return false;

    row->rfoot_w      = w;
    row->rfoot_h      = h;
    row->foot_pending = true;
    return true;
}

/* Called by gui_volatile_cb right after the callback returns during real emit -- closes the
   command-range bracket, tags every command in it with `id` (cmd_volatile_id is a range tag, not
   a single-command tag), stores the callback pointer, and CONFINES the range to `cell`: the
   block's own layout cell, already intersected with its region's view by the caller.  NULL when
   the callback never opened the footprint probe (no gui_volatile_begin), which is the same
   condition that leaves the row unreplayable -- nothing to confine.

   The confinement is stamped here rather than pushed before the callback because the cell is not
   KNOWN until the callback returns and its footprint is measured; the commands are still plain
   unconsumed data in the emit pool at this point, so re-pointing their clip index is free.

   Two things come of it, and the second is why this exists at all:

     - A block can no longer paint outside its own cell.  It never should have -- gui.h's
       fixed-footprint contract says so and volatile_footprint asserts on it -- but without the
       cut the only thing stopping a block scrolled under a title bar from covering it is the
       chrome being drawn afterwards (window_open_body: one clip for the whole window, scroll-out
       resolved by OVERPAINT rather than by clipping).  Cutting is also strictly cheaper than
       shading pixels that get painted over.
     - The cut is the LICENCE to paint the block last.  A block was always going to own its GPU
       command(s) (a patch must be able to rewrite their elem_counts), so a block sitting mid-body
       cuts the window into three draw calls -- content before, block, content after.  Because a
       confined block provably cannot touch anything outside its own cell, cache_tess_window may
       emit its range at the TAIL of the window's permutation (`confined` is that gate), where the
       content on either side of it merges back into one command and the reservation padding sits
       at the slot's end instead of mid-body.

   Also computes `hidden`: a range whose every command sits in an empty clip (scrolled out of a
   container) produces no geometry when the window tessellates, so there is nothing to capture or
   patch until it becomes visible again -- which takes a scroll, which is input, which is a real
   frame.  Measured AFTER the confinement, so a block scrolled out of its region's view is now
   recognized as hidden by the same test. */
void
volatile_cb_close( gui_volatile_fn fn, const gui_rect_t* cell )
{
    if ( s_open_id == GUI_ID_NONE ) return;
    gui_volatile_slot_t* row = volatile_find( s_open_id );
    if ( row )
    {
        u32  lo = s_open_cmd_lo, hi = s_draw.cmd_count;
        bool any_visible = false;

        /* One-entry memo over the source clip: a block's commands almost always share the ambient
           clip, so this resolves the intersection once.  Nested pushes inside the callback are
           still honoured -- each distinct source clip gets its own confined rect. */
        u8 memo_src = 0xFFu, memo_dst = 0u;

        for ( u32 i = lo; i < hi; ++i )
        {
            s_draw.cmd_volatile_id[ i ] = s_open_id;

            if ( cell )
            {
                u8 src = s_draw.cmds[ i ].clip_idx;
                if ( src != memo_src )
                {
                    memo_src = src;
                    memo_dst = clip_append( rect_intersect( *cell, s_draw.clip_table[ src ] ),
                                            0.0f );
                }
                s_draw.cmds[ i ].clip_idx = memo_dst;
            }

            if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
                any_visible = true;
        }

        /* Re-point the replay scope at the confined clip.  volatile_stamp recorded the AMBIENT one
           from inside the callback, before the cell was known; volatile_update reinstalls
           row->clip_idx as both cur_clip_idx and the one-deep clip stack, so leaving it stale
           would replay the block under a looser clip than the real emit drew with. */
        if ( cell && hi > lo )
            row->clip_idx = s_draw.cmds[ lo ].clip_idx;

        row->cmd_lo   = (u16)lo;
        row->cmd_hi   = (u16)hi;
        row->fn       = fn;
        row->hidden   = !any_visible;
        row->confined = ( cell != NULL );
    }
    s_open_id = GUI_ID_NONE;
}

/* True when `id`'s live command range is confined to its cell (forward gate for
   cache_tess_window's tail emission -- an unconfined range must keep its emission position, since
   nothing bounds where it paints). */
static bool
volatile_row_confined( gui_id_t id )
{
    gui_volatile_slot_t* row = volatile_find( id );
    return row && row->confined;
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

    /* u16 field widths (see the slot struct): the reservation shrinks to what the row can address,
       exactly as it shrinks to the arena above, and a block whose LIVE geometry already overruns
       16 bits is left uncaptured -- it still renders, it just never replays.  Keeps every cast
       below provably lossless without tying the row layout to the pool sizes. */
    if ( res_v > VOL_LOCAL_MAX ) res_v = VOL_LOCAL_MAX;
    if ( res_i > VOL_LOCAL_MAX ) res_i = VOL_LOCAL_MAX;
    if ( res_c > VOL_LOCAL_MAX ) res_c = VOL_LOCAL_MAX;

    if ( nv > VOL_LOCAL_MAX || ni > VOL_LOCAL_MAX || nc > VOL_LOCAL_MAX
         || ( ib_open - s_tess.slot_idx_base ) > VOL_LOCAL_MAX )
    {
        row->active = false;
        return;
    }

    /* Advance the write heads over the reservation; pad the command run with dormant commands so
       the slot's [cmd_base, cmd_base + cmd_count) range stays dense.  The gap vertices/indices
       are never referenced: draw calls read elem_count indices from each command's own ibase. */
    s_tess.vert_count = vb_open + res_v;
    s_tess.idx_count  = ib_open + res_i;
    for ( u32 k = nc; k < res_c; ++k )
    {
        u32 ci = cmd_open + k;
        s_tess.gpu_cmds[ ci ] = ( tess_gpu_cmd_t ){
            .cmd   = { .elem_count = 0, .tex_idx = 0, .clip_rect = s_tess.cur_clip },
            .vp    = GUI_VP_INVALID,
            .vbase = s_tess.vert_count,
            .ibase = s_tess.idx_count,
        };
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
/* Patch-time permutation scratch (file scope so the memory accounting sees it; volatile_patch
   is single-threaded within the build).  u16 like the cache_tess_window scratch: command
   indices fit (asserted at gui_cmd_seg_t). */
static u16 s_patch_order[ GUI_MAX_CMDS ];

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
    u32 n = 0;
    for ( u32 i = lo; i < hi; ++i )
        if ( !rect_empty( s_draw.clip_table[ s_draw.cmds[ i ].clip_idx ] ) )
            s_patch_order[ n++ ] = (u16)i;

    u32  vert_ck    = s_tess.vert_count;
    u32  idx_ck     = s_tess.idx_count;
    u32  tcmd_ck    = s_tess.cmd_count;
    u32  slot_vb_ck = s_tess.slot_vert_base;
    bool force_ck   = s_tess.force_new_cmd;
    bool ovf_ck     = s_tess.overflow;

    gui_clip_entry_t* clips_ck        = s_tess.slot_clips;
    u32*              clip_count_ck   = s_tess.slot_clip_count;
    u8*               clip_pending_ck = s_tess.slot_clip_pending;
    u32               clip_base_ck    = s_tess.slot_clip_base;

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

    /* The patched vertices bake clip-band indices exactly like the capture's did, so resolve
       them against the SAME local table -- the window slot's.  An unchanged footprint clip finds
       its existing entry; a genuinely new rect appends and uploads with the next flush. */
    cache_slot_clips_bind( row->win );

    tess_dispatch( s_draw.cmds, s_patch_order, n, row->win );

    s_volatile_patching      = false;
    s_tess.slot_clips        = clips_ck;
    s_tess.slot_clip_count   = clip_count_ck;
    s_tess.slot_clip_pending = clip_pending_ck;
    s_tess.slot_clip_base    = clip_base_ck;
    s_tess.clip_memo_ci      = 0xFF;

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

        /* Fine dirty spans, not a generation bump: only these ranges changed, so a
           generation-matching flush re-uploads just them (gui_build_tess.c, s_patch_pending). */
        u8 row_band;
        u8 row_vp = cache_slot_vp( row->win, &row_band );
        patch_span_union( row_vp, row_band, abs_vb, abs_vb + nv, abs_ib, abs_ib + ni );
        memcpy( &s_tess.verts  [ abs_vb ], &s_tess.verts  [ vert_ck ], nv * sizeof( gui_draw_vert_t ) );
        memcpy( &s_tess.indices[ abs_ib ], &s_tess.indices[ idx_ck  ], ni * sizeof( u16 ) );

        for ( u32 k = 0; k < row->cmd_alloc; ++k )
        {
            u32 dst = abs_cb + k;
            if ( k < nc )
            {
                u32 src = tcmd_ck + k;
                s_tess.gpu_cmds[ dst ]       = s_tess.gpu_cmds[ src ];   /* cmd + vp ride along */
                s_tess.gpu_cmds[ dst ].vbase = abs_vb + ( s_tess.gpu_cmds[ src ].vbase - vert_ck );
                s_tess.gpu_cmds[ dst ].ibase = abs_ib + ( s_tess.gpu_cmds[ src ].ibase - idx_ck  );
            }
            else
            {
                s_tess.gpu_cmds[ dst ].cmd.elem_count = 0;
                s_tess.gpu_cmds[ dst ].vp = GUI_VP_INVALID;
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

/* Registered row count -- the perf overlay's "vol rows" readout against GUI_MAX_VOLATILE. */
u32
volatile_row_count( void )
{
    return s_volatile_count;
}

/* True while at least one row would actually patch on an idle frame -- the same filter
   volatile_update applies (active, on screen, window slot live at the captured generation).
   gui_boot_pace reads this to keep presenting at the animation cadence instead of blocking on
   OS input: a volatile block only advances when a frame runs, so a blocking wait freezes it
   until a timeout/spurious wakeup and the animation stutters at the wait interval. */
bool
gui_volatile_live( void )
{
    for ( u32 i = 0; i < s_volatile_count; ++i )
    {
        gui_volatile_slot_t* row = &s_volatile[ i ];
        if ( !row->active || row->hidden || !row->fn )
            continue;
        u32 vb, ib, cb, gen;
        if ( cache_slot_lookup( row->win, &vb, &ib, &cb, &gen ) && gen == row->tess_gen )
            return true;
    }
    return false;
}

/* Host entry point for a clean frame (gui_frame_dirty() == false): re-invoke every live row's
   callback standalone inside a minimal replay scope and patch its reserved region in place.
   Rows whose window has no live slot, or whose slot was re-tessellated without them (generation
   mismatch), or that are clip-hidden, are skipped silently -- they are not on screen, and the
   real frame that changes that also recaptures them.  A patch FAILURE (reservation overflow)
   retires the row, invalidates the window, and asks for one real frame via
   replay_scope_exit's force_redraw so nothing stays visibly stale. */
void
volatile_update( void )
{
    if ( s_draw.seg_count == 0 )
        return;   /* no frame has ever emitted -- nothing to patch, and no segment to checkpoint */

    /* Restored once after the loop: the per-row replays land per-viewport DPI bakes and install
       stamped fonts, and the active font must leave this function as it entered (the landing
       machinery's lineage guard reads it). */
    u32 active_font_ck = font_active_id();
    u32 draw_font_ck   = s_draw.cur_font;

    for ( u32 i = 0; i < s_volatile_count; ++i )
    {
        gui_volatile_slot_t* row = &s_volatile[ i ];
        if ( !row->active || row->hidden || !row->fn || !row->stamped )
            continue;   /* !stamped: the callback never ran gui_volatile_begin, so there is no
                           cursor/scope stamp to replay under -- the block simply never animates */

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
        u32        tedge_ck      = s_draw.text_edge;

        /* Mixed DPI: land the bake of the viewport this row's window sits on, exactly as
           window_begin does before the real emit (gui_frame_dpi.c).  The idle frame's ambient
           metrics are viewport 0's (frame_begin lands them), so without this a floater on a
           differently-scaled monitor replays its callback against the wrong s_style metrics --
           the stamped font gives the right glyph sizes but every em-scaled layout metric (gaps,
           widget heights, the grid quantum) is the primary surface's, the measured footprint
           disagrees with every real emit, and the reflow strike counter runs out.  A no-op when
           the sizes already match (the single-monitor common case). */
        gui_dpi_land( (i32)row->vp );

        s_draw.cur_win      = row->win;
        s_draw.cur_z        = row->z;
        s_draw.cur_vp       = row->vp;
        s_draw.cur_font     = row->font;
        s_draw.cur_clip_idx = row->clip_idx;
        font_use( row->font );

        /* Same reasoning as the clip-stack force below: alpha/rounding/text-clip/text-edge are
           ambient scalars a raw draw_ call reads directly, not part of the minimal scope
           replay_scope_enter reconstructs, so without this the callback would draw with
           whatever an unrelated idle frame's leftover s_draw state happens to be. */
        s_draw.alpha        = row->alpha;
        s_draw.rounding     = row->rounding;
        s_draw.text_clip_x0 = row->text_clip_x0;
        s_draw.text_clip_x1 = row->text_clip_x1;
        s_draw.text_edge    = row->text_edge;

        /* draw_cull_box (gui_emit_draw.c) tests against clip_stack[clip_depth-1], NOT
           cur_clip_idx -- cur_clip_idx alone is not enough to reproduce the real-emit clip. Force
           a one-deep stack whose top is the captured clip's resolved rect, or a callback whose
           rect happens to sit outside whatever clip was left on the stack by the last real emit
           gets silently culled during replay. */
        s_draw.clip_stack    [ 0 ] = s_draw.clip_table[ row->clip_idx ];
        s_draw.clip_idx_stack[ 0 ] = row->clip_idx;
        s_draw.clip_depth          = 1;

        replay_scope_enter( row->id, row->x, row->y, row->w, &row->view, row->pad );
        row->fn( row->id, true );
        u32 cmd_hi = s_draw.cmd_count;

        /* Measure before the scope pops -- the layout half of "does this still fit what the cache
           holds for it", checked whether or not the geometry patch itself succeeds. */
        f32 foot_w, foot_h;
        replay_scope_measure( &foot_w, &foot_h );

        bool ok     = volatile_patch( row, cmd_ck, cmd_hi );
        bool reflow = volatile_footprint_reflow( row, foot_w, foot_h );

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

        /* Either failure mode costs exactly one real frame: geometry that outgrew its reservation
           recaptures bigger, layout that outgrew its cell re-lays-out the neighbours around it. */
        replay_scope_exit( !ok || reflow );

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
        s_draw.text_edge            = tedge_ck;

        /* The active font stays this row's stamped font until the tail below: re-activating an
           unrelated font here would trip the DPI landing's lineage guard for the next row. */
    }

    /* Re-land the primary surface's bake (the ambient state everything between frames reads --
       a no-op unless a row above landed a floater's), then restore the entry font state. */
    gui_dpi_land( 0 );
    font_use( active_font_ck );
    s_draw.cur_font = draw_font_ck;
}

// clang-format on
/*============================================================================================*/
