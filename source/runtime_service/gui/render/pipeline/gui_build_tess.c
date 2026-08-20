/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess.c -- CPU-side quad-record builder.

    Translates the frame's semantic gui_cmd_t list (s_draw) into quad records in s_tess:
    ONE 16-byte gui_quad_t per shape (gui.h), expanded by SV_VertexID in gui_quad.vs.hlsl --
    there is no vertex buffer and no index buffer.  Everything here reads semantic commands
    and writes quad + style records; nothing here touches the GPU API.

    s_tess is read only by the two files included after: gui_build_cache.c (the BUILD phase
    fills it via tess_dispatch) and gui_render_submit.c (gui_render_flush uploads it and emits
    draw calls).  No file above the backend unit touches it.

    Included by gui_render.c after gui_emit_path.c (provides v2, seg_normal,
    stroke_center_offset, STROKE_* constants) and before gui_build_cache.c (which drives
    tess_reset / tess_dispatch from cache_build_frame / cache_tess_window).

==============================================================================================*/
// clang-format off

/* One GPU command plus its placement -- the AOS command record.  Every consumer (the merge check,
   the flush loop, the volatile copy-back, the dashboard capture) reads a WHOLE command at a given
   index; none sweeps a single field across all commands, so the fields that belong to one command
   live together in one cache line rather than in parallel arrays keyed on the same index.
   qbase is explicit (not accumulated from elem_counts at flush) so the quad arena may hold
   reserved gaps -- volatile block headroom -- between commands.  Mirrors dash_cmd_t (the snapshot
   type in gui_render.h), which was AOS from the start; this is the live half catching up. */

typedef struct
{
    gui_gpu_cmd_t cmd;      // elem_count (quads), tex_idx -- the GPU draw-call unit
    i16           vp;       // viewport for this command (GUI_VP_INVALID = dormant volatile pad)
    u16           qbase;    // quad slot -- first quad of command (its draw's first_vertex / 6)

} tess_gpu_cmd_t;

ORB_STATIC_ASSERT( GUI_MAX_QUADS <= 0xFFFF, "tess_gpu_cmd_t.qbase is u16" );

/*==============================================================================================
    Tessellation state -- the quad and style arenas populated from the semantic command list.

    cache_build_frame tessellates the frame's gui_cmd_t list into s_tess (per window, via
    tess_dispatch), then gui_render_flush uploads s_tess.quads/prims to the GPU.  s_tess is
    backend-private; nothing above the backend unit touches it.  s_draw holds only semantic
    commands (gui_cmd_t) -- no geometry.

    cur_clip/cur_vp are written by tess_dispatch before each primitive and ARE the batch key, which
    is why tess_ensure_gpu_cmd takes no parameters -- it reads them from here.  cur_clip is
    resolved from s_draw.clip_table[c->clip_idx]; z is per-segment and is not tracked here.
==============================================================================================*/

static struct
{
    gui_quad_t      quads    [ GUI_MAX_QUADS ];    // quad records -- the geometry arena (gui.h)
    gui_prim_t      prims    [ GUI_MAX_PRIMS ];    // style records -- the second arena
    tess_gpu_cmd_t  gpu_cmds [ GUI_MAX_CMDS  ];    // gpu draw commands (AOS: cmd + vp/qbase)

    /* Write head cursors.  A QUAD RECORD is the geometry element everywhere past this file --
       the cache's slot spans, the volatile reservations, the dirty spans and the flush all count
       in quads, and a draw multiplies by six only at the cmd_draw itself. */
    u32 quad_count, prim_count, cmd_count;

    gui_rect_t  cur_clip;   /* clip resolved from s_draw.clip_table[c->clip_idx] for each command */
    u32         cur_clip_local; /* the same clip as the slot's LOCAL entry index (0..GUI_WIN_CLIP_MAX)
                                   -- what every quad's clip field carries.  The window's slab base
                                   is added at the flush, through pc.clip_base                     */
    i32         cur_vp;     /* viewport baked from the current semantic command                    */
    u32         cur_tex;    /* GUI_TEX_MODE | bindless slot the style record carries -- set by
                               tess_set_tex, folded by tess_quad_push.  NOT a batch key           */
    u32         cur_ops;    /* GUI_OP_* -- the per-primitive modifiers (gui.h).  Cleared per
                               semantic command alongside the record: leaking a self flag would
                               blank a textured quad, and leaking an op would reshape the next
                               fill.  tess_quad_push folds these into the style record. */
    f32         cur_corner_pow; /* corner profile exponent for the box family, ambient over one
                               command for the same reason cur_ops is: it reaches all four corners
                               of a shape without threading a parameter through every fill in the
                               library, and cannot leak onto the next command.  0 = circular arcs */

    /* GLYPH ATTRIBUTION -- ambient over one glyph run, read by tess_quad_push.  Only the two text
       tessellators (tess_text_n, tess_text_xf) raise the flag, and every quad they push while it
       is up is a character.  slot_text_* accumulate over ONE slot's tessellation:
       cache_slot_tessellate zeroes them before cache_tess_window and stores the result on the
       slot, so the per-frame total survives geometry caching (a retained window never re-enters
       this path).  Volatile patches are excluded -- they rewrite bytes inside a slot whose count
       was already taken, so counting them again would double-bill the row. */
    bool        cur_is_text;
    u32         slot_text_quads;   /* glyph quads pushed into the slot under tessellation */
    u32         slot_text_runs;    /* glyph runs that produced them (glyphs/run amortisation) */

    /* The ambient STYLE RECORD -- filled by whichever tess_* emitter is running and appended
       (deduplicated) by tess_quad_push via tess_prim_local.  Only the fields the ambient FIELD
       actually reads are written; the rest stay zero, which is what lets consecutive flat fills
       collapse onto one record (tess_prim_local).  Cleared per semantic command.

       cur_prim_local is the style index the last commit resolved to: slot-local into the arena, or
       -- when the record was one of the theme's own and the palette already holds it -- an absolute
       palette index past GUI_PAL_FIRST that no slot base applies to (gui.h).
       prim_dedup_floor is the lowest record index the memo may reach BACK to: every record in
       [max(floor, slot_prim_base), prim_count) was written by this pass and is a live dedup
       candidate.  The scan looks at the last few of those (TESS_PRIM_MEMO_DEPTH), which is what
       collapses the A-B-A interleave ordinary chrome emits -- a flat-fill record, a widget's box
       record, the flat record again -- where a last-record-only memo re-appended A per widget.
       The floor rises wherever the records behind it stop being reusable: a new slot (previous
       slot's indices are relative to its own base), a volatile block's boundary in either
       direction (its records are a patch-rewritable reservation), and a patch's scratch. */

    gui_prim_t  cur_prim;
    u32         cur_prim_local;
    u32         prim_dedup_floor;

    /* The open FX PAGE -- a style-arena record carrying four gui_fx_t records (gui.h).  fx_page is
       the record's slot-local index and fx_page_used how many of its four entries are spent; a
       fifth takes a new page.  fx_memo_row is the last row committed, so a run of quads wanting
       the same turn / phase / border shares one record -- the common case, since those lanes
       are ambient over a command.  Both reset wherever prim_dedup_floor rises (a new slot, a
       volatile boundary, a patch's scratch): a page below the floor belongs to a reservation this
       pass does not own, and writing another row into it would corrupt it.
       0 = no page open / no memo, which cannot collide with a real row (gui.h). */
    u32         fx_page, fx_page_used, fx_memo_row;

    /* Pages allocated this build, counted apart from prim_count so the arena's fill can be read as
       the two things it actually holds: STYLES (one per distinct look, deduped hard) and FX PAGES
       (four instance records each, driven by turns / phases / uv rects, which dedup far less).
       They compete for the same GUI_MAX_PRIMS entries, and they fill for opposite reasons, so a
       single "records" number cannot say which one to go after. */
    u32         fx_page_count;

    /* The ambient PER-INSTANCE lanes -- all three ride the QUAD, never the style, so an animated
       border, a turning shape and a staggered pulse never add style records: cur_prim states only
       what is shared (shape, widths, rates).  Each is an ambient the command sets before its
       tess_quad_push, which folds it into the quad unread by the style.  Cleared per command.
         cur_col_border  GUI_OP_FRAME's border band colour (0 = unused)
         cur_rot_c       the turn, as a unit pair; (1, 0) is the identity a plain shape leaves
         cur_phase       animation phase in cycles (0 = in step with the clock) */
    u32         cur_col_border;
    f32         cur_rot_c, cur_rot_s;
    f32         cur_phase;

    /* per-slot tesellation context */

    /* Quad base of the window slot currently being tessellated -- the origin volatile blocks
       measure their slot-relative positions against. */
    u32 slot_quad_base;

    /* Command base and tessellation generation of the window slot currently being tessellated --
       set alongside slot_quad_base by cache_build_frame's retess path.  Volatile blocks record
       their position slot-RELATIVE (never absolute) so a later patch can resolve the current
       absolute position from the live slot table, and stamp slot_tess_gen so a patch only ever
       writes into geometry produced by the exact tessellation pass that captured it
       (see render/pipeline/gui_build_volatile.c). */

    u32 slot_cmd_base;
    u32 slot_tess_gen;

    /* Style-record base of the window slot being tessellated -- the counterpart of
       slot_quad_base for the style arena.  Quads bake SLOT-LOCAL style indices
       (prim_count - slot_prim_base) and the flush adds this base back through pc.prim_base, so
       a repack that moves the slot's records leaves every cached quad's index correct. */
    u32 slot_prim_base;

    /* The slot's LOCAL clip table, written while its commands tessellate: first-seen distinct
       clip rects, appended by tess_clip_local and stored on the slot record itself so cache-hit
       frames replay the exact rects the baked clip entries mean.  slot_clips/slot_clip_count
       point into the win_geo_slot_t being tessellated (set alongside slot_quad_base); NULL
       between slots.  Entry indices are SLOT-LOCAL: the window's slab origin (its stable cache
       slot * GUI_WIN_CLIP_MAX) is added at the flush through pc.clip_base, so the four bits the
       quad spends on a clip are all it ever needs.  slot_clip_pending is the slot's upload mask (one bit per
       (frame-in-flight, viewport) region); an append marks all bits so the flush re-uploads the
       slab.  clip_memo_ci memoizes the common run of consecutive same-clip commands (0xFF =
       empty). */

    gui_clip_entry_t* slot_clips;
    u32*              slot_clip_count;
    u8*               slot_clip_pending;
    u8                clip_memo_ci;
    u32               clip_memo_local;

    /* Set before each cache_tess_window call so tess_ensure_gpu_cmd always opens a fresh
       command for the first primitive of a new slot, even when the previous slot's last
       command shares the same clip/vp (same-position windows would otherwise merge
       across the slot boundary and corrupt elem_count + first_index tracking). */

    bool force_new_cmd;
    u32  overflow;          /* TESS_OVF_* -- which walls this build hit; 0 = none.  Set per-primitive
                               at the reservation site, which then drops its geometry */

} s_tess;

/* The walls a build can hit, one bit each, so a report names the cap that actually spilled instead
   of the vertex/index pair the pre-quad backend had.  They are genuinely different problems:
   QUADS and CMDS scale with how much is on screen, PRIMS with how many distinct STYLES are,
   WINDOWS with how many are open, and FX_FIELD is not a pool at all -- it is the ceiling on how
   far into a window's own style arena a quad's fx field can reach.

   Every bit here means DROPPED CONTENT: something the frame asked for is not on screen.  The
   retained cache's per-window command cap (WIN_SLOT_CMD_MAX) is deliberately NOT one of them --
   overrunning it only makes a window uncacheable, and it still draws in full. */

#define TESS_OVF_QUADS      ( 1u << 0 )   /* quad arena full              -- GUI_MAX_QUADS    */
#define TESS_OVF_CMDS       ( 1u << 1 )   /* gpu command table full       -- GUI_MAX_CMDS     */
#define TESS_OVF_PRIMS      ( 1u << 2 )   /* style record arena full      -- GUI_MAX_PRIMS    */
#define TESS_OVF_FX_FIELD   ( 1u << 3 )   /* fx row past the quad's field -- GUI_QUAD_FX_MASK */
#define TESS_OVF_WIN_CLIPS  ( 1u << 4 )   /* window's clip slab full      -- GUI_WIN_CLIP_MAX */
#define TESS_OVF_WINDOWS    ( 1u << 5 )   /* windows this frame           -- RENDER_MAX_WIN   */

/* True while volatile_patch re-tessellates a row into scratch (gui_build_volatile.c, included
   after this file in the gui_render.c unity build).  Declared here because two things above the
   volatile seam read it: the range tracking, which must not mistake a patch for a fresh capture,
   and tess_quad_push's glyph counter, whose slot totals were already taken. */
static bool s_volatile_patching;

/* The geometry element copy: the cache's in-place relocation and the volatile patch both move
   quad spans by element index. */
static inline void
tess_geo_copy( u32 dst, u32 src, u32 count )
{
    memcpy( &s_tess.quads[ dst ], &s_tess.quads[ src ], count * sizeof( gui_quad_t ) );
}

/*==============================================================================================
    Tessellation diagnostics -- the cold companion to s_tess.

    High-water marks, the sticky overflow mask, and the arena band boundary the dashboard reads.
    Every field here is written at most once per slot placement / band boundary / frame end --
    never on the per-quad path -- so it lives apart from s_tess to keep the hot write cacheline
    (quad_count / cmd_count) small.  Read only by the dashboard capture and the render
    overlay; s_tess.overflow stays where it is because it is written per-primitive at the wall.
==============================================================================================*/

/* Geometry generation -- bumped ONLY when the whole arena layout moves: the repack retry
   (cache_build_frame), where every slot relocates at once.  Each (frame-in-flight, viewport)
   upload region remembers the generation it was last filled with (gui_render_submit.c); a
   mismatch forces the full span upload.  Everything finer goes through the dirty spans below:
   a single window's retess or a volatile patch touches only its own slot's bytes, so it unions
   a span instead.  Pure command-side changes (z reorder, a window vanishing) record nothing at
   all: draws re-record every flush and never reference bytes outside their own slots' spans. */
static u32 s_geo_gen = 1;

/* Fine dirty spans -- the per-change companion to s_geo_gen.  A window's in-place retess
   (cache_slot_tessellate) and a volatile patch (volatile_patch) each rewrite bytes inside one
   slot; forcing the full span for that would re-upload the whole arena every presented frame
   anything changes (and a stats overlay observing uploads would cause them).  Instead the
   writer unions its rewritten vertex/index ranges into every in-flight region of the window's
   viewport, and a generation-matching flush uploads just its region's accumulated spans and
   clears them (gui_render_submit.c).  A generation-stale flush's full upload covers every
   accumulated byte of its surface, so it clears the entry too. */
/* Kept per ARENA BAND as well as per region: debug-band slots pack at the arena tail, so a
   single union would bridge from a changed app window to a changed overlay and drag the whole
   arena between them.  Separate spans keep the two-band isolation contract intact -- and let
   the flush attribute band-1 upload bytes to the overlay in the stats it displays. */
static struct
{
    u32 v_lo, v_hi;   // pending quad range, arena-absolute (empty when v_lo >= v_hi)

} s_patch_pending[ RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS ][ 2 ];

static void
patch_range_union( u32* lo, u32* hi, u32 nlo, u32 nhi )
{
    if ( nlo >= nhi )
        return;
    if ( *lo >= *hi ) { *lo = nlo; *hi = nhi; return; }
    if ( nlo < *lo )  *lo = nlo;
    if ( nhi > *hi )  *hi = nhi;
}

static void
patch_span_union( u8 vp, u8 band, u32 v_lo, u32 v_hi )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return;
    u32 b = band != 0 ? 1u : 0u;
    for ( u32 f = 0; f < RHI_MAX_FRAMES_IN_FLIGHT; ++f )
    {
        u32 r = f * GUI_MAX_VIEWPORTS + vp;
        patch_range_union( &s_patch_pending[ r ][ b ].v_lo, &s_patch_pending[ r ][ b ].v_hi,
                           v_lo, v_hi );
    }
}

static struct
{
    u32  quad_hwm, prim_hwm, cmd_hwm;   /* lifetime peak of the TOTAL write head (both bands) */
    u32  win_hwm;                       /* lifetime peak of windows tracked in one frame      */
    u32  overflow_walls;                /* sticky union of the TESS_OVF_* bits hit this run   */

    /* Arena band boundary: the write head right after the last MAIN-band slot placed this frame
       (band-major placement packs every debug-band slot after it).  The dashboard's memory map
       reads this as "main arena ends here"; the span up to quad_count past it is the debug UI's
       own attributed footprint.  Re-derived by cache_build_frame every build. */
    u32 band0_quad_end;

    /* Lifetime peak of the MAIN-band (band 0) write head alone -- the real application's geometry
       ceiling, tracked apart from quad_hwm so the dashboard can show actual use limits with the
       self-measuring debug band filtered out.  Peaks on a different frame than quad_hwm, so it is a
       separate accumulator, not a subtraction. */
    u32 band0_quad_hwm;

    /* GLYPH SHARE of the live arena, re-derived each build by summing the placed slots' stored
       counts (cache_place_slots).  Summed rather than counted at the push, because geometry is
       cached per window: a per-push counter would only ever see the windows that retessellated
       this frame, which on a steady UI is none of them.

       text_quads is how much of the quad arena is characters; text_runs the number of glyph runs
       they came from, so text_quads / text_runs is the glyphs-per-run a per-run record would
       amortise over.  Split by band for the same reason the vert counts are: the dashboard that
       displays this is itself almost entirely text, and would otherwise measure mostly itself. */
    u32 text_quads,       text_runs;         /* both bands */
    u32 band0_text_quads, band0_text_runs;   /* main band alone */

