/*==============================================================================================

    gui/render/pipeline/gui_build_tess_state.c -- tessellation state + diagnostics.

    Part 1 of 7 of the CPU-side quad-record builder (gui_build_tess_*.c), which translates
    the frame's semantic gui_cmd_t list (s_draw) into quad records in s_tess: 
    
    ONE 16-byte gui_quad_t per shape (gui.h), expanded by SV_VertexID in gui_quad.vs.hlsl

    -- there is no vertex buffer and no index buffer. 
    -- Nothing in this file family touches the GPU API.

    This file holds the s_tess struct itself:
    
        - quad/style/gpu-cmd arenas plus every ambient field the emitters below read
        - overflow bits, s_volatile_patching, the geometry-generation/dirty-span tracking
        - cold s_tess_stats diagnostics block the dashboard and overlay read
    
    Every later file in this family _quad, _sdf, _arc, _text, _dispact, reads and writes s_tess.

    s_tess is read only by the two units included after this family: 
    
        - gui_build_cache.c (the BUILD phase fills it via tess_dispatch) and 
        - gui_render_submit.c (gui_render_flush uploads it and emits draw calls).
        - No file above the backend unit touches it.

    Included by gui_render.c after gui_emit_path.c (provides v2, seg_normal,
    stroke_center_offset, STROKE_* constants) and before gui_build_cache.c 
    (which drives tess_reset / tess_dispatch from cache_build_frame / cache_tess_window).

==============================================================================================*/
// clang-format off

/* One GPU draw command -- the AOS command record.  Every consumer (the merge check, the flush
   loop, the volatile copy-back, the dashboard capture) reads a WHOLE command at a given index;
   none sweeps a single field across all commands, so the fields that belong to one command live
   together in one cache line rather than in parallel arrays keyed on the same index.  qbase is
   explicit (not accumulated from elem_counts at flush) so the quad arena may hold reserved
   gaps -- volatile block headroom -- between commands. */

typedef struct
{
    u32 elem_count;    // number of quads to draw (the flush multiplies by 6 at the cmd_draw)

    /* The texture of the command's FIRST primitive, kept for diagnostics only (the dashboard
       tooltip).  Not a batch key and not a description of the whole command: the texture rides
       the prim record (gui_prim_t), so one command can span several. */

    u32 tex_idx;

    i16 vp;            // viewport for this command (GUI_VP_INVALID = dormant volatile pad)
    u16 qbase;         // quad slot -- first quad of command (its draw's first_vertex / 6)

} tess_gpu_cmd_t;

ORB_STATIC_ASSERT( GUI_MAX_QUADS <= 0xFFFF, "tess_gpu_cmd_t.qbase is u16" );