    /* LIVE QUADS: the sum of the placed slots' quad_count -- what actually draws, and the only
       correct denominator for the glyph share.  Distinct from the write heads above, which measure
       ARENA OCCUPANCY: every slot reserves quad_alloc = quad_count + max(quad_count/4, SLOT_QUAD_PAD),
       so a UI made of several small windows carries 64+ quads of padding per slot, plus whatever gap
       a repack has not yet reclaimed.  On a near-empty frame the head can be triple the live count. */
    u32 live_quads, band0_live_quads;

    /* Windows that overran the retained cache's per-window command run (WIN_SLOT_CMD_MAX) and so
       re-tessellate every real frame.  Not an overflow wall -- nothing is dropped, the window just
       stops being cacheable -- but it is the difference between a UI that idles for free and one
       that pays a full rebuild per frame, so it is worth a line at shutdown. */
    u32 uncacheable_wins;

} s_tess_stats;

/* Spell a TESS_OVF_* mask as a comma-separated list of the caps that were hit, into `buf`.
   Every diagnostic that mentions an overflow goes through this, so a report always names the pool
   that actually spilled and the #define to raise for it.  "" for a zero mask. */
static const char*
tess_overflow_walls( u32 mask, char* buf, u32 cap )
{
    static const struct { u32 bit; const char* name; } walls[] = {
        { TESS_OVF_QUADS,     "GUI_MAX_QUADS"    },
        { TESS_OVF_CMDS,      "GUI_MAX_CMDS"     },
        { TESS_OVF_PRIMS,     "GUI_MAX_PRIMS"    },
        { TESS_OVF_FX_FIELD,  "fx field width"   },
        { TESS_OVF_WIN_CLIPS, "GUI_WIN_CLIP_MAX" },
        { TESS_OVF_WINDOWS,   "RENDER_MAX_WIN"   },
    };
    u32 n = 0;
    buf[ 0 ] = '\0';
    for ( u32 i = 0; i < sizeof( walls ) / sizeof( walls[ 0 ] ); ++i )
    {
        if ( !( mask & walls[ i ].bit ) )
            continue;
        n += (u32)fmt_snprintf( buf + n, cap - n, n ? ", %s" : "%s", walls[ i ].name );
        if ( n >= cap - 1u )
            break;
    }
    return buf;
}

/*==============================================================================================
    Quantizers -- the two grids geometry lands on.

    Every axis-aligned fill snaps its ORIGIN to the pixel grid so its edges stay crisp; a shape
    with no straight edge (a disc, a rotated box) deliberately does not, since quantizing its
    centre makes an animated dot stutter.  A pattern's cell rides a quarter-pixel grid instead:
    fine enough that a scaled lattice does not visibly step, coarse enough that the fragment's
    packed cell field can carry it.
==============================================================================================*/

/* Round to the nearest whole pixel -- the snap every straight-edged primitive puts its origin
   through.  Named so a call site says WHY it rounds, not merely that it does. */
static f32
tess_snap_px( f32 v )
{
    return floorf( v + 0.5f );
}

/* A pattern cell floored at one pixel.  The quarter-pixel quantize and the upper bound that used
   to live here were the packed cell field's; the record carries an exact float.  The floor stays,
   and it is not about storage: a sub-pixel lattice is aliasing, not a pattern. */
static f32
tess_clamp_cell( f32 cell )
{
    return ( cell < 1.0f ) ? 1.0f : cell;
}

/*==============================================================================================
    Tessellation helpers -- mirrors of the draw_push_* functions in the gui_emit_* files, but writing
    into s_tess instead of s_draw.  These are the backend half of the command-list split.
    Called from tess_dispatch; not called from anywhere else.
==============================================================================================*/

static void
tess_reset( void )
{
    s_tess.quad_count      = 0;
    s_tess.prim_count      = 0;
    s_tess.cmd_count       = 0;
    s_tess.slot_quad_base  = 0;
    s_tess.slot_cmd_base   = 0;
    s_tess.slot_prim_base  = 0;
    s_tess.slot_tess_gen   = 0;
    s_tess.cur_prim_local  = 0;
    s_tess.prim_dedup_floor = 0;
    s_tess.fx_page = s_tess.fx_page_used = s_tess.fx_memo_row = 0;
    s_tess.fx_page_count     = 0;
    s_tess.slot_clips        = NULL;
    s_tess.slot_clip_count   = NULL;
    s_tess.slot_clip_pending = NULL;
    s_tess.clip_memo_ci      = 0xFF;
    s_tess.cur_clip_local    = 0;
    s_tess.cur_is_text       = false;
    s_tess.slot_text_quads   = 0;
    s_tess.slot_text_runs    = 0;
    s_tess.force_new_cmd     = false;
    s_tess.overflow          = 0u;
    s_tess.cur_col_border    = 0;
    s_tess.cur_rot_c         = 1.0f;
    s_tess.cur_rot_s         = 0.0f;
    s_tess.cur_phase         = 0.0f;
}

/* Name the texture the next quad's style will CARRY (tess_quad_push folds it into the record).
   Deliberately NOT part of opening a batch, and separated from it so that reads: the texture
   rides the style record, so a texture change costs nothing and must not open a command. */
static void
tess_set_tex( u32 tex_idx )
{
    s_tess.cur_tex = tex_idx;
}

/* Resolve the ambient clip to an ABSOLUTE entry index in the frame clip region: the window's
   fixed slab base plus its position in the slot's LOCAL clip table, appending a new entry at
   first sight.  Content-keyed -- (rect, radius) is the whole identity -- and first-seen ordered
   over the window's own commands, so a window whose commands hash identical reproduces identical
   indices: the property that lets a cached quad bake its clip entry.  The slab base
   is keyed by the window's id-keyed stable cache slot, so the absolute index survives as long as
   the window does.  An append marks the slot's upload mask -- the flush re-uploads a slab only
   when its content changed.  The memo serves the common run of consecutive same-clip commands.
   A slot past GUI_WIN_CLIP_MAX distinct clips falls back to its slab's entry 0 -- degrading INSIDE
   the window (its own first clip, usually the window rect) rather than borrowing a neighbour's
   slab.  Reported through the same overflow path as the arenas, because that many distinct clips
   in one window is a bug, not a budget. */
static u32
tess_clip_local( u8 ci )
{
    if ( s_tess.clip_memo_ci == ci )
        return s_tess.clip_memo_local;
    s_tess.clip_memo_ci = ci;

    if ( !s_tess.slot_clips )
        return s_tess.clip_memo_local = 0u;

    const gui_rect_t* r   = &s_draw.clip_table[ ci ];
    f32               rad = s_draw.clip_radius[ ci ];
    u32               n   = *s_tess.slot_clip_count;
    for ( u32 g = 0; g < n; ++g )
    {
        const gui_clip_entry_t* e = &s_tess.slot_clips[ g ];
        if ( e->rect.x == r->x && e->rect.y == r->y && e->rect.w == r->w && e->rect.h == r->h
          && e->radius == rad )
            return s_tess.clip_memo_local = g;
    }
    if ( n >= GUI_WIN_CLIP_MAX )
    {
        s_tess.overflow |= TESS_OVF_WIN_CLIPS;
        return s_tess.clip_memo_local = 0u;
    }
    s_tess.slot_clips[ n ] = ( gui_clip_entry_t ){ .rect = *r, .radius = rad };
    *s_tess.slot_clip_count = n + 1;
    if ( s_tess.slot_clip_pending )
        *s_tess.slot_clip_pending = 0xFF;
    return s_tess.clip_memo_local = n;
}

/* Ensure a GPU command is open whose viewport matches the ambient one, opening a new one at a
   mismatch.  THE VIEWPORT IS THE WHOLE BATCH KEY, which is why this takes no arguments: the
   texture, the style and the clip all travel per quad and cannot cut a draw call, and z is
   per-segment rather than per-command (the segment system already guarantees every command in
   one window's tessellation pass shares a z).  A new primitive type therefore batches correctly
   by construction -- there is nothing left to pass in and get wrong.
   Returns false when the command table is full and no matching command is open -- the caller must
   drop its primitive, or its geometry would append to a command with the wrong viewport. */

static bool
tess_ensure_gpu_cmd( void )
{
    if ( s_tess.cmd_count > 0 && !s_tess.force_new_cmd )
    {
        const tess_gpu_cmd_t* prev = &s_tess.gpu_cmds[ s_tess.cmd_count - 1 ];
        if ( prev->vp == s_tess.cur_vp )
            return true;
    }
    if ( s_tess.cmd_count >= GUI_MAX_CMDS )
    {
        s_tess.overflow |= TESS_OVF_CMDS;
        return false;
    }
    s_tess.force_new_cmd = false;
    /* Quad span of this command starts at the current quad_count; the next command's qbase (or
       the final quad_count for the last) bounds it.  Lets a surface upload only its own quads.
       tex_idx is the ambient value at the moment the command opened, i.e. the FIRST primitive's,
       and is diagnostic only (the dashboard tooltip) -- it rides the quad now and the command
       may go on to span several. */
    s_tess.gpu_cmds[ s_tess.cmd_count++ ] = ( tess_gpu_cmd_t ){
        .cmd   = { .elem_count = 0, .tex_idx = s_tess.cur_tex },
        .vp    = (i16)s_tess.cur_vp,
        .qbase = (u16)s_tess.quad_count,
    };
    return true;
}

/* Resolve the ambient primitive record to a SLOT-LOCAL index in the frame's record arena,
   appending a new entry when the ambient state has moved.  The counterpart of tess_clip_local, and
   deliberately the same shape -- but keyed on CONTENT rather than on a table index, because the
   record is assembled from half a dozen ambient fields and nothing upstream has a name for the
   combination.

   The memo is the whole performance story.  A glyph run is one semantic command emitting hundreds
   of quads under one unchanging record, and a run of flat fills sharing a texture and a clip is
   the same: comparing against the last appended record collapses each to ONE entry.  That only
   works because emitters leave the fields their field does not read at zero (gui.h) -- a writer
   that stamped its rect into a GUI_FX_NONE record would give every fill an entry of its own.

   Past the arena a slot degrades to its own first record, mirroring tess_clip_local's fallback to
   slab entry 0: a wrong shape is bad, but a wild index into a storage buffer is worse.  The
   TESS_OVF_PRIMS flag is what actually reports it. */

/* How far back the dedup scan reaches.  1 collapses a homogeneous run (a glyph run, consecutive
   flat fills); the extra depth collapses the ALTERNATION chrome actually emits -- text, a rounded
   widget's own record, text again -- which a 1-deep memo re-appended on every return.  Four
   128-byte compares against L1-hot records is noise next to the tessellation around it, and a HIT
   costs one compare at any depth -- only a miss pays the full scan.

   4 sits one step past a sharp knee, and everything above it is flat.  Measured on sb_gui's main
   window with the debug overlay up: depth 3 -> 35 records, 4 -> 25, 6 -> 23, 8 -> 21.  Past 4 a
   step buys about two records, so there is nothing to chase by going deeper.

   The residual will not yield to depth at all: dedup is slot-scoped, so each retained-cache window
   slot holds its own copy of an identical style and no walk can reach across that boundary.  A
   frame-global intern region is the lever for those, not this number. */

#define TESS_PRIM_MEMO_DEPTH  4u

/* Drop the open fx page and its memo.  Called wherever prim_dedup_floor rises: past that line the
   page belongs to another slot or to a reservation only a patch may rewrite, and appending a ninth
   row into it would write bytes this pass does not own. */
static inline void
tess_fx_page_reset( void )
{
    s_tess.fx_page = s_tess.fx_page_used = s_tess.fx_memo_row = 0;
}

static u32
tess_prim_local( void )
{
    /* Census before the memo, so the count is of quads that WANT this record rather than of the
       ones the memo happened to miss; the append site below counts the arena entries. */
    PRIM_CENSUS_QUAD( &s_tess.cur_prim );

    u32 hi = s_tess.prim_count;
    u32 lo = ( s_tess.prim_dedup_floor > s_tess.slot_prim_base )
             ? s_tess.prim_dedup_floor : s_tess.slot_prim_base;
    u32 n  = ( hi > lo ) ? hi - lo : 0u;
    if ( n > TESS_PRIM_MEMO_DEPTH ) n = TESS_PRIM_MEMO_DEPTH;

    /* Depth 1 first, alone: a homogeneous run -- a glyph run, consecutive flat fills -- hits here
       and never pays for the lookup below.  That is most quads. */
    if ( n >= 1u && memcmp( &s_tess.prims[ hi - 1u ], &s_tess.cur_prim,
                            sizeof( gui_prim_t ) ) == 0 )
        return s_tess.cur_prim_local = ( hi - 1u ) - s_tess.slot_prim_base;

    /* Then the PALETTE, ahead of the deeper memo walk.  A hit costs this slot nothing at all --
       no arena entry, and the same entry serves every other window drawing the same shape, which
       is the duplication no memo depth can reach (see TESS_PRIM_MEMO_DEPTH above).  A hit returns
       an ABSOLUTE index the flush resolves against pc.pal_base rather than a slot-local one; both
       ride the same field and the shader tells them apart by range (gui.h, GUI_PAL_FIRST).
       pal_find carries its own one-deep memo, which is what covers the repeat this arena memo
       structurally cannot: a palette hit appends nothing, so depth 1 above never sees it. */
    u32 entry = pal_find( &s_tess.cur_prim );
    if ( entry < (u32)GUI_PAL_MAX )
        return s_tess.cur_prim_local = gui_style_pal( entry );

    for ( u32 k = 2; k <= n; ++k ) {
        if ( memcmp( &s_tess.prims[ hi - k ], &s_tess.cur_prim, sizeof( gui_prim_t )) == 0 ) {
            return s_tess.cur_prim_local = ( hi - k ) - s_tess.slot_prim_base;
        }
    }

    if ( hi >= GUI_MAX_PRIMS )
    {
        s_tess.overflow |= TESS_OVF_PRIMS;
        s_tess.prim_dedup_floor = hi;
        tess_fx_page_reset();
        return s_tess.cur_prim_local = 0u;
    }

    PRIM_CENSUS_APPEND( &s_tess.cur_prim );

    s_tess.prims[ hi ] = s_tess.cur_prim;
    s_tess.cur_prim_local = hi - s_tess.slot_prim_base;
    s_tess.prim_count++;
    return s_tess.cur_prim_local;
}

/* Resolve the ambient turn / phase / border colour plus this quad's texture rect to an fx record,
   returning its SLOT-LOCAL row index in the style arena (0 = the quad needs none, which the shader
   reads as identity turn, zero phase, no border and no texture rect).

   Records pack four to a style-arena slot, so a page costs one gui_prim_t and serves four
   instances.  The memo is one deep and that is enough: the instance lanes are ambient over a
   semantic command, so the quads that share them arrive consecutively -- a framed row, a polyline
   of one direction, a set of glyph quads wanting nothing at all.  A uv rect breaks the memo by
   nature (consecutive icons sample different cells), which is the honest cost of the sprite path
   and still one QUARTER of a record each. */

#define TESS_FX_PER_PAGE   ( GUI_PRIM_ROWS / GUI_FX_ROWS )   /* a style record is four fx records */

static u32
tess_fx_local( u32 uv0, u32 uv1 )
{
    gui_fx_t fx = {
        .xform      = gui_xform_pack( s_tess.cur_rot_c, s_tess.cur_rot_s ),
        .phase      = gui_phase_pack( s_tess.cur_phase ),
        .col_border = s_tess.cur_col_border,
        .uv0        = uv0,
        .uv1        = uv1,
    };
    if ( fx.xform == 0u && fx.phase == 0u && fx.col_border == 0u
      && fx.uv0 == 0u && fx.uv1 == 0u )
        return 0u;      /* the whole record is the default -- the majority of quads, text included */

    /* The page is a style-arena record read as rows; the rows are addressed as bytes rather than
       through a second struct pointer, so the two record types never alias one another. */
    u8* page_p = (u8*)&s_tess.prims[ s_tess.slot_prim_base + s_tess.fx_page ];
    if ( s_tess.fx_memo_row
      && memcmp( page_p + ( s_tess.fx_page_used - 1u ) * GUI_FX_BYTES, &fx, sizeof fx ) == 0 )
        return s_tess.fx_memo_row;

    if ( s_tess.fx_page == 0u || s_tess.fx_page_used >= TESS_FX_PER_PAGE )
    {
        /* A fresh page. The floor rises past it for the same reason a volatile boundary raises it:
           the record now holds fx rows, and a style comparing equal to those bytes would be handed
           an index into them. */
        if ( s_tess.prim_count >= GUI_MAX_PRIMS )
        {
            s_tess.overflow |= TESS_OVF_PRIMS;
            return 0u;
        }

        /* Record 0 of a slot may never be an fx page -- row 0 is the index the quad spends on
           "no record".  Two kinds of quad reach here with the slot still empty: a GLYPH, which
           resolves no style at all, and any shape whose style the PALETTE answered, which claims
           no arena record either.  Leave record 0 unwritten in both cases. */
        if ( s_tess.prim_count == s_tess.slot_prim_base )
        {
            if ( s_tess.prim_count + 1u >= GUI_MAX_PRIMS )
            {
                s_tess.overflow |= TESS_OVF_PRIMS;
                return 0u;
            }
            memset( &s_tess.prims[ s_tess.prim_count++ ], 0, sizeof( gui_prim_t ) );
        }

        u32 page = s_tess.prim_count - s_tess.slot_prim_base;
        if ( page * GUI_PRIM_ROWS + ( TESS_FX_PER_PAGE - 1u ) * GUI_FX_ROWS > GUI_QUAD_FX_MASK )
        {
            s_tess.overflow |= TESS_OVF_FX_FIELD;   /* past what the quad's fx field can name */
            return 0u;
        }
        memset( &s_tess.prims[ s_tess.prim_count ], 0, sizeof( gui_prim_t ) );
        s_tess.prim_count++;
        s_tess.fx_page_count++;
        s_tess.prim_dedup_floor = s_tess.prim_count;

        s_tess.fx_page      = page;
        s_tess.fx_page_used = 0;
        page_p              = (u8*)&s_tess.prims[ s_tess.slot_prim_base + page ];
    }

    memcpy( page_p + s_tess.fx_page_used * GUI_FX_BYTES, &fx, sizeof fx );
    s_tess.fx_memo_row = s_tess.fx_page * GUI_PRIM_ROWS + s_tess.fx_page_used * GUI_FX_ROWS;
    s_tess.fx_page_used++;
    return s_tess.fx_memo_row;
}

/* The glyph table's rect for an ID, for the quads that cannot carry the ID itself.  Reading the
   table CPU-side costs the same lookup the vertex stage would have done and gives up only the
   repack stability -- which is exactly the trade a straddling glyph already makes. */
static void
tess_glyph_uv( u32 glyph_id, u32* uv0, u32* uv1 )
{
    if ( glyph_id >= glyph_table_count() )
    {
        *uv0 = *uv1 = 0u;
        return;
    }
    const gui_glyph_uv_t* g = &glyph_table_data()[ glyph_id ];
    *uv0 = g->uv0;
    *uv1 = g->uv1;
}

/*==============================================================================================
    tess_quad_push -- the ONE geometry writer.  Resolves the ambient style (placement and clip
    live on the quad, never the style), appends the quad, and folds one element into the open
    GPU command.

    `rule` is the expansion rule (GUI_QUAD_RULE_*).  Placement is the SHAPE's, by the rule's
    convention (gui.h), in pixels -- quantized to the record's quarter-pixel grid here, which is
    the only place that conversion happens.  uv0/uv1 are packed texcoord corners; a non-zero pair
    goes into the instance record beside the turn, since a texture rect is per-instance and the
    quad has no lane for one.

    `glyph_id` past GUI_GLYPH_ID_NONE asks for the GLYPH tag: the quad names a glyph-table entry
    instead of carrying an atlas rect, and names no style record at all -- the fragment resolves
    the text atlas from the push block.  That only holds while the ambient style says nothing but
    "sample the font atlas", so a glyph under an op or a field falls back to the SHAPED tag with
    the table's rect baked in, exactly like a straddling glyph.
==============================================================================================*/

#define GUI_GLYPH_ID_NONE   0xFFFFFFFFu

/* Is a BAND covering worth four quads instead of one?  (gui.h, THE BAND COVERING.)

   The hole this mirrors is the vertex stage's (gui_quad.vs.hlsl, band_local) and must not be a
   second opinion about geometry: band_local clamps its own numbers and tiles its own outer rect
   exactly, so a disagreement here can only mean a shape kept one quad it could have split, never a
   gap or a double-blended seam.  What this decides is the TRADE -- four records and four
   rasterizer setups against the interior they save.

   Restricted to the rounded box under the SKIRT rule, which is the only field whose hole is a
   rectangle band_local can derive.  FRAME is excluded outright: its fill paints the interior it
   would be carving away. */

#define TESS_BAND_MIN_FRAC  0.45f    /* of the covering -- below this the middle is not the cost   */
#define TESS_BAND_MIN_AREA  4096.0f  /* px -- a 64x64 hole, under which four setups beat the fill */

static bool
tess_band_worth_it( f32 qhw, f32 qhh, u32 rule )
{
    const gui_prim_t* p   = &s_tess.cur_prim;
    u32               ops = s_tess.cur_ops;

    /* The repetition ops are excluded for a reason of kind rather than of trade: the region a
       repeated shape leaves at zero coverage is the space BETWEEN its copies, which is not the
       single rectangle band_local knows how to tile.  A band covering there would carve away real
       ink -- and under the polar fold the empty region is the hole in the middle of a ring, which
       is a shape band_local has no way to state at all. */
    if ( rule != GUI_QUAD_RULE_SKIRT || p->field != (u32)GUI_FX_BOX
      || ( ops & ( GUI_OP_FRAME | GUI_OP_REPEAT | GUI_OP_REPEAT_POLAR ) ) )
        return false;

    /* EXACTLY one hole-cutting op.  Each of the three states where its own coverage reaches zero,
       and they measure from different boundaries -- BAND replaces the field INSET would then read,
       so a shape carrying two has a hole neither formula describes.  Rather than reason about which
       is the safe one, such a shape keeps its single quad: overstating a hole is the one failure
       that would clip real ink, and there is no shape in the library that asks for two. */
    u32 hole = ops & ( GUI_OP_CUT | GUI_OP_BAND | GUI_OP_INSET );
    if ( hole == 0u || ( hole & ( hole - 1u ) ) != 0u )
        return false;

    /* Where each op's coverage reaches zero, as a depth inward from the boundary.  CUT's is the
       caster's own outline -- depth 0, moved by the cut vector below. */
    f32 depth = ( hole == GUI_OP_CUT )  ? 0.0f
              : ( hole == GUI_OP_BAND ) ? p->border + p->feather * 0.5f
                                        : p->feather;

    f32 rmax = fmaxf( fmaxf( p->r_tl, p->r_tr ), fmaxf( p->r_br, p->r_bl ) );
    f32 pad  = p->feather * 0.5f + 1.0f;
    f32 in   = depth + 0.29289322f * rmax;

    f32 hix = fmaxf( qhw - in, 0.0f ), hiy = fmaxf( qhh - in, 0.0f );
    f32 hox = qhw + pad,               hoy = qhh + pad;

    /* The cut's offset shifts the hole; what it costs is the part that slides past the outer rect,
       which band_local clamps away.  Charging for it here keeps a shape whose hole barely fits from
       paying four quads for almost nothing. */
    if ( hole == GUI_OP_CUT )
    {
        hix = fmaxf( hix - fabsf( p->cut_dx ), 0.0f );
        hiy = fmaxf( hiy - fabsf( p->cut_dy ), 0.0f );
    }

    /* Both tests, and they are not the same question.  The FRACTION says the middle is where this
       shape's fill actually goes; the AREA says the fill saved is worth four quad records and four
       rasterizer setups instead of one.  A button's outline passes the first and fails the second
       -- its interior is a few hundred px -- and every widget outline on the screen quadrupling its
       quads for that would spend GUI_MAX_QUADS on nothing. */
    f32 hole_area = hix * hiy;
    return hole_area >= TESS_BAND_MIN_AREA
        && hole_area >= TESS_BAND_MIN_FRAC * ( hox * hoy );
}

static void
tess_quad_push( f32 qcx, f32 qcy, f32 qhw, f32 qhh, u32 rule,
                u32 uv0, u32 uv1, u32 tex_idx, u32 abgr, u32 glyph_id )
{
    bool band = tess_band_worth_it( qhw, qhh, rule );

    if ( s_tess.quad_count + ( band ? GUI_QUAD_BAND_COUNT : 1u ) > GUI_MAX_QUADS )
    {
        s_tess.overflow |= TESS_OVF_QUADS;
        return;
    }
    tess_set_tex( tex_idx );
    if ( !tess_ensure_gpu_cmd() )
        return;

    /* BBOX states a covering the vertex stage takes no pad on, so it is the one rule with no slack
       to absorb the quantization below.  A quarter pixel of margin restores it; every other rule
       either grows by the SDF pad or defines the rect it is mapped against, which must not move. */
    if ( rule == GUI_QUAD_RULE_BBOX )
    {
        qhw += 0.25f;
        qhh += 0.25f;
    }

    i16 pcx = gui_quad_pos_pack( qcx ), pcy = gui_quad_pos_pack( qcy );
    u16 phw = gui_quad_ext_pack( qhw ), phh = gui_quad_ext_pack( qhh );

    /* A glyph keeps the tag only while the style would have said nothing: no ops, no field, and an
       fx row inside the narrower field the GLYPH layout has room for.  Anything else -- an SDF
       outline, a pattern, a rotation past the twelfth bit -- takes the table's rect and rejoins
       the SHAPED path, which has a style record to carry the difference. */
    if ( glyph_id != GUI_GLYPH_ID_NONE
      && s_tess.cur_ops == 0u && s_tess.cur_prim.field == 0u )
    {
        /* The tag carries one bit of texture -- which of the two text atlases -- and the fragment
           reads the slot itself from the push block.  That only means the right thing while a font
           samples one of those two, which is the whole of what font_slot_tex can return. */
        ORB_ASSERT( gui_tex_index( tex_idx ) == ( gui_tex_mode( tex_idx ) == GUI_TEX_SDF
                                                  ? res_sdf_idx() : res_atlas_idx() ) );

        u32 fx = tess_fx_local( 0u, 0u );
        if ( fx <= GUI_QUAD_GFX_MASK )
        {
            s_tess.quads[ s_tess.quad_count++ ] = ( gui_quad_t ){
                .cx   = pcx,
                .cy   = pcy,
                .hw   = phw,
                .hh   = phh,
                .idx  = gui_quad_idx_glyph( s_tess.cur_clip_local, glyph_id,
                                            gui_tex_mode( tex_idx ) == GUI_TEX_SDF, fx ),
                .abgr = abgr,
            };
            goto counted;
        }
        /* The fx row is past what the GLYPH layout can name -- fall through and let the SHAPED
           arm carry it, where the field is a bit wider.  The record written just above is left
           behind: the SHAPED arm asks for one carrying the table's rect as well, which is a
           different record.  One wasted entry at the far end of a slot's fx pages. */
    }

    if ( glyph_id != GUI_GLYPH_ID_NONE )
        tess_glyph_uv( glyph_id, &uv0, &uv1 );   /* the fallback bakes what the table holds */

    /* Fold the ambient texture and ops into the style; the clip entry rides the quad below,
       never the style, so a style compares equal across scroll regions. */
    s_tess.cur_prim.tex = s_tess.cur_tex;
    s_tess.cur_prim.ops = s_tess.cur_ops;

    u32 style = tess_prim_local();
    u32 fx    = tess_fx_local( uv0, uv1 );

    /* The BAND covering emits the same quad four times over, differing in nothing but which strip
       of the frame each expands to.  One placement, one style, one clip, one fx record -- so the
       fragment resolves every one of them exactly as it resolved the single quad they replace. */
    for ( u32 b = 0; b < ( band ? GUI_QUAD_BAND_COUNT : 1u ); ++b )
        s_tess.quads[ s_tess.quad_count++ ] = ( gui_quad_t ){
            .cx    = pcx,
            .cy    = pcy,
            .hw    = phw,
            .hh    = phh,
            .abgr  = abgr,
            .idx   = band ? gui_quad_idx_band( b, s_tess.cur_clip_local, style, fx )
                          : gui_quad_idx( rule, s_tess.cur_clip_local, style, fx ),
        };

counted:

    /* elem_count counts QUADS under this backend; the flush multiplies by six at the draw. */
    s_tess.gpu_cmds[ s_tess.cmd_count - 1 ].cmd.elem_count += band ? GUI_QUAD_BAND_COUNT : 1;

    if ( s_tess.cur_is_text && !s_volatile_patching )
        s_tess.slot_text_quads++;
}

/* Tessellate a filled quad into s_tess.  abgr has alpha pre-baked by the emit side. */
static void
tess_rect_filled( f32 x, f32 y, f32 w, f32 h,
                  f32 u0, f32 v0, f32 u1, f32 v1,
                  u32 tex_idx, u32 abgr )
{
    /* tex_idx 0 = solid-color convention: GUI_OP_SELF says "do not consult the texel", which
       cuts the fill's last tie to atlas PLACEMENT (the texture INDEX it still carries is
       repack-stable), so a plain fill's style never goes stale. */
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        s_tess.cur_ops |= GUI_OP_SELF;
        u0 = v0 = u1 = v1 = 0.0f;
    }
    x = tess_snap_px( x );
    y = tess_snap_px( y );
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr,
                    GUI_GLYPH_ID_NONE );
}

/* Tessellate one whole glyph: same placement as tess_rect_filled, but the quad names a glyph-table
   entry instead of carrying an atlas rect.  The rect is resolved in the vertex stage, so this quad
   survives an atlas repack that would leave a baked uv sampling another tenant's pixels.
   Only for a glyph drawn ENTIRELY -- a run cut to its window narrows one glyph's uv span, which is
   per-instance and has no table entry, so that case stays on tess_rect_filled. */
static void
tess_rect_glyph( f32 x, f32 y, f32 w, f32 h, u32 glyph_id, u32 tex_idx, u32 abgr )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0u, 0u, tex_idx, abgr, glyph_id );
}

/* Tessellate a two-color gradient quad: a GRAD style -- a quad record carries ONE colour, and
   the fragment's linear ramp is the same photometric blend the old per-vertex corner
   interpolation produced.  The axis is stored pre-divided by the extent (the tess_fx_box_core
   convention), so the style dedups across same-size ramps.  Origin grid-snapped like
   tess_rect_filled.  rot_cos is written explicitly: the ramp is evaluated in the shape-local
   frame, and a zeroed rot pair would collapse it to a flat 50/50 blend. */
static void
tess_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_GRAD | GUI_OP_DITHER;
    s_tess.cur_prim.col_b   = col_b;
    s_tess.cur_prim.grad_x  = horizontal ? 1.0f : 0.0f;
    s_tess.cur_prim.grad_y  = horizontal ? 0.0f : 1.0f;
    s_tess.cur_rot_c = 1.0f;
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), col_a, GUI_GLYPH_ID_NONE );
}

/*==============================================================================================
    Sprites and the nine-slice expansion

    ONE semantic command becomes 1, 3 or 9 quads here.  The expansion lives at tessellation time
    rather than in the emit layer for two reasons, and they are the same reason twice: only the
    registry knows the source's pixel size and slice insets, and only tessellation runs late enough
    that a sprite-atlas repack (which moves UVs) is already accounted for.  Emitting quads early
    would bake both facts into a command that outlives them.

    The pieces come out of ONE atlas with ONE bindless slot, so a whole nine-slice frame is a single
    GPU batch -- which is what makes an authored border affordable on every panel rather than a
    special occasion.
==============================================================================================*/

/* Repeat cap per axis for a TILEd piece.  Past this the piece stretches instead: a pathological
   pitch (art authored 1px wide, or a scale near zero) would otherwise turn one command into tens
   of thousands of quads, and a stretched fallback is wrong in a way you can see and fix, where an
   exhausted vertex budget is wrong in a way that takes the rest of the frame down with it. */
#define TESS_SPRITE_TILE_MAX  64u

/* One piece of the grid.  pitch <= 0 on an axis means "stretch across that axis" (the default);
   a positive pitch repeats the piece at authored size, trimming the trailing repeat to fit by
   cutting its UV in the same proportion.  UVs may arrive reversed (u0 > u1) -- that is how a flip
   is expressed -- and every interpolation here is a lerp from u0 toward u1, so reversal carries
   through the trim untouched. */
static void
tess_sprite_piece( f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   f32 pitch_x, f32 pitch_y, u32 tex_idx, u32 abgr )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    u32 nx = 1, ny = 1;
    if ( pitch_x > 0.5f )
    {
        f32 n = ceilf( w / pitch_x );
        if ( n < 1.0f || n > (f32)TESS_SPRITE_TILE_MAX ) pitch_x = 0.0f;   /* stretch instead */
        else                                             nx = (u32)n;
    }
    if ( pitch_y > 0.5f )
    {
        f32 n = ceilf( h / pitch_y );
        if ( n < 1.0f || n > (f32)TESS_SPRITE_TILE_MAX ) pitch_y = 0.0f;
        else                                             ny = (u32)n;
    }

    for ( u32 j = 0; j < ny; ++j )
    {
        f32 py = ( pitch_y > 0.0f ) ? y + (f32)j * pitch_y : y;
        f32 ph = ( pitch_y > 0.0f ) ? pitch_y : h;
        if ( py + ph > y + h ) ph = y + h - py;          /* trailing repeat: cut to the piece */
        f32 fv = ( pitch_y > 0.0f ) ? ph / pitch_y : 1.0f;
        f32 pv1 = v0 + ( v1 - v0 ) * fv;

        for ( u32 i = 0; i < nx; ++i )
        {
            f32 px = ( pitch_x > 0.0f ) ? x + (f32)i * pitch_x : x;
            f32 pw = ( pitch_x > 0.0f ) ? pitch_x : w;
            if ( px + pw > x + w ) pw = x + w - px;
            f32 fu = ( pitch_x > 0.0f ) ? pw / pitch_x : 1.0f;
            f32 pu1 = u0 + ( u1 - u0 ) * fu;

            tess_rect_filled( px, py, pw, ph, u0, v0, pu1, pv1, tex_idx, abgr );
        }
    }
}

/* Tessellate one sprite command.  Resolves the sprite through the source contract, then either
   stretches it over the rect as a single quad or lays the nine-slice grid.

   Slice insets are authored in SOURCE pixels and scaled by the command's `scale`, so one piece of
   art serves several UI scales; when the scaled insets no longer fit the destination they are
   shrunk proportionally rather than allowed to overlap, which is what keeps a frame legible when
   its window is dragged smaller than its own corners. */