/*==============================================================================================
    Tessellation state -- the quad and prim arenas populated from the semantic command list.

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
    gui_prim_t      prims    [ GUI_MAX_PRIMS ];    // prim records -- the second arena
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
    u32         cur_tex;    /* GUI_TEX_MODE | bindless slot the prim record carries -- set by
                               tess_set_tex, folded by tess_quad_push.  NOT a batch key           */
    u32         cur_ops;    /* GUI_OP_* -- the per-primitive modifiers (gui.h).  Cleared per
                               semantic command alongside the record: leaking a self flag would
                               blank a textured quad, and leaking an op would reshape the next
                               fill.  tess_quad_push folds these into the prim record. */
    f32         cur_corner_pow; /* corner profile exponent for the box family, ambient over one
                               command for the same reason cur_ops is: it reaches all four corners
                               of a shape without threading a parameter through every fill in the
                               library, and cannot leak onto the next command.  0 = circular arcs */
    u32         cur_fx_field;   /* the FIELD the box family writes, when it is not the analytic box.
                               Only GUI_FX_TEX sets it today: a baked shape is the same surface
                               with its distance sampled instead of evaluated, so it rides
                               tess_fx_box_core rather than forking it.  0 = whatever the emitter
                               states for itself, and cleared per command like cur_ops.          */

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

    /* The style accounting for the slot under tessellation, carried onto the slot at the end of
       its pass and reused verbatim across retained frames -- exactly like the two above, and for
       the same reason: retained geometry is never re-walked, so a number derived at replay time
       would read zero for every window that did not change.

       style_refs is the TOTAL -- one per shape that wants a prim record, which is what
       tess_prim_local is called once for (a BAND covering resolves one style and emits four
       quads; a plain glyph resolves none at all).  stored_mask is WHICH palette entries those
       shapes resolved to, one bit each, so the frame's distinct stored set is the OR over slots
       -- two windows naming entry 5 are one entry in use, which is the entire point of it being
       stored rather than per-slot. */

    u32         slot_style_refs;
    u32         slot_stored_mask[ ( GUI_PAL_MAX + 31u ) / 32u ];

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

    /* The last ANSWER tess_prim_local gave, and the context it was given in.  A repeat
       quad -- another row of the same table, another segment of one polyline, the next
       shape of a run under one unchanging record -- compares equal here and reuses
       cur_prim_local outright.  It catches what neither of the two memos below it
       structurally can: the arena memo compares against the record at prim_count-1, and
       an answer that APPENDED NOTHING (a palette hit, or a hit on this memo) leaves
       prim_count where it was, so a run of one style misses it on every quad.

       The memo owns a COPY rather than pointing at the answer's home: an arena entry is
       rewritten by a later fx page and a palette entry by an epoch reset, and a pointer into
       either would compare against bytes that had moved on.

       base/floor are the validity guard, and they are the whole of it -- an answer is
       slot-LOCAL, so it means something else the moment the slot origin moves, and the
       records behind the floor belong to a reservation this pass does not own.  Guarding
       on the two values rather than invalidating at each site that moves them is what
       keeps a new such site from silently inheriting a stale answer.  tess_reset clears
       the flag because both values legitimately return to 0 there while the arena
       underneath them is gone. */

    gui_prim_t  prim_memo_rec;
    u32         prim_memo_base;
    u32         prim_memo_floor;
    bool        prim_memo_valid;

    /* The palette entry the CURRENT semantic command resolved to the last time it
       tessellated, or GUI_PAL_NONE.  Set once per command by tess_dispatch and read by
       tess_prim_local, which confirms it against the entry's own bytes before believing it.

       This is the memo the two above cannot be: both are one deep, so a window alternating
       among a dozen styles -- a toolbar of different buttons, a table with several column
       looks -- evicts them on every step and folds the record again for each.  A command site
       is a stable address for its own answer, so a dozen alternating commands keep a dozen
       live memos. Where it comes from is gui_render_intern.c (pal_cmd_hint), keyed on the
       command hash the emit phase already folds for the retained cache. */

    u32         cmd_hint;

    /* What the current command has resolved so far, for the park at its end.  Separate from
       cur_prim_local because that one must survive across commands -- the answer memo hands
       it straight back -- while this has to read "nothing yet" at each command's start, which
       it does by being an ARENA index (0) that pal_cmd_learn declines to park. */

    u32         cmd_prim_out;

    /* The open FX PAGE -- a prim-arena record carrying four gui_fx_t records (gui.h).  fx_page is
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
       border, a turning shape and a staggered pulse never add prim records: cur_prim states only
       what is shared (shape, widths, rates).  Each is an ambient the command sets before its
       tess_quad_push, which folds it into the quad unread by the style.  Cleared per command.
         cur_col_border  GUI_OP_FRAME's border band colour (0 = unused)
         cur_rot_c       the turn, as a unit pair; (1, 0) is the identity a plain shape leaves
         cur_phase       animation phase in cycles (0 = in step with the clock)
         cur_swell       GUI_OP_SWELL's amplitude in px (0 = unused; negative shrinks) */
    u32         cur_col_border;
    f32         cur_rot_c, cur_rot_s;
    f32         cur_phase;
    f32         cur_swell;

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
       slot_quad_base for the prim arena.  Quads bake SLOT-LOCAL style indices
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
   far into a window's own prim arena a quad's fx field can reach.

   Every bit here means DROPPED CONTENT: something the frame asked for is not on screen.  The
   retained cache's per-window command cap (WIN_SLOT_CMD_MAX) is deliberately NOT one of them --
   overrunning it only makes a window uncacheable, and it still draws in full. */

#define TESS_OVF_QUADS      ( 1u << 0 )   /* quad arena full              -- GUI_MAX_QUADS    */
#define TESS_OVF_CMDS       ( 1u << 1 )   /* gpu command table full       -- GUI_MAX_CMDS     */
#define TESS_OVF_PRIMS      ( 1u << 2 )   /* prim record arena full      -- GUI_MAX_PRIMS    */
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

// clang-format on