static void
tess_sprite( const gui_cmd_t* c )
{
    u32 tex = res_sprite_idx();
    if ( tex == 0 )
        return;                       /* no sprite atlas yet -- nothing was ever registered */

    f32       u0, v0, u1, v1;
    u32       sw = 0, sh = 0;
    gui_pad_t sl = { 0 };
    if ( !sprite_get( c->sprite.sprite, &u0, &v0, &u1, &v1, &sw, &sh, &sl ) || sw == 0 || sh == 0 )
        return;

    tex |= GUI_TEX_MODE( GUI_TEX_RGBA );   /* the texel IS the colour; vertex colour tints it */

    const u32 flags  = c->sprite.flags;
    const bool flipx = ( flags & GUI_BRUSH_FLIP_X ) != 0;
    const bool flipy = ( flags & GUI_BRUSH_FLIP_Y ) != 0;
    const bool tile  = ( flags & GUI_BRUSH_TILE   ) != 0;

    const f32 x = c->sprite.x, y = c->sprite.y, w = c->sprite.w, h = c->sprite.h;
    const f32 s = c->sprite.scale;
    const u32 col = c->sprite.abgr;

    /* Scaled slice insets.  A sprite with none (or a command that did not ask for the expansion)
       is one stretched quad -- the flip is then just a reversed UV span. */
    f32 L = sl.l * s, R = sl.r * s, T = sl.t * s, B = sl.b * s;
    if ( !c->sprite.nine || ( L <= 0.0f && R <= 0.0f && T <= 0.0f && B <= 0.0f ) )
    {
        tess_rect_filled( x, y, w, h,
                          flipx ? u1 : u0, flipy ? v1 : v0,
                          flipx ? u0 : u1, flipy ? v0 : v1, tex, col );
        return;
    }

    /* Shrink insets that no longer fit rather than letting opposite corners overlap. */
    if ( L + R > w && L + R > 0.0f ) { f32 k = w / ( L + R ); L *= k; R *= k; }
    if ( T + B > h && T + B > 0.0f ) { f32 k = h / ( T + B ); T *= k; B *= k; }

    /* A flip mirrors the whole sprite, so the destination edge widths swap with the source columns
       they will sample -- do it here, once, and the grid loop below stays flip-agnostic. */
    if ( flipx ) { f32 t2 = L; L = R; R = t2; }
    if ( flipy ) { f32 t2 = T; T = B; B = t2; }

    /* Destination and source boundaries, three tracks each.  su/sv are in UV; the middle source
       track is the stretchable / tileable span between the insets. */
    const f32 upx = ( u1 - u0 ) / (f32)sw;   /* UV per source pixel */
    const f32 vpx = ( v1 - v0 ) / (f32)sh;

    f32 dx[ 4 ] = { x, x + L, x + w - R, x + w };
    f32 dy[ 4 ] = { y, y + T, y + h - B, y + h };
    f32 su[ 4 ] = { u0, u0 + sl.l * upx, u1 - sl.r * upx, u1 };
    f32 sv[ 4 ] = { v0, v0 + sl.t * vpx, v1 - sl.b * vpx, v1 };

    /* Authored pitch of the middle tracks, for TILE.  The corners never tile (they ARE the fixed
       part); the edges tile along their long axis only, and the centre tiles on both. */
    f32 pitch_x = tile ? ( (f32)sw - sl.l - sl.r ) * s : 0.0f;
    f32 pitch_y = tile ? ( (f32)sh - sl.t - sl.b ) * s : 0.0f;

    for ( u32 j = 0; j < 3; ++j )
        for ( u32 i = 0; i < 3; ++i )
        {
            /* Under a flip the destination track samples the mirrored source track, and the span
               comes out reversed (lo > hi) -- which is exactly the mirrored sampling wanted. */
            u32 si = flipx ? ( 3u - i ) : i;
            u32 sj = flipy ? ( 3u - j ) : j;
            f32 pu0 = su[ si ], pu1 = flipx ? su[ si - 1u ] : su[ si + 1u ];
            f32 pv0 = sv[ sj ], pv1 = flipy ? sv[ sj - 1u ] : sv[ sj + 1u ];

            tess_sprite_piece( dx[ i ], dy[ j ], dx[ i + 1 ] - dx[ i ], dy[ j + 1 ] - dy[ j ],
                               pu0, pv0, pu1, pv1,
                               ( i == 1 ) ? pitch_x : 0.0f,
                               ( j == 1 ) ? pitch_y : 0.0f,
                               tex, col );
        }
}

/* Tessellate a hollow rectangle as four edge quads.

   The frame is snapped to whole pixels ONCE, here, and the four quads are cut from those integer
   edges.  Every fill snaps its own origin (tess_rect_filled) and nothing snaps its extent, so
   handing four unsnapped rects to it rounds four origins independently against four fractional
   far edges: the top rail lands on one row and the side rails start on another, and the right
   rail's inner edge misses the top rail's end.  That reads as pixel gaps at the corners and a
   one-pixel overhang on the right and bottom -- on any fractional origin (a scrolled row, a
   fractional layout position) and worse the thicker the stroke.  Snapped first, the shared edges
   are the same integer on both sides and the joins are exact.

   t is rounded to a whole stroke (never below one pixel, so a hairline cannot vanish) and clamped
   to half the shorter side, so a thick border on a small rect degenerates to a filled rect
   instead of inverted side quads. */
static void
tess_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    f32 x0 = tess_snap_px( x ),     y0 = tess_snap_px( y );
    f32 x1 = tess_snap_px( x + w ), y1 = tess_snap_px( y + h );
    f32 bw = x1 - x0,               bh = y1 - y0;
    if ( bw <= 0.0f || bh <= 0.0f )
        return;

    f32 tmax = ( bw < bh ? bw : bh ) * 0.5f;
    t = tess_snap_px( t );
    if ( t < 1.0f ) t = 1.0f;
    if ( t > tmax ) t = tmax;

    tess_rect_filled( x0,     y0,     bw, t, 0,0,1,1, 0, abgr );
    tess_rect_filled( x0,     y1 - t, bw, t, 0,0,1,1, 0, abgr );

    f32 mid = bh - 2.0f * t;   /* the span between the rails; zero once t swallowed the box */
    if ( mid > 0.0f )
    {
        tess_rect_filled( x0,     y0 + t, t, mid, 0,0,1,1, 0, abgr );
        tess_rect_filled( x1 - t, y0 + t, t, mid, 0,0,1,1, 0, abgr );
    }
}

/*==============================================================================================
    The SDF surface -- every rounded shape, in ONE quad.

    The CPU emits a covering quad and the fragment shader resolves the boundary exactly (gui.h,
    the effect band).  The covering is grown past the box by the falloff pad (the SKIRT rule) so
    a feathered edge -- and a shadow's whole soft skirt -- has somewhere to land; the BOUNDARY
    still sits exactly on the authored rect.

    A wide BAND/CUT/INSET surface rasterizes its interior at zero coverage (the fields early-out
    cheaply); the hole-carving the old vertex path did for those has no home in a one-rectangle
    record, and UI fill is nowhere near bound.

    Every SDF surface samples the same atlas as everything else and names its shape through a
    STYLE record, so it merges into whatever GPU command is already open: a soft shadow behind a
    panel costs a batch split of zero.
==============================================================================================*/

/* Emit one SDF surface.  `r4` is the corner radius PER QUADRANT, in the tessellation order
   top-left, top-right, bottom-right, bottom-left (tess_fx_box passes four copies of one radius;
   only tess_round_rect_ex passes four different ones), and `feather` the total width of the falloff
   band straddling the boundary (0 = hard edge); both are always read.  The remaining parameters are
   OP-SPECIFIC, mirroring the record's own re-partitioning -- `border` is the border width under
   GUI_OP_BAND, `rate`/`depth` the wave under GUI_OP_PULSE, and each ignores the other's.
   UVs span the AUTHORED box and are clamped over the grown skirt, so a textured rounded quad cannot
   bleed into its atlas neighbour where the coverage has already faded to nothing.

   The surface is always GUI_FX_BOX; which of the four ops it carries comes in on ambient state
   (s_tess.cur_ops), set by the caller BEFORE this runs.  GUI_OP_CUT and GUI_OP_INSET take no
   parameter of their own -- they read radius and feather exactly as a plain fill does.

   Per-corner radii are FREE: all four ride the record and the fragment picks the one its own
   quadrant wants, so the geometry does not change at all.

   Why neighbouring quadrants cannot seam, which is the part that has to be true for any of this to
   work.  Each quadrant measures from its OWN radius, so the obvious worry is that the two sides of
   a shared centre line disagree.  They cannot.  Take the horizontal one (local.y = 0): q.y is
   r - hy, and r is clamped to lim <= hy, so q.y <= 0 for every radius.  The y term therefore drops
   out of both branches of the field and what remains is

       d = max( |local.x| - hx, -hy )

   in which r has cancelled.  The same holds on the vertical centre line.  The selection lines are
   precisely where the corner radius stops contributing, so the two sides agree EXACTLY -- not
   approximately, and not merely because the interior saturates.  That was the load-bearing claim
   when the quadrants were separate QUADS and it is the same claim now that they are separate
   BRANCHES of one fragment, which is why the shape survived the fold moving. */
/* `rot` turns the whole surface about the box CENTRE (radians, screen space; 0 = the common
   axis-aligned path).  Only the corner POSITIONS rotate; the fragment un-rotates by the same pair
   out of the record to recover its box-local coordinate.  The UVs are computed from the UNROTATED
   position first, so a textured rotated box still maps its picture across the authored rect and
   clamps over the skirt exactly as the upright one does. */
/*----------------------------------------------------------------------------------------------
    tess_fx_aux_t -- the two extras a box surface can carry, absent from every plain fill.

    One pointer rather than four more parameters, because that is what they are: a rarely-taken
    branch off a call that already states sixteen things.  Both are read only when the op that owns
    them is set, the same rule `border` and `rate`/`depth` follow.
----------------------------------------------------------------------------------------------*/
typedef struct
{
    u32 grad_col;         // GUI_OP_GRAD: the ramp's far colour
    f32 grad_ang;         // GUI_OP_GRAD: axis, radians, box-local, 0 points +x (linear ramp only)
    f32 grad_mid;         // GUI_OP_GRAD: midpoint bend, already the exponent (0 = linear)
    f32 cut_dx, cut_dy;   // GUI_OP_CUT: the cut boundary's centre, offset from this shape's
    f32 anim_rate;        // CYCLES/SEC, for every animating op -- one unit, whatever a cycle
                          //   means to the op reading it (gui.h row 5)
    f32 anim_phase;       // CYCLES, the static offset that staggers same-rate elements
    u32 anim_curve;       // gui_curve_t: what the phase does between its endpoints
    f32 anim_param;       // the curve's own parameter -- exponent, step count, duty
    f32 dash_period;      // GUI_OP_DASH: px per on+off cycle, already snapped to the perimeter
    f32 dash_duty;        // GUI_OP_DASH: on-fraction of the period
    f32 dash_scroll;      // GUI_OP_DASH: periods slid per cycle -- 1 marches, 0 pins to the shape

    /* GUI_OP_REPEAT: the lattice, sharing row 6 with the dash above (gui.h).  The pitch is
       centre-to-centre and the cell is HALF the copy's size, which is the form the fragment folds
       in.  The count is not here because it is not stored -- see tess_repeat_box.
       GUI_OP_REPEAT_POLAR reads the first two lanes as the copy COUNT and the orbit radius
       instead; its count IS stored, since a circle has no extent to recover one from. */
    f32 rep_pitch_x, rep_pitch_y;
    f32 rep_cell_hx, rep_cell_hy;

} tess_fx_aux_t;

/* `aux` NULL is a plain fill with neither extra -- almost every caller. */
static void
tess_fx_box_core( f32 x, f32 y, f32 w, f32 h, const f32* r4,
                  f32 feather, f32 border, f32 rate, f32 depth, f32 rot,
                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
                  const tess_fx_aux_t* aux )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    /* Clamp what is GEOMETRICALLY meaningless, and only that.  The long list that used to live
       here was the packed word's doing: every field had a fixed-point ceiling, and the geometry
       below is built from the same numbers, so a value the fragment could not see would leave the
       quad describing a different shape than the one it resolved.  The record has no
       ceilings, so what remains is the one bound that is about the SHAPE rather than the storage
       -- a corner radius past half the short side is a capsule -- plus the negatives, which are
       nonsense in every field. */
    f32 hx = w * 0.5f, hy = h * 0.5f;
    f32 lim = ( hx < hy ) ? hx : hy;

    f32 rq[ 4 ];
    f32 rmin, rmax;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 r = r4[ i ];
        if ( r > lim ) r = lim;                   /* a radius past half the short side is a capsule */
        if ( r < 0.0f ) r = 0.0f;
        rq[ i ] = r;
    }
    rmin = rmax = rq[ 0 ];
    for ( u32 i = 1; i < 4; ++i )
    {
        if ( rq[ i ] < rmin ) rmin = rq[ i ];
        if ( rq[ i ] > rmax ) rmax = rq[ i ];
    }
    if ( feather < 0.0f ) feather = 0.0f;
    if ( border  < 0.0f ) border  = 0.0f;
    if ( rate    < 0.0f ) rate  = 0.0f;
    if ( depth   < 0.0f ) depth = 0.0f;
    if ( depth   > 1.0f ) depth = 1.0f;   /* a fraction, not a pixel count -- a real upper bound */

    /* Grid-snap the origin like tess_rect_filled -- UNLESS the shape is a circle.
       Snapping exists to keep STRAIGHT edges crisp, and it is derived rather than passed in
       because the condition is a property of the shape, not of the caller: a shape has no straight
       edge in either axis exactly when it is square and its radius reached the half-extent.  That
       is a disc, and it is also a circular RING -- so both fall out of one test, which is what
       keeps them aligned.  It matters that they agree: a filled disc and a ring drawn at the same
       centre would otherwise sit up to half a pixel apart, and concentric marks are precisely how
       these get used.
       Snapping a circle is not merely pointless but harmful.  Its origin is (centre - r), so
       snapping quantizes the CENTRE, and a small dot animating along a path steps instead of
       gliding.  A pill (w != h, r == the short half-extent) still snaps, correctly -- it does have
       two straight edges.  With per-corner radii the test reads rMIN: a shape is a disc only when
       EVERY corner reached the limit, and one square corner is a straight edge worth snapping.
       A ROTATED box never snaps: it has no axis-aligned edge to keep crisp, and quantizing its
       centre is the animated-dot mistake again (tess_quad_xf's rule). */
    if ( rot == 0.0f && !( hx == hy && rmin >= lim ) )
    {
        x = tess_snap_px( x );
        y = tess_snap_px( y );
    }

    /* tex_idx 0 = solid-color convention, same as tess_rect_filled: GUI_OP_SELF, no texel
       consulted at all. */
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        s_tess.cur_ops |= GUI_OP_SELF;
        u0 = v0 = u1 = v1 = 0.0f;
    }

    f32 cx  = x + hx,   cy  = y + hy;
    f32 rcs = 1.0f, rsn = 0.0f;                   /* the rotation, computed once per shape */
    if ( rot != 0.0f ) { rcs = cosf( rot ); rsn = sinf( rot ); }

    /* The style: the AUTHORED shape, after the clamps and the grid snap above.  The falloff
       skirt is a rasterization detail the vertex stage grows the covering by (GUI_QUAD_RULE_
       SKIRT) -- the field the fragment resolves is measured from the real boundary, so the
       record states that one.  All four radii travel. */
    s_tess.cur_prim.field   = (u32)GUI_FX_BOX;
    s_tess.cur_prim.r_tl    = rq[ 0 ];
    s_tess.cur_prim.r_tr    = rq[ 1 ];
    s_tess.cur_prim.r_br    = rq[ 2 ];
    s_tess.cur_prim.r_bl    = rq[ 3 ];
    s_tess.cur_prim.feather = feather;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & ( GUI_OP_BAND | GUI_OP_FRAME ) ) ? border : 0.0f;
    s_tess.cur_rot_c = rcs;
    s_tess.cur_rot_s = rsn;
    /* The pulse states only its DEPTH as a parameter of its own; its rate joins the shared
       animation clock, which is what lets a curve authored once shape the breath, the spin and
       the marching ants alike. */
    s_tess.cur_prim.param_a = ( s_tess.cur_ops & GUI_OP_PULSE ) ? depth : 0.0f;
    if ( s_tess.cur_ops & GUI_OP_PULSE )
        s_tess.cur_prim.anim_rate = rate;

    /* The corner profile -- ambient over the command, like the ops, and applied only where there
       is a corner to profile: a square box has no arc to reshape, and leaving the lane at zero is
       what keeps square fills deduping onto one record. */
    s_tess.cur_prim.corner_pow = ( rmax > 0.0f ) ? s_tess.cur_corner_pow : 0.0f;

    /* GUI_OP_GLOW's dropoff, derived from the reach the caller already stated as the feather --
       there is no second parameter, because the distance the glow travels and the distance the
       covering has to grow by are the same distance.  ln(255) puts the falloff under one 8-bit
       step at half the feather, which is where the vertex stage's SKIRT pad ends: the halo fades
       out exactly at the edge of the quad carrying it, rather than being cut off inside it. */
    if ( s_tess.cur_ops & GUI_OP_GLOW )
        s_tess.cur_prim.glow_k = 5.5413f / fmaxf( feather * 0.5f, 0.5f );

    /* DITHER, derived rather than asked for: a wide falloff and a colour ramp are the two shapes
       that band on an 8-bit target, and half a step of screen noise is invisible everywhere else
       it could apply.  The 1 px AA feather stays clean -- there is no ramp to band. */
    if ( feather > 2.0f || ( s_tess.cur_ops & GUI_OP_GRAD ) )
        s_tess.cur_ops |= GUI_OP_DITHER;

    /* The animation lane and the perimeter dash, written only under the op that reads each --
       the zero-when-unused rule that keeps plain fills deduping onto one record. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_DASH ) )
    {
        s_tess.cur_prim.dash_period = aux->dash_period;
        s_tess.cur_prim.dash_duty   = aux->dash_duty;
        s_tess.cur_prim.dash_scroll = aux->dash_scroll;
    }

    /* The repetition ops read the SAME row under different names (gui.h, row 6).  Writing them
       here, beside the dash they share with, is what keeps "at most one of them" visible at the one
       place any of them can be written. */
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_REPEAT | GUI_OP_REPEAT_POLAR ) ) )
    {
        s_tess.cur_prim.dash_period = aux->rep_pitch_x;
        s_tess.cur_prim.dash_duty   = aux->rep_pitch_y;
        s_tess.cur_prim.dash_scroll = aux->rep_cell_hx;
        s_tess.cur_prim.reserved_c  = aux->rep_cell_hy;
    }
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_DASH | GUI_OP_SPIN ) ) )
        s_tess.cur_prim.anim_rate = aux->anim_rate;
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_DASH | GUI_OP_PULSE | GUI_OP_SPIN ) ) )
    {
        s_tess.cur_prim.anim_curve = aux->anim_curve;
        s_tess.cur_prim.anim_param = aux->anim_param;
        s_tess.cur_phase = aux->anim_phase;
    }

    /* GUI_OP_FRAME's border colour does NOT land here -- it rides the quad (cur_col_border, set by
       the dispatcher before this call), so an animated border never adds a style record.  This
       record's own col_b is the SHAPE's second colour (GRAD, CHECKER, TEXT_EDGE, ARC_GRAD); the
       two are different lanes on purpose.  See gui_fx_t.col_border (gui.h) and the dispatch of
       GUI_CMD_FRAME below. */

    /* GUI_OP_GRAD -- the ramp's far colour and its axis, stored as a UNIT direction.  The
       fragment divides by the extent the shape spans along it, recovered from the placement it
       already holds.  Storing the direction rather than direction-over-extent is what lets ONE
       record serve every size: the same ramp on a 40 px chip and a 400 px panel used to be two
       records, because the divisor was baked in here.  Linear and conic now store the same
       thing, which is also one branch fewer. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_GRAD ) )
    {
        s_tess.cur_prim.col_b    = aux->grad_col;
        s_tess.cur_prim.grad_mid = aux->grad_mid;

        /* A radial ramp has no axis, so it stays ZERO rather than carrying an angle the fragment
           will not read -- otherwise two identical radial fills authored at different angles take
           two records for no reason (tess_prim_local memos on the record's bytes). */
        if ( !( s_tess.cur_ops & GUI_OP_GRAD_RADIAL ) )
        {
            s_tess.cur_prim.grad_x = cosf( aux->grad_ang );
            s_tess.cur_prim.grad_y = sinf( aux->grad_ang );
        }
    }

    /* GUI_OP_CUT -- where the cut boundary sits.  Zero is the shape cutting itself, which is every
       caller that wants a shadow cast straight down onto the ground under its subject; a non-zero
       offset is the DIRECTIONAL cast, the falloff measured from this outline while the hole is
       taken against the caster's. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_CUT ) )
    {
        /* `+ 0.0f` folds NEGATIVE ZERO onto positive.  A caller that negates an offset it was
           handed hands one down for free (draw_push_skirt passes -ox), and -0.0f compares equal to
           0.0f while hashing and memcmp-ing as a different record -- so the un-offset cut would
           quietly take a second style entry that draws exactly the same shape. */
        s_tess.cur_prim.cut_dx = aux->cut_dx + 0.0f;
        s_tess.cur_prim.cut_dy = aux->cut_dy + 0.0f;
    }

    /* The COVERING: one quad, the shape's true extents under the SKIRT rule (the vertex stage
       grows them by the style's feather pad).  The old vertex path carved an interior hole out
       of wide BAND/CUT/INSET surfaces; a record stores one rectangle, so the interior rasterizes
       at zero coverage instead -- the fields early-out cheaply and UI fill is nowhere near
       bound.  The uv rect is the authored span; the vertex stage scales it over the skirt and
       clamps at the corners, so a textured rounded quad shows its picture at authored size. */
    tess_quad_push( cx, cy, hx, hy, GUI_QUAD_RULE_SKIRT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr,
                    GUI_GLYPH_ID_NONE );
}

/*----------------------------------------------------------------------------------------------
    tess_repeat_box -- nx by ny copies of one rounded cell, from ONE quad.

    The quad spans the whole set and the fragment folds its coordinate into a cell (GUI_OP_REPEAT),
    so the copy count costs nothing: a 3x3 grip and a 40-tick ruler are the same one record and the
    same one quad.

    THIS FUNCTION OWNS THE SIZING CONTRACT the fragment decodes against.  The set's half-extent must
    be exactly (n-1)/2 pitches plus one cell on each axis, because that is what the count is
    recovered from -- state the quad any other way and the lattice repeats the wrong number of
    times.  Deriving the extent here rather than taking a rect from the caller is what makes that
    unbreakable: there is no rect to get wrong.

    (cx, cy) is the SET's centre.  The pitch is floored just above the cell so copies can never
    touch, which the recovery also depends on.
----------------------------------------------------------------------------------------------*/

static void
tess_repeat_box( f32 cx, f32 cy, u32 nx, u32 ny, f32 pitch_x, f32 pitch_y,
                 f32 cell_w, f32 cell_h, f32 rounding, u32 abgr )
{
    if ( nx == 0u || ny == 0u || cell_w <= 0.0f || cell_h <= 0.0f )
        return;

    f32 chx = cell_w * 0.5f, chy = cell_h * 0.5f;

    /* Copies that touch would blur two cells into one shape AND break the count recovery, which
       divides the span by the pitch.  A hair over twice the half-extent keeps both honest. */
    f32 px = ( pitch_x > cell_w ) ? pitch_x : cell_w + 1.0f;
    f32 py = ( pitch_y > cell_h ) ? pitch_y : cell_h + 1.0f;

    /* The half-span of copy CENTRES, then the set's own half-extent. */
    f32 spanx = (f32)( nx - 1u ) * 0.5f * px;
    f32 spany = (f32)( ny - 1u ) * 0.5f * py;
    f32 hx    = spanx + chx;
    f32 hy    = spany + chy;

    /* A radius past half the cell's short side is a pill end; past that it is nothing the cell can
       be.  Clamped against the CELL, not the set, since the cell is the shape. */
    f32 lim = ( chx < chy ) ? chx : chy;
    if ( rounding > lim )  rounding = lim;
    if ( rounding < 0.0f ) rounding = 0.0f;

    tess_fx_aux_t aux = { 0 };
    aux.rep_pitch_x = px;
    aux.rep_pitch_y = py;
    aux.rep_cell_hx = chx;
    aux.rep_cell_hy = chy;

    s_tess.cur_ops |= GUI_OP_REPEAT;

    /* Through the ordinary box path so the cell gets the same clamps, the same solid-fill
       convention and the same corner profile every other rounded shape does.  It states the SET's
       rect; the record's cell extent is what the field actually measures against. */
    const f32 r4[ 4 ] = { rounding, rounding, rounding, rounding };
    tess_fx_box_core( cx - hx, cy - hy, hx * 2.0f, hy * 2.0f, r4, TESS_FX_AA, 0.0f,
                      0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, abgr, &aux );
}

/*----------------------------------------------------------------------------------------------
    tess_repeat_polar -- n copies of one cell on a circle, from ONE quad.

    The angular twin of tess_repeat_box, and the reason the pair is worth having: at `rate` > 0 the
    ring turns on the SHADER CLOCK (GUI_OP_SPIN, composed upstream of the fold), so a rotating dot
    spinner is one quad whose command bytes never change -- it re-tessellates nothing while it runs.
    The caller still presents frames with request_redraw, the draw_pulse contract.

    Unlike the linear fold the count is STORED: a circle has no extent to recover one from, and the
    orbit takes the lane the second pitch would have used.

    The cell's frame turns with its angular position, so a WIDE cell reads as a dial tick pointing
    outward and a square one as a dot.  That is the only difference between the two things this
    draws.
----------------------------------------------------------------------------------------------*/

static void
tess_repeat_polar( f32 cx, f32 cy, u32 n, f32 orbit, f32 cell_w, f32 cell_h,
                   f32 rounding, f32 rate, f32 phase, u32 curve, f32 curve_param, u32 abgr )
{
    if ( n == 0u || cell_w <= 0.0f || cell_h <= 0.0f || orbit <= 0.0f )
        return;

    f32 chx = cell_w * 0.5f, chy = cell_h * 0.5f;

    /* The set's half-extent.  A cell sits at distance `orbit` in some direction, so the furthest
       it reaches on either axis is orbit + that axis' half-extent -- tight at the four cardinal
       angles and conservative everywhere between. */
    f32 hx = orbit + chx;
    f32 hy = orbit + chy;

    f32 lim = ( chx < chy ) ? chx : chy;
    if ( rounding > lim )  rounding = lim;
    if ( rounding < 0.0f ) rounding = 0.0f;

    tess_fx_aux_t aux = { 0 };
    aux.rep_pitch_x = (f32)n;      /* the count, where the linear fold states a pitch */
    aux.rep_pitch_y = orbit;
    aux.rep_cell_hx = chx;
    aux.rep_cell_hy = chy;

    s_tess.cur_ops |= GUI_OP_REPEAT_POLAR;

    /* SPIN turns prim_frame, which is upstream of the angular fold, so the whole ring rotates as
       one rigid body.  With CURVE_STAIR at `n` steps it advances exactly one copy per step -- the
       mechanical clock-hand spinner, from the same record as the smooth one. */
    if ( rate > 0.0f )
    {
        s_tess.cur_ops  |= GUI_OP_SPIN;
        aux.anim_rate    = rate;
        aux.anim_phase   = phase;
        aux.anim_curve   = curve;
        aux.anim_param   = curve_param;
    }

    const f32 r4[ 4 ] = { rounding, rounding, rounding, rounding };
    tess_fx_box_core( cx - hx, cy - hy, hx * 2.0f, hy * 2.0f, r4, TESS_FX_AA, 0.0f,
                      0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, abgr, &aux );
}

/* The uniform-radius entry every rounded shape in the library goes through.  Four copies of one
   radius is not a workaround -- it is the honest statement that a rounded rect is the special case
   of a per-corner one, and it keeps a single tessellator for both. */
static void
tess_fx_box( f32 x, f32 y, f32 w, f32 h, f32 r, f32 feather, f32 border, f32 rate, f32 depth,
             f32 rot, f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
             const tess_fx_aux_t* aux )
{
    const f32 r4[ 4 ] = { r, r, r, r };
    tess_fx_box_core( x, y, w, h, r4, feather, border, rate, depth, rot,
                      u0, v0, u1, v1, tex_idx, abgr, aux );
}

/* TESS_FX_AA -- the default 1 px antialiasing band -- lives in gui_render.h now: the emit side
   bakes it into commands (a pulse's feather) as well. */

/* Tessellate a solid triangle into s_tess: the GUI_FX_TRI field -- one quad over the bbox,
   three points about its centre in the style's radius + param lanes.  Centre-relative points
   are what lets repeated arrow glyphs share one style; the edges antialias through the shared
   feather, which the old rasterized triangle never had. */
static void
tess_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    f32 lox = fminf( ax, fminf( bx, cx ) ), hix = fmaxf( ax, fmaxf( bx, cx ) );
    f32 loy = fminf( ay, fminf( by, cy ) ), hiy = fmaxf( ay, fmaxf( by, cy ) );
    if ( hix <= lox || hiy <= loy )
        return;
    f32 qx = ( lox + hix ) * 0.5f, qy = ( loy + hiy ) * 0.5f;

    s_tess.cur_ops         |= GUI_OP_SELF;
    s_tess.cur_prim.field   = (u32)GUI_FX_TRI;
    s_tess.cur_prim.r_tl    = ax - qx;
    s_tess.cur_prim.r_tr    = ay - qy;
    s_tess.cur_prim.r_br    = bx - qx;
    s_tess.cur_prim.r_bl    = by - qy;
    s_tess.cur_prim.param_a = cx - qx;
    s_tess.cur_prim.param_b = cy - qy;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_rot_c = 1.0f;
    tess_quad_push( qx, qy, ( hix - lox ) * 0.5f, ( hiy - loy ) * 0.5f,
                    GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}

/*==============================================================================================
    tess_circle_filled -- a disc, which is a rounded box whose radius reached its half-extent.

    There is no circle primitive anywhere in the pipeline and there does not need to be one:
    tess_fx_box clamps the corner radius to half the short side, so a SQUARE box asking for a
    radius of its own half-extent degenerates exactly to a disc -- same field, same one quad,
    same fragment, antialiased at any size.  The emit side agrees (draw_push_circle_filled emits
    GUI_CMD_RECT_FILLED with rounding = r); this helper survives for tess_fx_arc's full-turn PIE
    route.

    NOT grid-snapped, and it does not have to ask: tess_fx_box derives it -- a square whose radius
    reached its half-extent has no straight edge for snapping to keep crisp, and quantizing a
    circle's centre is exactly what a small moving dot must not do.  A circular RING satisfies the
    same test, so the two stay aligned when drawn concentrically.
==============================================================================================*/

static void
tess_circle_filled( f32 pcx, f32 pcy, f32 r, u32 abgr )
{
    tess_fx_box( pcx - r, pcy - r, r * 2.0f, r * 2.0f,
                 r, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                 0, 0, 1, 1, 0, abgr, NULL );
}

/*==============================================================================================
    tess_fx_ngon -- a regular polygon (GUI_FX_NGON) in one quad.

    The polyline fan this replaces sampled up to 64 perimeter points and strung them through the
    ribbon; the field resolves the exact boundary at any size, and a stroked form is the same
    quad under GUI_OP_BAND -- set by the replay case before this runs, the ambient-ops rule every
    shape follows.  The record's row [2] re-partitions for the field: r_tl is the corner
    rounding, r_tr the side count (gui.h).
==============================================================================================*/

static void
tess_fx_ngon( f32 pcx, f32 pcy, f32 r, u32 sides, f32 rot, f32 rounding,
              f32 border, u32 abgr )
{
    if ( r <= 0.0f )
        return;

    s_tess.cur_prim.field   = (u32)GUI_FX_NGON;
    s_tess.cur_prim.r_tl    = rounding;
    s_tess.cur_prim.r_tr    = (f32)sides;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & GUI_OP_BAND ) ? border : 0.0f;
    s_tess.cur_rot_c = cosf( rot );
    s_tess.cur_rot_s = sinf( rot );

    /* The circumcircle covering under SKIRT: r on both axes, grown by the pad the vertex stage
       derives from the feather.  Rotation-safe -- a rotated square covering of a circle is
       still a covering. */
    s_tess.cur_ops |= GUI_OP_SELF;
    tess_quad_push( pcx, pcy, r, r, GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr,
                    GUI_GLYPH_ID_NONE );
}

/*==============================================================================================
    tess_round_rect_ex -- a fill whose four corners have four different radii.

    The tab, the notch, the asymmetric card: shapes that used to walk a per-corner perimeter (up to
    72 sampled points) and fan it into as many separate TRIANGLE commands, with a polygonal boundary
    and no antialiasing at all.  Here it is the ONE quad record a uniform rounded rect costs, and
    the boundary is exact, because all four radii ride the record and the fragment picks the one
    its own quadrant wants -- the radii are data, not geometry.

        perimeter fan, 4 rounded corners   ~70 verts, 62 draw commands, aliased
        the field                          1 record,   1 draw command,   antialiased

    The RAMP rides the same record.  A linear one could be carried by the four corners instead --
    colour is affine along the axis and so is interpolation -- but only a linear one, and only
    approximately: the corners it would be evaluated at are the FALLOFF SKIRT's, a pixel or more
    outside the shape, so the ramp arrives stretched by however wide the skirt is.  Resolved in the
    fragment it spans the shape exactly, and the two ramps a rectangle's corners cannot describe at
    all -- radial and conic -- cost the same one branch.
==============================================================================================*/

static void
tess_round_rect_ex( f32 x, f32 y, f32 w, f32 h,
                    f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather,
                    u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind, f32 grad_mid )
{
    /* Corner order: top-left, top-right, bottom-right, bottom-left -- the order gui_cmd_t
       .round_rect declares its radii in, and the order the record's r_tl/r_tr/r_br/r_bl carry
       them, which is what the fragment indexes by the sign of its own position.  feather below
       the standard AA band clamps up -- 0 means "crisp", never "hard-edged". */
    const f32 r4[ 4 ] = { rtl, rtr, rbr, rbl };

    /* Equal endpoints ARE a flat fill, so the op is left off rather than special-cased: a ramp
       between one colour and itself is that colour, and the fragment should not pay for it. */
    tess_fx_aux_t aux = { 0 };
    if ( col_b != abgr )
    {
        s_tess.cur_ops |= GUI_OP_GRAD;
        if ( grad_kind == (u32)GUI_GRAD_RADIAL ) s_tess.cur_ops |= GUI_OP_GRAD_RADIAL;
        if ( grad_kind == (u32)GUI_GRAD_CONIC  ) s_tess.cur_ops |= GUI_OP_GRAD_CONIC;
        aux.grad_col = col_b;
        aux.grad_ang = grad_ang;
        aux.grad_mid = grad_mid;
    }

    tess_fx_box_core( x, y, w, h, r4, ( feather > TESS_FX_AA ) ? feather : TESS_FX_AA,
                      0.0f, 0.0f, 0.0f, 0.0f,
                      0, 0, 1, 1, 0, abgr, &aux );
}

/*==============================================================================================
    tess_fx_arc -- a circular sector, stroked (ARC) or filled (PIE), in ONE quad.

    The last sampled curve in the library.  An arc used to be up to 66 points from cos/sin fed to
    the polyline ribbon (~130 vertices, and a visible polygon at small radii where sym_arc_segs
    gives a 10 px mark ten segments); a pie fanned the same points from the centre, which cost 65
    separate TRIANGLE commands -- 6% of the entire per-frame command budget for one shape.

        spinner, r = 24    ~90 verts,  1 draw cmd,  faceted, ribbon-AA
        pie,     r = 40    ~66 verts, 65 draw cmds, faceted, no AA
        the field          1 record,   1 draw cmd,  exact, antialiased

    A circular shape subtracts no half-extent: its effect coordinate is the raw signed offset from
    the centre, which is affine everywhere, so nothing has to fold at the vertex stage (gui.h).
    Keeping the sign is also the only reason an arc is expressible at all -- |p| would erase the
    angle.

    What the CPU does here is the per-shape work the fragment must not repeat: rotate the coordinate
    frame so the sector's bisector points +y.  That turns two absolute angles into one aperture (the
    shape is then symmetric about local x = 0, which the fragment folds itself) and it is paid once
    per record instead of once per pixel.  The matrix is a reflection and its own inverse, so the
    same two lines map local -> world here as map world -> local conceptually.

    The quad is the sector's bounding box IN THAT LOCAL FRAME, not the circle's: a 90-degree arc
    covers about a quarter of the disc's area, so the fragment cost tracks the shape rather than the
    circle it belongs to.  A full turn is not a sector at all and routes to the exact ring / disc
    primitives instead -- cheaper, and it sidesteps the aperture = pi degenerate.
==============================================================================================*/

#define TESS_PI       3.14159265358979f
#define TESS_HALF_PI  1.57079632679490f
#define TESS_TAU      6.28318530717959f

/* `mode` is GUI_FX_ARC, GUI_FX_PIE, or GUI_FX_ARC_GRAD, which additionally carries its second
   colour in (uvx, uvy) -- the pair the fragment recovers from the quad's flat uv word (gui.h).
   Every sector is self-sampled (GUI_OP_SELF): the fragment never reads a texel, ARC/PIE included,
   so the atlas index the quad carries is only there to keep the bound slot valid.
   A non-zero `dash_turns` dashes the sector through GUI_OP_DASH, in period-turns and on-duty. */
static void
tess_fx_arc( f32 pcx, f32 pcy, f32 r, f32 thickness, f32 a0, f32 a1,
             gui_fx_mode_t mode, f32 uvx, f32 uvy, f32 dash_turns, f32 dash_duty,
             f32 spin_rate, f32 spin_phase, u32 curve, f32 curve_param, u32 abgr )
{
    if ( r <= 0.0f )
        return;

    bool pie = ( mode == GUI_FX_PIE );

    /* Normalize the sweep so the bisector/aperture split below is always well formed.  A reversed
       range is the same sector drawn the other way round, which for a symmetric shape is the same
       sector.  (The gradient is NOT symmetric; its emit side pre-normalizes and swaps the colours,
       so by here every reversed range really is harmless.) */
    f32 sweep = a1 - a0;
    if ( sweep < 0.0f ) { f32 t = a0; a0 = a1; a1 = t; sweep = -sweep; }
    if ( sweep <= 0.0f )
        return;
    if ( sweep > TESS_TAU ) { sweep = TESS_TAU; a1 = a0 + TESS_TAU; }

    /* A full turn is not a sector, and the exact primitives are cheaper: a PIE is a disc, and an
       ARC is a closed ring, which is a BOX under GUI_OP_BAND whose interior the band carves away
       -- worth real fragments on a large one.  It is reachable: draw_progress_arc at 100% is
       exactly a full sweep.  The reroute used to be gated on the band fitting the packed `border`
       field, so a thick ring fell through to the sector formula (exact at aperture pi, it merely
       rasterizes the hole); the record has no such ceiling, so every full-turn ring takes the
       cheaper path now.
       A dashed or gradient sector never reroutes: the dash cut and the sweep both measure against
       the sector's own frame, which the exact ring does not build -- and at aperture pi the sector
       formula serves them exactly, so a closed dashed ring is this same one quad. */
    if ( sweep >= TESS_TAU && dash_turns <= 0.0f
      && ( mode == GUI_FX_ARC || mode == GUI_FX_PIE ) )
    {
        if ( pie )
        {
            tess_circle_filled( pcx, pcy, r, abgr );
            return;
        }
        /* The same shape draw_circle's unfilled path asks for, measured from the OUTER boundary
           inward -- so the band still straddles r. */
        f32 outer = r + thickness * 0.5f;
        s_tess.cur_ops |= GUI_OP_BAND;
        tess_fx_box( pcx - outer, pcy - outer, outer * 2.0f, outer * 2.0f,
                     outer, TESS_FX_AA, thickness, 0.0f, 0.0f, 0.0f,
                     0, 0, 1, 1, 0, abgr, NULL );
        return;
    }

    f32 ra = r;
    f32 rb = pie ? 0.0f : thickness * 0.5f;
    if ( rb < 0.0f ) rb = 0.0f;

    f32 am = ( a0 + a1 ) * 0.5f;          /* the bisector, which becomes local +y */
    f32 ap = sweep * 0.5f;                /* the half-aperture measured from it   */
    f32 sm = sinf( am ), cm = cosf( am );
    f32 sa = sinf( ap ), ca = cosf( ap );

    /* The sector's bounding box in local space.  x is bounded by the widest point of the sweep
       (sin saturates at 1 once the aperture passes a quarter turn) plus the tube; y runs from the
       far edge of the sweep up to the bisector's own rim.  A PIE also contains its centre, which a
       narrow sweep's box would otherwise sit entirely above. */
    f32 pad  = TESS_FX_AA * 0.5f + 1.0f;
    f32 xext = ( ( ap >= TESS_HALF_PI ) ? ra : ra * sa ) + rb + pad;
    f32 ymax = ra + rb + pad;
    f32 ymin = ra * ca - rb - pad;
    if ( pie && ymin > -pad )
        ymin = -pad;

    /* A SPINNING sector sweeps the whole disc over time while its retained quad never moves,
       so the quad must cover every orientation the fragment will ever resolve -- the disc's own
       bounding box, not the sector's. */
    if ( spin_rate != 0.0f )
    {
        xext = ra + rb + pad;
        ymax = ra + rb + pad;
        ymin = -ymax;
    }

    /* The record.  (cm, sm) is the sector's own frame -- the bisector direction the local
       coordinate above is expressed in -- so it goes where every other field's turn goes. */
    s_tess.cur_prim.field   = (u32)mode;
    s_tess.cur_rot_c = cm;
    s_tess.cur_rot_s = sm;
    s_tess.cur_prim.param_a = ra;
    s_tess.cur_prim.param_b = rb;
    s_tess.cur_prim.param_c = ap;

    /* GUI_OP_SPIN -- the whole frame (aperture, dashes, everything the record states) rotates at
       anim_rate turns/sec on pc.time.  The record is byte-identical every frame it runs, which is
       the point: the spinner joins the pulse in re-tessellating nothing. */
    if ( spin_rate != 0.0f )
    {
        s_tess.cur_ops |= GUI_OP_SPIN;
        s_tess.cur_prim.anim_rate  = spin_rate;
        s_tess.cur_phase = spin_phase;
    }

    /* A DASHED sector is the plain sector plus GUI_OP_DASH now -- the sector states arc-length as
       its boundary coordinate, which is the one axis the dash op cuts on whatever the shape.  The
       caller still speaks in turns, so convert once here: a period of `uvx` turns is that fraction
       of the full circumference at this radius.  The snap keeps whole cycles around the sweep, so
       a closed dashed ring meets itself exactly as the retired ARC_DASH field arranged. */
    if ( dash_turns > 0.0f )
    {
        f32 arc_len = sweep * ra;
        f32 period  = dash_turns * TESS_TAU * ra;
        f32 cycles  = ( period > 0.0f ) ? arc_len / period : 0.0f;
        if ( cycles >= 1.0f )
            period = arc_len / (f32)(i32)( cycles + 0.5f );

        s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_DASH;
        s_tess.cur_prim.dash_period = period;
        s_tess.cur_prim.dash_duty   = dash_duty;

        /* A spinning sector already carries its dashes around with it -- the boundary coordinate
           they are cut on is measured in the frame the spin rotates -- so the pattern is pinned
           to the shape rather than scrolled a second time along it.  A static sector has no
           rotation to ride, so there the clock is what moves the pattern at all. */
        s_tess.cur_prim.dash_scroll = ( spin_rate != 0.0f ) ? 0.0f : 1.0f;
    }

    /* The curve belongs to the clock, not to any one op that reads it, so it lands once here for
       whichever of the two the sector turned on. */
    if ( s_tess.cur_ops & ( GUI_OP_SPIN | GUI_OP_DASH ) )
    {
        s_tess.cur_prim.anim_curve = curve;
        s_tess.cur_prim.anim_param = curve_param;
    }

    /* ARC_GRAD still carries its second colour as a packed pair, which the self-sampled bit
       announces: the fragment forces coverage to 1 and never reads the texel, so the white texel
       is not needed and the atlas stays bound only to keep the index valid. */
    if ( mode == GUI_FX_ARC_GRAD )
    {
        s_tess.cur_ops |= GUI_OP_SELF;

        u32 bu = (u32)( uvx * 65535.0f + 0.5f );
        u32 bv = (u32)( uvy * 65535.0f + 0.5f );
        s_tess.cur_prim.col_b = ( bu & 0xFFu ) | ( ( bu >> 8 ) << 8 )
                              | ( ( bv & 0xFFu ) << 16 ) | ( ( bv >> 8 ) << 24 );
    }

    static const f32 lsx[ 4 ] = { -1.0f, 1.0f, 1.0f, -1.0f };
    static const u32 lsy[ 4 ] = {  0u,   0u,   1u,   1u   };

    /* The sector's frame is a REFLECTION, which the vertex stage's rotation cannot reproduce,
       so the covering goes out under the BBOX rule -- axis-aligned half-extents that reach
       every reflected corner from the SHAPE centre (the fragment's rotation origin).  The fold
       to the centre wastes the asymmetric slack a tight bbox would trim; sectors are small. */
    s_tess.cur_ops |= GUI_OP_SELF;
    f32 bhx = 0.0f, bhy = 0.0f;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 lx = lsx[ i ] * xext;
        f32 ly = lsy[ i ] ? ymax : ymin;
        f32 rx = -sm * lx + cm * ly;
        f32 ry =  cm * lx + sm * ly;
        if ( fabsf( rx ) > bhx ) bhx = fabsf( rx );
        if ( fabsf( ry ) > bhy ) bhy = fabsf( ry );
    }
    tess_quad_push( pcx, pcy, bhx, bhy, GUI_QUAD_RULE_BBOX,
                    0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}

/*==============================================================================================
    tess_checker / tess_grid -- the framebuffer-tiling pattern quads.

    ONE quad each: the fragment computes the pattern from gl_FragCoord / SV_Position, not from
    the effect coordinate, and the reason is precision where these shapes actually live.  A
    backdrop is the one shape that reaches fullscreen, and there the HALF2 coordinate's ulp is a
    full pixel at the far corners -- a fine lattice line would land half a pixel wrong and blur.
    The rasterizer's own pixel coordinate is exact everywhere at any size.  (It also means the
    pattern assumes the pixel-space ortho mvp, which is the only mvp this pipeline has.)

    The CPU's share is the ANCHOR: quantize the cell pitch EXACTLY as the packed word carries it
    (1/4 px), then derive the phase against that quantized pitch -- deriving it against the raw
    pitch would let phase and pitch disagree by up to 1/8 px per cell, which walks the pattern
    off its anchor across a wide panel.  The checker's phase is a fraction of the TWO-cell
    colour period (one cell of phase would swap the colours); the grid's is a fraction of one
    cell.  Both ride the style record's pattern row (gui_prim_t, pat_phase).
==============================================================================================*/

/* A pattern quad, with the shape it lands in.  A zero radius is the plain rectangle the pattern
   used to be able to be and nothing else: one bare quad, no field, no falloff.  A non-zero radius
   routes the same record through the BOX field, so the lattice or the chequerboard is cut to a
   rounded boundary -- the whole point of these being ops rather than fields.  Either way it is ONE
   quad and one style. */
static void
tess_pattern_push( f32 x, f32 y, f32 w, f32 h, f32 rounding, u32 abgr )
{
    if ( rounding > 0.0f )
    {
        tess_fx_box( x, y, w, h, rounding, TESS_FX_AA, 0.0f,
                     0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, abgr, NULL );
        return;
    }
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}

static void
tess_checker( f32 x, f32 y, f32 w, f32 h, f32 cell, f32 rounding, u32 col_a, u32 col_b )
{
    /* Snap like tess_rect_filled: the pattern anchors at the box origin, so the box must land
       where the plain fill under it does. */
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_clamp_cell( cell );

    f32 period = 2.0f * cell;
    f32 phx    = ( x - period * floorf( x / period ) ) / period;
    f32 phy    = ( y - period * floorf( y / period ) ) / period;

    s_tess.cur_prim.pat_cell  = cell;
    s_tess.cur_prim.pat_phase = gui_uv_pack( phx, phy );
    s_tess.cur_prim.pat_col   = col_b;
    s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_CHECKER;

    tess_pattern_push( x, y, w, h, rounding, col_a );
}

static void
tess_grid( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 cell, f32 thickness,
           f32 angle, f32 rounding, bool stripes, u32 abgr )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_clamp_cell( cell );

    /* WRAP rather than clamp: a lattice at `angle` and at angle + pi are the same lattice, so an
       animated rotation must roll over rather than stick at pi, which is what a clamp would do.
       The wrapped value is used for BOTH the packed word and the phase below -- the fragment
       rotates the pixel coordinate by exactly this angle, so a disagreement would slide the
       pattern off its anchor. */
    angle -= TESS_PI * floorf( angle / TESS_PI );

    /* The lattice anchor, mod the quantized pitch.  (ox, oy) is a screen-space content origin
       and may be anywhere (a panned canvas sends large negatives); only its residue matters.
       The anchor is rotated INTO lattice space first, because that is the space the fragment
       does its mod in -- rotation is linear, so R(px - o) is R(px) - R(o), and the phase is the
       residue of R(o).  Taking the residue before the rotation would anchor the wrong point. */
    f32 acs = cosf( angle ), asn = sinf( angle );
    f32 rx  =  ox * acs + oy * asn;
    f32 ry  = -ox * asn + oy * acs;

    f32 phx = ( rx - cell * floorf( rx / cell ) ) / cell;
    f32 phy = ( ry - cell * floorf( ry / cell ) ) / cell;

    s_tess.cur_prim.pat_cell  = cell;
    s_tess.cur_prim.pat_size  = thickness;
    s_tess.cur_prim.pat_angle = angle;
    s_tess.cur_prim.pat_phase = gui_uv_pack( phx, phy );
    if ( stripes )
        s_tess.cur_ops |= GUI_OP_STRIPES;
    s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_GRID;

    tess_pattern_push( x, y, w, h, rounding, abgr );
}

/* The ambient TEXT_EDGE, straight onto the record: a band `width` px outside the glyph boundary,
   painted in `abgr`.  A zero width is no edge at all and leaves the field NONE, which is what
   every plain run wants. */
static void
tess_text_edge_prim( f32 width, u32 abgr )
{
    if ( width <= 0.0f )
        return;

    s_tess.cur_prim.pat_size = width;
    s_tess.cur_prim.pat_col  = abgr;
    s_tess.cur_ops |= GUI_OP_TEXT_EDGE;
}

/* Tessellate a glyph run from the font atlas into s_tess, hard-clipped to the horizontal pixel
   window [clip_x0, clip_x1].  Glyphs fully outside the window are skipped; glyphs fully inside emit
   whole; the (at most two) straddling glyphs are cut on a pixel boundary with their U remapped by
   the same fraction -- exact, since the glyph quad is an axis-aligned 1:1 atlas sample.  The window
   is monotonic with the left-to-right cursor, so interior glyphs pay only one compare: no clip math.
   The unclipped sentinel (clip_x1 >= GUI_TEXT_NO_CLIP) takes the original whole-run fast path. */
static void
tess_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    bool clipped = ( clip_x1 < GUI_TEXT_NO_CLIP );
    f32  cx      = x;

    /* Hoisted: the active font cannot change mid-run, and this carries the sampling model, so it is
       also what keeps a distance-field run in its own batch without the batcher knowing why. */
    u32  tex = font_tex();
    if ( tex == 0 )
        return;                       /* the font's atlas is not up yet -- nothing to sample */

    s_tess.cur_is_text = true;        /* every quad below is a character (glyph attribution) */
    s_tess.slot_text_runs++;

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        /* ID + placement in one record lookup; the atlas rect is the glyph table's now. */
        u32 gid;
        f32 ox, oy, gw, gh, advance;
        font_glyph_placed( cp, &gid, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
        {
            f32 gx0 = cx + ox;          /* glyph bitmap left/right in screen px */
            f32 gx1 = gx0 + gw;

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                /* Whole glyph (or no clipping): emit by table ID -- the hot interior path. */
                tess_rect_glyph( gx0, y + oy, gw, gh, gid, tex, abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                /* Straddler: cut to the window and walk U by the same fraction on each cut edge,
                   so the narrowed rect samples exactly the visible part of the glyph bitmap.  The
                   narrowed span is per-instance and has no table entry, so this one glyph pays the
                   second lookup and bakes its own rect -- at most two per run, at its ends. */
                f32 u0, v0, u1, v1, sox, soy, sgw, sgh, sadv;
                font_glyph( cp, &u0, &v0, &u1, &v1, &sox, &soy, &sgw, &sgh, &sadv );

                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                if ( nx0 < clip_x0 )    /* left edge cut  */
                {
                    nu0 = u0 + du * ( ( clip_x0 - gx0 ) / gw );
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )    /* right edge cut */
                {
                    nu1 = u0 + du * ( ( clip_x1 - gx0 ) / gw );
                    nx1 = clip_x1;
                }
                tess_rect_filled( nx0, y + oy, nx1 - nx0, gh, nu0, v0, nu1, v1, tex, abgr );
            }
            /* else: glyph wholly outside the window -- drop it. */
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )   /* cursor past the window: nothing further is visible */
            break;
    }

    s_tess.cur_is_text = false;
}

/* Same walk as tess_text_n, but each glyph decode + atlas lookup feeds TWO quads: the shadow
   copy (offset dx, dy; shadow_abgr) then the main glyph, in that order so the main glyph's
   antialiased edge composites over the shadow rather than under it.  Whichever of the pair a
   glyph resolves to (whole-glyph table id vs. cut-and-remapped rect) is decided once and used for
   both copies -- the shadow is never independently clip-tested, so the pair always lives or dies
   together instead of a shadow surviving a main glyph the clip window dropped (or the reverse). */
static void
tess_text_shadow_n( f32 x, f32 y, u32 abgr, u32 shadow_abgr, f32 dx, f32 dy,
                     const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    bool clipped = ( clip_x1 < GUI_TEXT_NO_CLIP );
    f32  cx      = x;

    u32 tex = font_tex();
    if ( tex == 0 )
        return;

    s_tess.cur_is_text = true;
    s_tess.slot_text_runs++;

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        u32 gid;
        f32 ox, oy, gw, gh, advance;
        font_glyph_placed( cp, &gid, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
        {
            f32 gx0 = cx + ox;
            f32 gx1 = gx0 + gw;

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                tess_rect_glyph( gx0 + dx, y + oy + dy, gw, gh, gid, tex, shadow_abgr );
                tess_rect_glyph( gx0,      y + oy,      gw, gh, gid, tex, abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                f32 u0, v0, u1, v1, sox, soy, sgw, sgh, sadv;
                font_glyph( cp, &u0, &v0, &u1, &v1, &sox, &soy, &sgw, &sgh, &sadv );

                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                if ( nx0 < clip_x0 )
                {
                    nu0 = u0 + du * ( ( clip_x0 - gx0 ) / gw );
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )
                {
                    nu1 = u0 + du * ( ( clip_x1 - gx0 ) / gw );
                    nx1 = clip_x1;
                }
                tess_rect_filled( nx0 + dx, y + oy + dy, nx1 - nx0, gh, nu0, v0, nu1, v1, tex,
                                   shadow_abgr );
                tess_rect_filled( nx0,      y + oy,      nx1 - nx0, gh, nu0, v0, nu1, v1, tex,
                                   abgr );
            }
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )
            break;
    }

    s_tess.cur_is_text = false;
}

/* One textured quad placed by an affine map: the local rect (lx, ly, lw, lh) is rotated by the
   prebuilt (cs, sn) and translated to the run origin (px, py) -- centre mapped through the
   transform, half-extents stored true, the style's rot pair doing the turn in the vertex stage.
   One style per (angle x scale) run: every glyph of a transformed run shares it.
   One thing this does NOT do: SNAP.  tess_rect_filled floors the origin to the pixel grid so
   straight edges stay crisp, which is right for chrome and wrong here twice over.  Snapping only
   the origin of a rotated quad moves the whole shape without straightening anything, and
   snapping a scaled run's per-glyph origins quantizes the advances -- the pen drifts by up to
   half a pixel per glyph and the word visibly breathes as the scale animates.  A transformed run
   is sub-pixel by nature; the distance field is what makes that legible (gui.h, GUI_TEX_SDF). */
static void
tess_quad_xf( f32 px, f32 py, f32 cs, f32 sn,
              f32 lx, f32 ly, f32 lw, f32 lh,
              f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr )
{
    f32 ccx = lx + lw * 0.5f, ccy = ly + lh * 0.5f;
    s_tess.cur_rot_c = cs;
    s_tess.cur_rot_s = sn;
    tess_quad_push( px + ccx * cs - ccy * sn, py + ccx * sn + ccy * cs,
                    lw * 0.5f, lh * 0.5f, GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr,
                    GUI_GLYPH_ID_NONE );
}

/* tess_quad_xf for a glyph: the same affine placement, the atlas rect named by table ID.  A
   transformed run is never cut mid-glyph (it has no window-relative pen to test), so every glyph
   of one takes this path. */
static void
tess_glyph_xf( f32 px, f32 py, f32 cs, f32 sn,
               f32 lx, f32 ly, f32 lw, f32 lh, u32 glyph_id, u32 tex_idx, u32 abgr )
{
    f32 ccx = lx + lw * 0.5f, ccy = ly + lh * 0.5f;
    s_tess.cur_rot_c = cs;
    s_tess.cur_rot_s = sn;
    tess_quad_push( px + ccx * cs - ccy * sn, py + ccx * sn + ccy * cs,
                    lw * 0.5f, lh * 0.5f, GUI_QUAD_RULE_EXACT,
                    0u, 0u, tex_idx, abgr, glyph_id );
}

/* Tessellate a glyph run under a uniform scale and a rotation about its origin (the text_xf
   command).  The run is laid out in its OWN space -- pen at 0, the font's unscaled advances -- and
   the whole of it is mapped once per glyph quad, so the transform never accumulates: 200 glyphs in
   and the pen is still exactly `sum(advance) * scale` from the origin along the rotated axis.

   Nothing about the ATLAS side changes: the same glyph-table IDs, the same tex, the same batch key
   as the 1:1 path, so a rotated run merges into the very same draw call as the upright text beside
   it as long as both are in the same font.  What makes it LOOK right rather than merely be placed
   right is the sampling model -- a coverage font is point-sampled and will show its texels here,
   while a distance-field font resolves its edge in the fragment from a screen-space derivative and
   is therefore indifferent to both the scale and the angle. */
static void
tess_text_xf( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 scale, f32 rot )
{
    u32 tex = font_tex();
    if ( tex == 0 || scale <= 0.0f )
        return;

    s_tess.cur_is_text = true;        /* every quad below is a character (glyph attribution) */
    s_tess.slot_text_runs++;

    f32 cs = cosf( rot ), sn = sinf( rot );
    f32 pen = 0.0f;                      /* run-local, UNSCALED: scale is applied at the map */

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        u32 gid;
        f32 ox, oy, gw, gh, advance;
        font_glyph_placed( cp, &gid, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
            tess_glyph_xf( x, y, cs, sn,
                           ( pen + ox ) * scale, oy * scale, gw * scale, gh * scale,
                           gid, tex, abgr );

        pen += advance;
    }

    s_tess.cur_is_text = false;
}

/* Tessellate a dashed / dotted line as one oriented textured quad sampling the atlas dash row.
   U spans 0..len/period so the row tiles along the line under REPEAT-U addressing; V selects the
   baked row whose on-fraction is closest to `duty`.  O(1) geometry regardless of line length --
   the per-dash quad explosion (which used to exhaust the command list) is gone. */
static void
tess_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 period, f32 duty, u32 abgr )
{
    if ( thickness <= 0.0f || period <= 0.0f )
        return;
    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;
    f32 inv  = 1.0f / len;
    f32 ux   = dx * inv, uy = dy * inv;          /* unit vector along the line  */
    f32 half = thickness * 0.5f;
    f32 umax = len / period;                     /* number of tiled periods -> U span */
    f32 vv   = res_atlas_dash_v( duty );

    /* U runs 0..1 in the quad's uv lanes and is multiplied back up to `umax` periods by the
       fragment: the packed UV cannot hold a coordinate past 1, and the sampler's REPEAT-U is
       what tiles the atlas dash row (gui.h, GUI_OP_TILE_U).  The line's direction is the quad's
       turn -- a style per direction; dashed lines are rare enough that the dedup loss is noise. */
    s_tess.cur_prim.pat_size = umax;
    s_tess.cur_ops |= GUI_OP_TILE_U;
    s_tess.cur_rot_c = ux;
    s_tess.cur_rot_s = uy;
    tess_quad_push( ( x0 + x1 ) * 0.5f, ( y0 + y1 ) * 0.5f, len * 0.5f, half,
                    GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( 0.0f, vv ), gui_uv_pack( 1.0f, vv ),
                    res_atlas_idx(), abgr, GUI_GLYPH_ID_NONE );
}

/*==============================================================================================
    tess_stroke_poly_aa -- the polyline as a CAPSULE CHAIN: one SEG quad per segment, endpoints
    offset along the miter normals so alignment matches the old ribbon stroker.  Every quad
    resolves to ONE shared style (the direction rides the quad, tess_fx_segment), so a long path
    costs segments, not records.  Joins are the round caps overlapping, which composites darker
    on a translucent stroke -- the accepted trade for retiring the miter ribbon.
    abgr is pre-baked (alpha folded in at emit time).  v2 / seg_normal / stroke_center_offset
    are defined in gui_emit_path.c (included before this file in the unity build).
==============================================================================================*/

static void tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 border,
                             u32 abgr );   /* the quad backend's polyline expansion, defined below */

static void
tess_stroke_poly_aa( const gui_vec2_t* pts, u32 n, f32 thickness, f32 center_off,
                     bool closed, u32 abgr )
{
    if ( n < 2 )
        return;
    if ( n > GUI_MAX_PATH_PTS )
         n = GUI_MAX_PATH_PTS;

    /* Sub-pixel coverage: hold a 1px footprint, fade peak alpha by the requested thickness.
       Done here rather than left to tess_fx_segment's own clamp so the fold below is already
       final -- the segment's re-fold is then a no-op and every segment shares one colour. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )
    {
        a_scale   = thickness < 0.0f ? 0.0f : thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    u32 seg = closed ? n : n - 1;

    /* Per-point miter normal; static avoids an 8K+ stack frame. Single-threaded. */
    static gui_vec2_t nrm[ GUI_MAX_PATH_PTS ];
    for ( u32 i = 0; i < n; ++i )
    {
        gui_vec2_t n0, n1;
        if ( closed )
        {
            n0 = seg_normal( pts[ ( i + n - 1 ) % n ], pts[ i ] );
            n1 = seg_normal( pts[ i ], pts[ ( i + 1 ) % n ] );
        }
        else
        {
            n0 = ( i > 0 )     ? seg_normal( pts[ i - 1 ], pts[ i ] ) : v2( 0.0f, 0.0f );
            n1 = ( i < n - 1 ) ? seg_normal( pts[ i ], pts[ i + 1 ] ) : v2( 0.0f, 0.0f );
        }

        if ( !closed && i == 0 )          nrm[ i ] = n1;
        else if ( !closed && i == n - 1 ) nrm[ i ] = n0;
        else
        {
            gui_vec2_t dm = v2( ( n0.x + n1.x ) * 0.5f, ( n0.y + n1.y ) * 0.5f );
            f32 d2 = dm.x * dm.x + dm.y * dm.y;
            if ( d2 > 1e-6f )
            {
                f32 inv = 1.0f / d2;
                if ( inv > 100.0f ) inv = 100.0f;   /* miter limit */
                dm.x *= inv; dm.y *= inv;
                nrm[ i ] = dm;
            }
            else { nrm[ i ] = n1; }
        }
    }

    for ( u32 s2 = 0; s2 < seg; ++s2 )
    {
        u32 j = ( s2 + 1u ) % n;
        tess_fx_segment( pts[ s2 ].x + nrm[ s2 ].x * center_off,
                         pts[ s2 ].y + nrm[ s2 ].y * center_off,
                         pts[ j ].x + nrm[ j ].x * center_off,
                         pts[ j ].y + nrm[ j ].y * center_off,
                         thickness, 0.0f, col );
    }
}

/*==============================================================================================
    tess_fx_segment -- one line segment as a CAPSULE distance field: the distance from a point
    to a segment, minus the half-thickness.  One quad under the CAPSULE rule, an edge that is
    correct at any angle, and round caps that cost nothing because they ARE the field.

    Round caps extend half a thickness past each endpoint.  On a polyline (the capsule chain,
    tess_stroke_poly_aa) they are also the JOINS: neighbouring capsules overlap there, which
    composites darker on a translucent stroke -- the accepted cost of per-segment records.

    Axis-aligned single lines never come here: gui_draw_line routes them through a grid-snapped
    rect at EMIT (stroke_axis_aligned_rect, gui_emit_path.c), which is crisper than any field
    since a horizontal edge has nothing to antialias.
==============================================================================================*/

static void
tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 border, u32 abgr )
{
    if ( thickness <= 0.0f )
        return;

    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;

    /* Sub-pixel coverage, matched to the ribbon stroker rather than left to the field: hold a 1 px
       footprint and fade peak alpha.  The field would happily render a 0.4 px capsule, but it would
       weigh a hairline differently than every other line in the library, and consistency across the
       two paths is worth more here than the extra correctness. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )                 /* thickness > 0 by the guard above */
    {
        a_scale   = thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    f32 inv = 1.0f / len;
    f32 ux  = dx * inv, uy = dy * inv;      /* unit vector along the segment  */
    f32 r   = thickness * 0.5f;             /* the capsule radius             */
    f32 hl  = len * 0.5f;                   /* half-length: what q.x subtracts */
    f32 mx  = ( x0 + x1 ) * 0.5f, my = ( y0 + y1 ) * 0.5f;

    /* The style states the capsule's radius in the corner-radius lane.  The DIRECTION is the
       quad's own turn, like every other shape's -- which is exactly why every segment of every
       stroke at one thickness shares ONE style: the turn was never in the style to forfeit.  The
       half-length rides the quad (rect.z). */
    s_tess.cur_prim.field   = (u32)GUI_FX_SEG;
    s_tess.cur_prim.r_tl    = r;
    s_tess.cur_prim.feather = TESS_FX_AA;

    /* A HOLLOW capsule is the same field under GUI_OP_BAND -- the op that makes a rounded outline
       out of a filled box, reaching this shape because an op modifies whatever field arrived.  A
       border at or past the radius has no interior left to remove, so it stays filled rather than
       inverting into one. */
    if ( border > 0.0f && border < r )
    {
        s_tess.cur_ops         |= GUI_OP_BAND;
        s_tess.cur_prim.border  = border;
    }

    s_tess.cur_ops  |= GUI_OP_SELF;
    s_tess.cur_rot_c = ux;
    s_tess.cur_rot_s = uy;
    tess_quad_push( mx, my, hl, r, GUI_QUAD_RULE_CAPSULE, 0, 0,
                    res_atlas_idx(), col, GUI_GLYPH_ID_NONE );
}

/* Volatile-widget seam (render/pipeline/gui_build_volatile.c, included right after this file in
   the gui_render.c unity build).  tess_dispatch calls volatile_range_close once a tagged command
   RANGE's quads / style records / GPU commands are fully written; it records the block's slot-relative
   position, reserves padded headroom past the live geometry (advancing this file's write heads),
   and stamps the slot tessellation generation.  s_volatile_patching is declared up with s_tess
   (tess_quad_push reads it to keep a patch out of the glyph counters) and set by volatile_patch around its scratch re-tessellation so the range tracking below
   stays inert during a patch -- a patch must never look like a fresh capture. */
static void volatile_range_close( gui_id_t id, u32 vb_open, u32 pb_open, u32 cmd_open );

/* Tessellate one frame's semantic command list into s_tess geometry.

   `order` is a permutation of [0,count): the window's visible commands in emission order (built
   by cache_tess_window; clip-empty commands are already dropped).  Nothing about clips shapes it
   any more -- the clip rides the vertex (the clip band) and cannot cut a draw call.
   `win` is the window being tessellated (informational; volatile rows already know their window
   from emit-time stamping).

   The FONT is activated per TEXT COMMAND, from the command's own font id, and only by the two cases
   that read glyphs.  It used to arrive as a `fonts[]` array parallel to `order` -- one entry per
   ordered command, reconstructing a per-segment property after the clip sort had torn the segments
   apart -- and it used to be switched at the top of this loop for every command, text or not.
   Neither was needed: a fill, a line and a sprite never call font_glyph, and the font is per-command
   data now (gui.h).  Activating it changes which atlas the glyph lookups resolve from and nothing
   else; it does NOT split the GPU batch, since it only alters the texture tess_set_tex stamps onto
   the following quads' style records.  A bitmap label, an SDF heading and the fill behind them still go out as one
   draw call.  The active font is saved and restored so the BUILD phase leaves the global font state
   (used by the next frame's layout) untouched. */
static void
tess_dispatch( const gui_cmd_t* cmds, const u16* order, u32 count, gui_id_t win )
{
    u32 saved_font = font_active_id();
    u32 cur_font   = saved_font;

    /* Volatile-widget range tracking: cmd_volatile_id tags a contiguous RANGE of commands (not
       just one), so bracket [vb_open, ...) / [ib_open, ...) / [cmd_open, ...) while the tag stays
       the same and hand the finished range to volatile_range_close when it changes (or at the
       end).  force_new_cmd is raised when a range OPENS so the block's geometry lands in its own
       fresh GPU command(s), never merged with a neighbour's -- the block's elem_counts must stay
       independently rewritable by a later patch.  Tracking is inert during a patch's own scratch
       re-tessellation (s_volatile_patching). */
    gui_id_t open_vid = GUI_ID_NONE;
    u32      vb_open = 0, pb_open = 0, cmd_open = 0;
    (void)win;

    /* Attribute the records this pass commits to their window, and to this tessellation pass --
       the census counts a record's PASSES to show the cross-slot spread that slot-scoped dedup
       can never collapse. */
    PRIM_CENSUS_WINDOW( win, s_tess.slot_tess_gen );

    for ( u32 oi = 0; oi < count; ++oi )
    {
        u32              ci = order[ oi ];
        const gui_cmd_t* c  = &cmds[ ci ];

        gui_id_t vid = s_volatile_patching ? GUI_ID_NONE : s_draw.cmd_volatile_id[ ci ];
        if ( vid != open_vid )
        {
            if ( open_vid != GUI_ID_NONE )
                volatile_range_close( open_vid, vb_open, pb_open, cmd_open );
            open_vid = vid;

            /* The dedup floor rises at BOTH sides of a volatile boundary.  Entering: the block's
               first primitive must append a record INSIDE its own range instead of reusing the
               window's preceding one -- a reused record would sit outside the reservation the
               patch is allowed to rewrite, and the patch (which always starts cold) would then
               disagree with the capture about how many it needs.  Leaving: the block's records
               ARE that rewritable reservation, so a later command deduping onto one would be
               corrupted by the next patch. */
            s_tess.prim_dedup_floor = s_tess.prim_count;
            tess_fx_page_reset();

            if ( vid != GUI_ID_NONE )
            {
                s_tess.force_new_cmd = true;   /* block owns its GPU commands from the first primitive */
                vb_open  = s_tess.quad_count;
                pb_open  = s_tess.prim_count;
                cmd_open = s_tess.cmd_count;
            }
        }

        s_tess.cur_clip       = s_draw.clip_table[ c->clip_idx ];
        s_tess.cur_clip_local = tess_clip_local( c->clip_idx );
        s_tess.cur_vp         = c->vp;

        /* The op word is ambient over ONE command and cleared here, so a case that sets it
           cannot leak the effect onto the next primitive.  That containment is the whole reason
           it can be ambient at all -- it lets an outline reach every glyph of a run without
           threading a parameter through tess_rect_filled, which every fill in the library
           shares. */
        s_tess.cur_ops        = 0u;
        s_tess.cur_corner_pow = 0.0f;
        s_tess.cur_col_border = 0u;
        s_tess.cur_rot_c      = 1.0f;
        s_tess.cur_rot_s      = 0.0f;
        s_tess.cur_phase      = 0.0f;

        /* The record is cleared WHOLE, and it matters for two reasons: a leftover rect or radius
           does not merely paint wrong, it defeats the memo -- a run of flat fills carrying stale
           geometry would take one record each. */
        s_tess.cur_prim = ( gui_prim_t ){ 0 };

        switch ( c->type )
        {
            /* A square rect keeps the one-quad fast path: it is pixel-aligned by construction, so
               there is no edge for an SDF to resolve and nothing to gain.  Rounding is what turns
               it into a surface -- and routing the TEXTURED case through as well is what finally
               lets a rounded quad carry an image, which the arc fan never could. */
            case GUI_CMD_RECT_FILLED:
                if ( c->rect.rounding > 0.0f )
                {
                    s_tess.cur_corner_pow = c->rect.corner_pow;
                    tess_fx_box( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                 c->rect.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                                 c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                 c->rect.tex_idx, c->rect.abgr, NULL );
                }
                else
                    tess_rect_filled( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                      c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                      c->rect.tex_idx, c->rect.abgr );
                break;

            /* The band measures from the OUTER boundary inward, matching the square path's
               INSIDE band (and the closed AA stroke this replaced). */
            case GUI_CMD_RECT_OUTLINE:
                if ( c->rect_outline.rounding > 0.0f )
                {
                    s_tess.cur_ops       |= GUI_OP_BAND;
                    s_tess.cur_corner_pow = c->rect_outline.corner_pow;
                    tess_fx_box( c->rect_outline.x, c->rect_outline.y,
                                 c->rect_outline.w, c->rect_outline.h,
                                 c->rect_outline.rounding, TESS_FX_AA, c->rect_outline.t,
                                 0.0f, 0.0f, 0.0f,
                                 0, 0, 1, 1, 0, c->rect_outline.abgr, NULL );
                }
                else
                    tess_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                       c->rect_outline.w, c->rect_outline.h,
                                       c->rect_outline.t, c->rect_outline.abgr );
                break;

            /* Body + border in one surface.  A square frame runs the field with feather 0 -- a
               hard cut on the snapped boundary, matching the crisp edges the fill + four-rail
               pair drew -- and a rounded one takes the standard AA band. */
            case GUI_CMD_FRAME:
                s_tess.cur_ops       |= GUI_OP_FRAME;
                s_tess.cur_corner_pow = c->frame.corner_pow;
                s_tess.cur_col_border = c->frame.col_border;   /* rides the quad, not the style */
                tess_fx_box( c->frame.x, c->frame.y, c->frame.w, c->frame.h,
                             c->frame.rounding,
                             ( c->frame.rounding > 0.0f ) ? TESS_FX_AA : 0.0f,
                             c->frame.t, 0.0f, 0.0f, 0.0f,
                             0, 0, 1, 1, 0, c->frame.abgr, NULL );
                break;

            /* The parameterized surface: a shadow is the wide feather (what used to be six
               stacked rects pretending to be a gaussian is the exact same falloff the corners
               use, only spread out), a pulse the shader-clock word -- geometrically a plain
               rounded fill whose record is correct for every frame it runs, so the retained
               slot never invalidates and the breathing costs no re-tessellation. */
            case GUI_CMD_FX_BOX:
                /* All four of these are the one GUI_FX_BOX mode; the variant and the rate pick
                   which ops ride the tex word.  They are INDEPENDENT flags rather than a choice of
                   one, which is what lets a cut or inset surface breathe -- as a mode number the
                   pulse had to displace whichever shape it was applied to.  Set before the
                   tessellator runs because the interior hole is sized from them (see `reach`). */
                if ( c->fx_box.variant == 1u )   s_tess.cur_ops |= GUI_OP_CUT;
                if ( c->fx_box.variant == 2u )   s_tess.cur_ops |= GUI_OP_INSET;
                if ( c->fx_box.variant == 3u )   s_tess.cur_ops |= GUI_OP_GLOW;
                if ( c->fx_box.rate    > 0.0f )  s_tess.cur_ops |= GUI_OP_PULSE;
                {
                    /* The cut boundary, for the DIRECTIONAL cast: the command states where the
                       shadow is drawn, and this says where the caster it belongs to sits relative
                       to it.  Zero for every other variant, and the aux is read only under the op
                       that owns it. */
                    tess_fx_aux_t aux = { 0 };
                    aux.cut_dx     = c->fx_box.cut_dx;
                    aux.cut_dy     = c->fx_box.cut_dy;
                    aux.anim_phase = c->fx_box.phase;
                    aux.anim_curve = c->fx_box.curve;
                    aux.anim_param = c->fx_box.curve_param;
                    s_tess.cur_corner_pow = c->fx_box.corner_pow;
                    tess_fx_box( c->fx_box.x, c->fx_box.y, c->fx_box.w, c->fx_box.h,
                                 c->fx_box.rounding, c->fx_box.feather, 0.0f,
                                 c->fx_box.rate, c->fx_box.depth, c->fx_box.rot,
                                 0, 0, 1, 1, 0, c->fx_box.abgr, &aux );
                }
                break;

            /* Four radii and a ramp -- and still one surface, one command and no batch split,
               exactly like the uniform fill it generalizes. */
            case GUI_CMD_ROUND_RECT_EX:
                s_tess.cur_corner_pow = c->round_rect.corner_pow;
                tess_round_rect_ex( c->round_rect.x, c->round_rect.y,
                                    c->round_rect.w, c->round_rect.h,
                                    c->round_rect.rtl, c->round_rect.rtr,
                                    c->round_rect.rbr, c->round_rect.rbl,
                                    c->round_rect.feather, c->round_rect.abgr,
                                    c->round_rect.col_b, c->round_rect.grad_ang,
                                    c->round_rect.grad_kind, c->round_rect.grad_mid );
                break;

            /* The sectors share their geometry and differ only in the field the fragment
               evaluates: round caps on a band, sharp radial edges on a wedge.  A dashed sector is
               no longer a field of its own -- it is this same ARC under GUI_OP_DASH. */
            case GUI_CMD_ARC:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, c->arc.thickness,
                             c->arc.a0, c->arc.a1, GUI_FX_ARC, 0.0f, 0.0f, 0.0f, 0.0f,
                             c->arc.spin_rate, c->arc.spin_phase,
                             c->arc.curve, c->arc.curve_param, c->arc.abgr );
                break;

            case GUI_CMD_PIE:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, 0.0f,
                             c->arc.a0, c->arc.a1, GUI_FX_PIE, 0.0f, 0.0f, 0.0f, 0.0f,
                             c->arc.spin_rate, c->arc.spin_phase,
                             c->arc.curve, c->arc.curve_param, c->arc.abgr );
                break;

            case GUI_CMD_ARC_DASH:
                tess_fx_arc( c->arc_dash.cx, c->arc_dash.cy, c->arc_dash.r,
                             c->arc_dash.thickness, c->arc_dash.a0, c->arc_dash.a1,
                             GUI_FX_ARC, 0.0f, 0.0f,
                             c->arc_dash.period / TESS_TAU, c->arc_dash.duty,
                             0.0f, 0.0f, 0u, 0.0f, c->arc_dash.abgr );
                break;

            /* GRAD splits col_b's four bytes across the two unorm16 uv lanes -- the shader
               contract for the self-sampled sweep (gui.h).  Exact k/65535 through pack and back. */
            case GUI_CMD_ARC_GRAD:
                tess_fx_arc( c->arc_grad.cx, c->arc_grad.cy, c->arc_grad.r,
                             c->arc_grad.thickness, c->arc_grad.a0, c->arc_grad.a1,
                             GUI_FX_ARC_GRAD,
                             (f32)(   c->arc_grad.col_b         & 0xFFFFu ) / 65535.0f,
                             (f32)( ( c->arc_grad.col_b >> 16 ) & 0xFFFFu ) / 65535.0f,
                             0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, c->arc_grad.col_a );
                break;

            /* The framebuffer-tiling patterns: the fragment does the tiling, the CPU's share is
               the quantized pitch + anchor phase (see tess_checker). */
            case GUI_CMD_CHECKER:
                s_tess.cur_corner_pow = c->checker.corner_pow;
                tess_checker( c->checker.x, c->checker.y, c->checker.w, c->checker.h,
                              c->checker.cell, c->checker.rounding,
                              c->checker.col_a, c->checker.col_b );
                break;

            case GUI_CMD_GRID:
                s_tess.cur_corner_pow = c->grid.corner_pow;
                tess_grid( c->grid.x, c->grid.y, c->grid.w, c->grid.h,
                           c->grid.ox, c->grid.oy, c->grid.cell, c->grid.thickness,
                           c->grid.angle, c->grid.rounding, c->grid.stripes != 0u,
                           c->grid.abgr );
                break;

            /* The regular polygon: filled, or stroked under GUI_OP_BAND -- the op set here for
               the reason every shape's is (the record's band width is sized from it). */
            case GUI_CMD_NGON:
                if ( c->ngon.thickness > 0.0f )
                    s_tess.cur_ops |= GUI_OP_BAND;
                tess_fx_ngon( c->ngon.cx, c->ngon.cy, c->ngon.r, c->ngon.sides,
                              c->ngon.rot, c->ngon.rounding, c->ngon.thickness,
                              c->ngon.abgr );
                break;

            /* The dashed border: a BAND box whose coverage the fragment cuts on the perimeter
               coordinate (GUI_OP_DASH).  The CPU's share is the SNAP: fit a whole number of
               dash cycles to the perimeter, computed from the same clamped radius the record
               will state, so the pattern meets itself where the walk closes. */
            case GUI_CMD_BOX_DASH:
            {
                f32 hw  = c->box_dash.w * 0.5f, hh = c->box_dash.h * 0.5f;
                f32 lim = ( hw < hh ) ? hw : hh;
                f32 r   = c->box_dash.rounding;
                if ( r > lim )  r = lim;
                if ( r < 0.0f ) r = 0.0f;

                f32 L      = 4.0f * ( hw + hh ) - 8.0f * r + TESS_TAU * r;
                f32 period = c->box_dash.dash + c->box_dash.gap;
                f32 n      = ( period > 0.0f ) ? floorf( L / period + 0.5f ) : 1.0f;
                if ( n < 1.0f ) n = 1.0f;

                tess_fx_aux_t aux = { 0 };
                aux.dash_period = L / n;
                aux.dash_duty   = c->box_dash.dash / period;
                aux.dash_scroll = 1.0f;                   /* the ants: one period per cycle */
                aux.anim_curve  = c->box_dash.curve;
                aux.anim_param  = c->box_dash.curve_param;

                /* The caller speaks in PERIMETER PX -- px/sec of scroll, a px offset.  The clock
                   speaks in cycles, the one unit every animating op reads.  Convert both once,
                   here, against the period the snap above just settled. */
                aux.anim_rate   = ( aux.dash_period > 0.0f )
                                ? c->box_dash.rate  / aux.dash_period : 0.0f;
                aux.anim_phase  = c->box_dash.anim_phase
                                + ( ( aux.dash_period > 0.0f )
                                    ? c->box_dash.phase / aux.dash_period : 0.0f );

                s_tess.cur_ops |= GUI_OP_BAND | GUI_OP_DASH;
                tess_fx_box( c->box_dash.x, c->box_dash.y, c->box_dash.w, c->box_dash.h,
                             c->box_dash.rounding, TESS_FX_AA, c->box_dash.t,
                             0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, c->box_dash.abgr, &aux );
                break;
            }

            /* One textured quad about its centre -- the glyph-run transform (tess_quad_xf)
               with the pivot every icon caller wants.  No snap, by the transformed-quad rule. */
            /* The lattice: one quad, one style record, however many copies -- the count reaches
               the fragment as the set's extent against the pitch, so it costs no lane and no
               per-copy work. */
            case GUI_CMD_REPEAT:
                tess_repeat_box( c->repeat.cx, c->repeat.cy, c->repeat.nx, c->repeat.ny,
                                 c->repeat.pitch_x, c->repeat.pitch_y,
                                 c->repeat.cell_w, c->repeat.cell_h,
                                 c->repeat.rounding, c->repeat.abgr );
                break;

            /* The ring: same one-quad trade taken angularly, and at a non-zero rate it spins in
               the fragment -- so the command's bytes stay put while it turns. */
            case GUI_CMD_REPEAT_POLAR:
                tess_repeat_polar( c->repeat_polar.cx, c->repeat_polar.cy,
                                   c->repeat_polar.n, c->repeat_polar.orbit,
                                   c->repeat_polar.cell_w, c->repeat_polar.cell_h,
                                   c->repeat_polar.rounding, c->repeat_polar.rate,
                                   c->repeat_polar.phase, c->repeat_polar.curve,
                                   c->repeat_polar.curve_param, c->repeat_polar.abgr );
                break;

            case GUI_CMD_IMAGE_XF:
            {
                f32 hx = c->image_xf.w * 0.5f, hy = c->image_xf.h * 0.5f;
                tess_quad_xf( c->image_xf.x + hx, c->image_xf.y + hy,
                              cosf( c->image_xf.rot ), sinf( c->image_xf.rot ),
                              -hx, -hy, c->image_xf.w, c->image_xf.h,
                              c->image_xf.u0, c->image_xf.v0, c->image_xf.u1, c->image_xf.v1,
                              c->image_xf.tex_idx, c->image_xf.abgr );
                break;
            }

            case GUI_CMD_TRIANGLE:
                tess_triangle( c->tri.ax, c->tri.ay, c->tri.bx, c->tri.by,
                               c->tri.cx, c->tri.cy, c->tri.abgr );
                break;

            /* The outline word is set once for the whole run: every glyph quad the loop emits
               carries it, and the fragment resolves fill and outline from the one distance field
               it was already sampling. */
            /* The only two cases that read glyphs, and therefore the only two that care which font
               is active.  Guarded on a change rather than set unconditionally because a run of
               labels in one font is the overwhelmingly common case and font_use rebuilds metrics. */
            case GUI_CMD_TEXT:
                if ( c->text.font != cur_font )
                    font_use( cur_font = c->text.font );
                tess_text_edge_prim( c->text.edge_w, c->text.edge_col );
                tess_text_n( c->text.x, c->text.y, c->text.abgr, s_draw.text_pool + c->text.off,
                             c->text.len, c->text.clip_x0, c->text.clip_x1 );
                break;

            /* Shadow + main copy in one string walk -- see tess_text_shadow_n. No TEXT_EDGE field
               to prime: cur_ops was just zeroed above, which is the no-edge state every plain
               run wants, and a drop shadow is never combined with a distance-field halo. */
            case GUI_CMD_TEXT_SHADOW:
                if ( c->text_shadow.font != cur_font )
                    font_use( cur_font = c->text_shadow.font );
                tess_text_shadow_n( c->text_shadow.x, c->text_shadow.y, c->text_shadow.abgr,
                                     c->text_shadow.shadow_abgr, c->text_shadow.dx, c->text_shadow.dy,
                                     s_draw.text_pool + c->text_shadow.off, c->text_shadow.len,
                                     c->text_shadow.clip_x0, c->text_shadow.clip_x1 );
                break;

            case GUI_CMD_TEXT_XF:
                if ( c->text_xf.font != cur_font )
                    font_use( cur_font = c->text_xf.font );
                tess_text_edge_prim( c->text_xf.edge_w, c->text_xf.edge_col );
                tess_text_xf( c->text_xf.x, c->text_xf.y, c->text_xf.abgr,
                              s_draw.text_pool + c->text_xf.off, c->text_xf.len,
                              c->text_xf.scale, c->text_xf.rot );
                break;

            /* Always a CAPSULE, because only diagonals ever arrive: gui_draw_line routes every
               axis-aligned segment through a grid-snapped rect at EMIT (stroke_axis_aligned_rect,
               gui_emit_path.c), and that is the sole producer of GUI_CMD_LINE.  One segment has
               no joints, which is the only thing that kept the ribbon (see tess_fx_segment). */
            case GUI_CMD_LINE:
                tess_fx_segment( c->line.x0, c->line.y0, c->line.x1, c->line.y1,
                                 c->line.thickness, c->line.border, c->line.abgr );
                break;

            case GUI_CMD_POLYLINE:
            {
                const gui_vec2_t* pts = &s_draw.points[ c->polyline.pt_offset ];
                f32 center_off = stroke_center_offset( c->polyline.align, c->polyline.thickness * 0.5f );
                tess_stroke_poly_aa( pts, c->polyline.pt_count, c->polyline.thickness,
                                     center_off, c->polyline.closed, c->polyline.abgr );
                break;
            }

            case GUI_CMD_DASHED_LINE:
                tess_dashed_line( c->dash.x0, c->dash.y0, c->dash.x1, c->dash.y1,
                                  c->dash.thickness, c->dash.period, c->dash.duty, c->dash.abgr );
                break;

            case GUI_CMD_RECT_GRADIENT:
                tess_rect_gradient( c->gradient.x, c->gradient.y, c->gradient.w, c->gradient.h,
                                    c->gradient.col_a, c->gradient.col_b, c->gradient.horizontal );
                break;

            case GUI_CMD_RECT_LIST:
            {
                /* One quad per pooled entry; all share this command's clip/vp so they collapse
                   into the same GPU batch.  Solid color (tex 0, self-sampled), never rounded. */
                const gui_rect_col_t* rl = &s_draw.rect_pool[ c->rect_list.offset ];
                for ( u32 k = 0; k < c->rect_list.count; ++k )
                    tess_rect_filled( rl[ k ].x, rl[ k ].y, rl[ k ].w, rl[ k ].h,
                                      0, 0, 1, 1, 0, rl[ k ].abgr );
                break;
            }

            case GUI_CMD_SPRITE:
                /* 1, 3 or 9 quads from this one command -- the whole expansion, plus the sprite
                   lookup it needs, lives in tess_sprite. */
                tess_sprite( c );
                break;
        }
    }

    if ( open_vid != GUI_ID_NONE )
        volatile_range_close( open_vid, vb_open, pb_open, cmd_open );

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/
