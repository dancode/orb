/*==============================================================================================

    gui/render/pipeline/gui_emit_state.c -- The draw list's state: pools, clip stack, ambient.

    EMIT is the pipeline's first phase: widgets push semantic gui_cmd_t records (no vertices yet)
    that BUILD later tessellates.  It spans six files, included in this order by gui_render.c:

        gui_emit_state.c   this file -- s_draw, the frame reset, the clip stack, the ambient
        gui_emit_cmd.c     the command record: claim, stamp, hash, seal
        gui_emit_shape.c   fills, pictures, gradients
        gui_emit_fx.c      the SDF surface family (shadows, sectors, patterns, lattices)
        gui_emit_edge.c    outlines, bezels, the bare triangle
        gui_emit_text.c    glyph runs
        gui_emit_path.c    the line / path stroker

    This file owns s_draw -- every pool the frame fills and every ambient the pushes read -- and
    nothing in it emits a command.  Unity visibility flows downward only, so it must come first.

    The resolvers the shape files call -- font_glyph, icon_get, icon_atlas_idx -- are NOT in this
    unit: fonts and icons are the draw object unit's, but the server reaches them through the
    glyph/sprite resource contract declared in render/gui_render.h, which the draw unit
    implements.  Nothing here depends on include order for them.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    GPU Command -- backend-private GPU draw command after tessellation.

    One bounded range of quads -- the unit the GPU sees (not exposed in gui.h) 
    The public gui_cmd_t carries semantic shapes; the BUILD phase (gui_build_tess.c)
    tessellates those into these.
==============================================================================================*/

typedef struct
{
    u32          elem_count;    // number of quads to draw (6 vertices each)

    /* The texture of the command's FIRST primitive, kept for diagnostics only (the dashboard
       tooltip).  It is no longer a batch key and no longer describes the whole command: the
       texture rides the style record (gui_prim_t), so one command can span several. */

    u32          tex_idx;       // first primitive's model|slot -- diagnostic, not a batch key

} gui_gpu_cmd_t;

/*==============================================================================================
    CPU Draw list -- the per-frame command buffer and segment table.

    Commands are pushed into cmds[] by the widget layer.  Whenever (win, z, vp, band) changes,
    the current open span is closed and a new one is opened, so the buffer is partitioned into
    contiguous per-(win,z,vp,band) segments.  cache_tess_window walks these segments per window
    rather than re-scanning the full command buffer.  [lo, hi) is a half-open range; the final
    segment's hi is closed at build time.  win=0 is the background (non-window) draw layer.

    The FONT used to be a fifth axis here and is not one any more.  A segment exists to partition
    the command list for backend dispatch, so its key should be the things that decide dispatch --
    which window owns the geometry, where it sorts, which surface it lands on, which arena band it
    is accounted to.  A font decides none of those.  It decides where a glyph's UVs come from, which
    is per-command information, and since every font packs into the one shared atlas a font change
    usually moves no texture at all; even when it does (a distance-field font has its own atlas) the
    texture rides the VERTEX and cannot cut a draw call.  So the font was cutting segments to carry
    a lookup.  It now rides the text commands that actually use it (gui.h), which is why nothing
    below threads a font through the permutation any more.
==============================================================================================*/

/* Packed to 16 bytes: GUI_MAX_SEGS of these live here, and the command stepper keeps two more
   full copies (segs + disp_segs).  Every narrow field's range is capped by construction: lo/hi
   by GUI_MAX_CMDS (asserted below), vp by GUI_MAX_VIEWPORTS (4), band is 0/1.  z stays u32 -- it
   carries full sort keys (popup/overlay z-bands). */

typedef struct
{
    gui_id_t win;
    u32      z;
    u16      lo, hi;   /* half-open command range into s_draw.cmds[] */
    u8       vp;
    u8       band;     /* arena band: 0 = main UI, 1 = debug/diagnostic UI (GUI_WIN_DEBUG_BAND) */

} gui_cmd_seg_t;

ORB_STATIC_ASSERT( GUI_MAX_CMDS <= 0xFFFF, "gui_cmd_seg_t.lo/hi are u16" );

static struct
{
    gui_cmd_t       cmds            [ GUI_MAX_CMDS ];           // semantic command list; one entry per shape
    u32             cmd_hashes      [ GUI_MAX_CMDS ];           // per-command hash baked at emit (for cache diff)
    gui_id_t        cmd_volatile_id [ GUI_MAX_CMDS ];           // GUI_ID_NONE, or the volatile widget owning this cmd

#ifdef GUI_CMD_STEPPER
    gui_id_t        cmd_owner       [ GUI_MAX_CMDS ];           // emitting widget id (0 = chrome); stepper display only
    gui_id_t        cur_owner;                                  // stamped by item_state (STEP_SET_OWNER); 0 at window seams
#endif

    gui_vec2_t      points          [ GUI_MAX_PATH_PTS ];       // point pool for CMD_POLYLINE data; indexed by pt_offset
    gui_rect_col_t  rect_pool       [ GUI_MAX_RECT_ENTRIES ];   // rect pool for CMD_RECT_LIST data; indexed by offset

    gui_cmd_seg_t   segs            [ GUI_MAX_SEGS ];            // per-(win,z,vp,band) spans, in emit order
    u32             seg_count;                                  // spans open this frame (>= 1; segs[0] is the bg span)

    /* Flat string pool: draw_push_text_n copies every string here so that stack-local buffers
       (textf, snprintf labels) remain valid until gui_render_flush consumes them. */

    char            text_pool[ GUI_MAX_TEXT_POOL ];
    u32             text_pool_used;

    u32             cmd_count;      /* commands in the list this frame  */
    u32             pt_count;       /* points in the pool this frame */
    u32             rect_count;     /* rect-pool entries used this frame */

    /* Lifetime peak of each pool above, folded in by draw_reset just before it clears the counts.
       The frame counters are the only truth while a frame is open and they are gone the moment it
       closes, so a cap can only be argued from what was retained here -- see backend_pool_report
       (render/gui_render_mem.c), which prints all of them against their #defines. */
    u32             cmd_hwm, pt_hwm, rect_hwm, text_hwm, clip_hwm, seg_hwm;

    gui_id_t        cur_win;        /* owning window id stamped onto new commands (set by begin/window_end) */
    u32             cur_z;          /* sort key tracked per-segment (draw_seg_retag; NOT baked per command) */
    i32             cur_vp;         /* viewport stamped onto new commands (set by begin/window_end)        */
    u32             cur_font;       /* active font id (draw_set_font), stamped ONTO each text command as it
                                       is pushed.  Not a segment axis: it selects glyph metrics and atlas
                                       UVs, which is per-command data, and cuts no batch. */
    u32             cur_band;       /* arena band tracked per-segment (draw_set_band); 0 = main UI,
                                       1 = debug band (self-measuring diagnostic windows).  NOT per command. */

    /* Clip table: append-only per-frame pool of distinct scissor rects.  clip_push_clip_rect
       appends each intersected rect and records its index in clip_idx_stack so the active
       index (cur_clip_idx) is available O(1) at emit time -- no per-emit search. */

    gui_rect_t      clip_table      [ GUI_MAX_CLIP_RECTS ];     /* flat pool of all clip rects this frame   */
    f32             clip_radius     [ GUI_MAX_CLIP_RECTS ];     /* per-entry corner radius; 0 = square cut.
                                                                   Part of the entry's identity: baked into
                                                                   clip_hash_cache, matched by dedup, read
                                                                   by tess_clip_local into the slot table  */
    u32             clip_hash_cache [ GUI_MAX_CLIP_RECTS ];     /* fnv1a of (rect, radius), baked when added */
    u32             clip_table_n;                               /* entries used this frame                  */
    u8              clip_idx_stack  [ GUI_CLIP_DEPTH ];         /* parallel to clip_stack: index per level  */
    u8              cur_clip_idx;                               /* top-of-stack index, stamped on each emit */

    gui_rect_t      clip_stack[ GUI_CLIP_DEPTH ];               /* intersected rects, mirrors clip_table   */
    u32             clip_depth;

    /* Global opacity multiplier applied to every pushed shape.  1.0 normally; lowered for the
       span of a disabled item so it dims with no per-widget code; reset by item / chrome seams. */
    f32 alpha;

    /* Ambient corner radius folded into every filled / outlined rect.  Set from the resolved
       rounding category (window / widget / grab) at the item / chrome seams and at the few sites
       that draw a different category; 0 emits square shapes (the fast path). */
    f32 rounding;

    /* Ambient corner PROFILE, folded in beside the radius: the exponent of the norm the corner
       arc is measured in (gui.h, gui_prim_t row 2).  Held as the exponent rather than as the
       0..1 smoothing a caller authors, so the conversion happens once in draw_set_corner_smooth
       instead of per pushed shape.  0 = circular, which is what every shape gets until a theme
       says otherwise -- it is installed once per frame from GUI_VAR_CORNER_SMOOTH, so a caller
       only touches it to override one shape. */
    f32 corner_pow;

    /* Ambient ANIMATION CURVE, folded into every shape that carries a clock -- the pulse, the
       spinner, the marching ants (gui_curve_t, gui_prim_t row 5).  Ambient rather than a
       parameter on each of those calls because it is one property of MOTION, and a seam that
       wants its whole panel to move on the same easing should say so once.  GUI_CURVE_LINEAR
       leaves the phase unshaped, which is what a spin and a scroll want; a pulse that names no
       curve gets the raised cosine it has always had, resolved at push time below. */
    u32 anim_curve;
    f32 anim_curve_param;

    /* Ambient animation PHASE, in cycles, ADDED to whatever phase an animating call states of
       its own.  The two compose because they answer different questions: a call's phase staggers
       one element against its neighbours, this one anchors the whole cycle to an EVENT (see
       anim_once, gui_api.h).  A shape can want both. */
    f32 anim_phase;

    /* Ambient BORDER ALIGNMENT for the stroked box family: where the band sits against the
       authored boundary.  0 = inside (the band's outer edge on the boundary -- the default every
       outline has always had), 0.5 = centred, 1 = outside (the band's inner edge on it).
       Resolved at PUSH time by inflating the shape and its radius by align * width, so the
       tessellator and the fragment never learn the concept -- an inside band of the inflated
       shape IS the aligned band of the authored one. */
    f32 border_align;

    /* Ambient horizontal text-clip window: glyph-level [x0, x1] hard-clip folded into every pushed
       text run that does not carry its own explicit window.  The sentinel (-/+ GUI_TEXT_NO_CLIP)
       means unclipped (the common path).  A seam that draws text into a bounded slot -- a table cell
       at the scroll/viewport edge -- sets it for the span so glyphs terminate cleanly at the slot
       edge instead of bleeding to the enclosing scissor (the self-fit rule at the glyph boundary). */
    f32 text_clip_x0;
    f32 text_clip_x1;

    /* Ambient TEXT_EDGE word folded into every pushed text run; 0 (the default) is a plain run.
       Stamped onto the command at push time rather than read at tessellation, because a retained
       window re-tessellates long after the ambient has moved on. */
    f32 text_edge_w;
    u32 text_edge_col;

} s_draw;

/*==============================================================================================
    FNV-1a hash helpers -- defined here (before draw_reset) so clip pre-hashing can use them.
    draw_hash_cmd below also uses them; fnv1a_u32 is visible in gui_build_cache.c, and pal_hash /
    census_hash fold whole gui_prim_t records through fnv1a (all included after this file by
    gui_render.c).

    These run on the two hottest loops in the whole backend -- once per emitted command
    (draw_hash_cmd) and once per style-record miss (pal_hash) -- and both are pure latency: an
    FNV fold is a serial xor/multiply chain, so the byte count IS the cost.  Everything they
    hash is 4-byte data (f32 coordinates, packed colours, whole gui_prim_t records), so the
    unit of folding is the WORD, with a byte tail for the odd short field.

    The extra `h ^= h >> 15` is what makes a word fold safe to substitute for four byte folds.
    A multiply propagates bits upward only, so without it a difference confined to a value's
    top byte (an alpha-only colour change) would stay pinned to the top bits of h forever and
    never spread across the accumulation.  The xor-shift folds them back down, which gives
    strictly better avalanche than the byte-at-a-time chain it replaces, at well under half its
    dependency length.

    The fold is frame-to-frame and in-memory only -- nothing persists a hash across runs -- so
    the values these produce are free to change.
==============================================================================================*/

/* One 4-byte mix: the FNV-1a step plus the down-fold described above. */
static inline u32
fnv1a_u32( u32 h, u32 v )
{
    h  = ( h ^ v ) * 16777619u;
    h ^= h >> 15;
    return h;
}

/* Whole-word fold with a byte tail.

   The word is assembled from its bytes rather than loaded through a u32 pointer, for two
   reasons that both have teeth.  ALIGNMENT: callers hash spans that are not 4-aligned -- a
   text-pool substring starts wherever its command's offset lands -- and a fold that took the
   word path only when the span happened to be aligned would hash the same string differently
   at different pool offsets, which is precisely the false-dirty that draw_hash_cmd excludes
   text.off to avoid.  DEBUG BUILDS: /Od inlines nothing and turns a memcpy of four bytes into
   a CRT call, which would cost more than the byte chain this replaces.  Assembled explicitly,
   the loads are independent of h and sit off the critical path, and optimized builds fold the
   four of them back into one unaligned load.

   The mix is spelled out here rather than calling fnv1a_u32 for the same /Od reason: a static
   inline is a real call under it, and one per word inside this loop would undo the win. */

static inline u32
fnv1a( u32 h, const void* p, u32 n )
{
    const u8* b = (const u8*)p;

    for ( ; n >= 4u; b += 4, n -= 4u )
    {
        u32 w = (u32)b[ 0 ] | ( (u32)b[ 1 ] << 8 )
              | ( (u32)b[ 2 ] << 16 ) | ( (u32)b[ 3 ] << 24 );
        h  = ( h ^ w ) * 16777619u;
        h ^= h >> 15;
    }
    for ( u32 i = 0; i < n; ++i )
        h = ( h ^ b[ i ] ) * 16777619u;

    return h;
}

/*==============================================================================================
    COMMAND STEPPING

    draw_emit_blocked -- the one gate every draw_push_* entry point checks before spending a
    command slot (and, by early-outing first, any pool space): the command list is full.
    
    It also integrates with the command stepper, replaying a frozen frame and live main-band 
    emission is suppressed at the source (render/gui_step_capture.c).
==============================================================================================*/

static inline bool
draw_emit_blocked( void )
{
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
    {
        /* Say so. Every other fixed pool in the gui follows the loud-overflow rule (GUI_WARN_ONCE,
           rect/gui_rect.h): degrade gracefully, but report once so the symptom traces to its cap.
           This one -- the pool most likely to saturate, since a single dense drawer can exhaust it
           in one call -- was the exception, and a silent drop here is the worst possible failure
           mode: emission simply STOPS mid-frame, so the shapes that vanish are the ones with
           nothing wrong with them, and the real culprit (whatever ran earlier and spent the
           budget) is still on screen looking fine. */
        GUI_WARN_ONCE( "draw command list full (%u) -- shapes emitted after this point are dropped "
                       "for the rest of the frame; raise GUI_MAX_CMDS or batch dense fills through "
                       "draw_rects\n", (u32)GUI_MAX_CMDS );
        return true;
    }

    /* The command stepper is a debug tool that freezes the main-band draw list and replays a
       frozen frame, so the user can step through the commands one at a time. It is compiled 
       out in release builds */

    if ( STEP_EMIT_SUPPRESSED() )
        return true;

    #ifdef GUI_CMD_STEPPER
    /* Stepper only array: Stamp the emitting widget onto the slot this push will occupy.
       (the push may still drop for alpha/cull), but exact: every push checks this gate first,
       so whichever push finally lands at this index was also the last stamper. */
    s_draw.cmd_owner[ s_draw.cmd_count ] = s_draw.cur_owner;
    #endif

    return false; 
}

#ifdef GUI_CMD_STEPPER

/* Called by item_state (STEP_SET_OWNER, gui_render.h) as each widget registers 
   -- the commands it paints right after carry its id.  Reset to 0 (chrome) at window 
   transitions in draw_seg_retag; chrome painted after a window's last widget still 
   attributes to that widget (a known display-only imprecision). */

void
draw_set_cmd_owner( gui_id_t id )
{
    s_draw.cur_owner = id;
}

#endif

/*==============================================================================================
    draw_reset -- call at the top of the frame (frame_begin)
==============================================================================================*/

void
draw_reset( i32 display_w, i32 display_h )
{
    /* Volatile range tags must not survive into a frame whose command indices shifted: a stale
       tag on an unrelated command would exclude it from its window's hash, split it out of its
       batch, and let a volatile patch stomp its geometry.  Clearing the PREVIOUS frame's used
       range is complete: tags only ever land at indices below that frame's cmd_count
       (volatile_cb_close brackets live commands), so everything above the high-water mark is
       already zero by induction from the zeroed static.  Must run before cmd_count resets.
       (GUI_ID_NONE is 0.) */
    memset( s_draw.cmd_volatile_id, 0, s_draw.cmd_count * sizeof( s_draw.cmd_volatile_id[ 0 ] ) );

    /* Retain the closing frame's fills before they are cleared -- the pool report has no other
       chance to see them (a saturated pool that dropped content still reads at its cap here,
       which is exactly the signal). */
    if ( s_draw.cmd_count      > s_draw.cmd_hwm  ) s_draw.cmd_hwm  = s_draw.cmd_count;
    if ( s_draw.pt_count       > s_draw.pt_hwm   ) s_draw.pt_hwm   = s_draw.pt_count;
    if ( s_draw.rect_count     > s_draw.rect_hwm ) s_draw.rect_hwm = s_draw.rect_count;
    if ( s_draw.text_pool_used > s_draw.text_hwm ) s_draw.text_hwm = s_draw.text_pool_used;
    if ( s_draw.clip_table_n   > s_draw.clip_hwm ) s_draw.clip_hwm = s_draw.clip_table_n;
    if ( s_draw.seg_count      > s_draw.seg_hwm  ) s_draw.seg_hwm  = s_draw.seg_count;

    s_draw.cmd_count       = 0;
    s_draw.pt_count        = 0;
    s_draw.rect_count      = 0;
    s_draw.text_pool_used  = 0;
    s_draw.cur_win         = 0;   /* background; windows tag it via draw_set_window */
    s_draw.cur_z           = 0;   /* background; windows raise it via draw_set_sort_key */
    s_draw.cur_vp          = 0;   /* main viewport; windows route via draw_set_viewport */
    s_draw.cur_font        = font_active_id();   /* background segment inherits whatever font is active now */
    s_draw.cur_band        = 0;   /* main band; diagnostic windows switch via draw_set_band */

    /* Open the first command segment: background (win 0, z 0, main viewport, active font, main band). */
    s_draw.seg_count       = 1;
    s_draw.segs[ 0 ]       = ( gui_cmd_seg_t ){ 0 };

    /* Seed the clip table: slot 0 = full display rect.  clip_idx_stack[0] and cur_clip_idx both
       start at 0 so every emitter finds the root clip without a push being required first.
       The hash folds (rect, radius) exactly as clip_append's does, or slot 0 could never dedup. */

    s_draw.clip_table[ 0 ]      = ( gui_rect_t ){ 0.0f, 0.0f, (f32)display_w, (f32)display_h };
    s_draw.clip_radius[ 0 ]     = 0.0f;
    s_draw.clip_hash_cache[ 0 ] = fnv1a( 2166136261u, &s_draw.clip_table[ 0 ], sizeof( gui_rect_t ) );
    s_draw.clip_hash_cache[ 0 ] = fnv1a( s_draw.clip_hash_cache[ 0 ], &s_draw.clip_radius[ 0 ], sizeof( f32 ) );
    s_draw.clip_stack[ 0 ]      = s_draw.clip_table[ 0 ];
    s_draw.clip_table_n         = 1;
    s_draw.clip_idx_stack[ 0 ]  = 0;
    s_draw.cur_clip_idx         = 0;
    s_draw.clip_depth           = 1;

    s_draw.alpha                = 1.0f;
    s_draw.rounding             = 0.0f;                 /* square until a seam sets the resolved radius */
    s_draw.corner_pow           = 0.0f;                 /* circular arcs until the frame installs the style */
    s_draw.border_align         = 0.0f;                 /* borders lie inside until a caller moves them */
    s_draw.text_clip_x0         = -GUI_TEXT_NO_CLIP;    /* unclipped until a seam sets a window */
    s_draw.text_clip_x1         =  GUI_TEXT_NO_CLIP;
    s_draw.text_edge_w          = 0.0f;                 /* plain runs until a caller asks for an edge */
    s_draw.text_edge_col        = 0u;

#ifdef GUI_CMD_STEPPER
    s_draw.cur_owner = 0;       /* background/chrome until the first widget stamps */
#endif

    /* Command stepper: while a frozen frame is replayed, pre-load the frozen command prefix and
       pools over the empty frame just seeded.  A no-op unless GUI_CMD_STEPPER and frozen. */

    STEP_RESTORE_EMIT();
}

/*==============================================================================================

    Clip Stack

==============================================================================================*/

/* Append r to the clip table and return its index -- or the index of an existing identical rect.
   Dedup keeps the table at "distinct scissors this frame": the root set/restore swap around every
   window and same-rect region pushes would otherwise mint a fresh entry per call (the table is
   append-only -- entries are immutable once a command holds their index -- so reuse is the only
   way to collapse them).  Safe by design: the retained cache hashes the clip VALUE, not the index
   (draw_hash_cmd), and tess groups by index, so collapsing duplicates can only merge batches.
   On overflow (table full) the index saturates to the second-to-last slot -- commands share a
   slightly wrong clip rather than writing OOB.  Slot GUI_MAX_CLIP_RECTS-1 is reserved as an
   invalid sentinel and is never written. */

static u8
clip_append( gui_rect_t r, f32 radius )
{
    /* Frozen replay: suppressed band-0 spans must not grow the table (the frozen entries plus
       the live debug band share its 64 slots).  Slot 0 is the frozen frame's root -- the display
       rect -- so any command stamped with it (all suppressed anyway) stays sane. */
    if ( STEP_EMIT_SUPPRESSED() )
        return 0;

    /* A radius past the half-extent inverts the rounded-box field; clamp so the tightest legal
       capsule is the worst case.  Runs before hashing so equal requests dedup to one entry. */
    f32 max_r = ( ( r.w < r.h ) ? r.w : r.h ) * 0.5f;
    if ( radius > max_r ) radius = max_r;
    if ( radius < 0.0f )  radius = 0.0f;

    u32 h = fnv1a( 2166136261u, &r, sizeof( gui_rect_t ) );
    h     = fnv1a( h, &radius, sizeof( f32 ) );
    for ( u32 i = 0; i < s_draw.clip_table_n; ++i )
        if ( s_draw.clip_hash_cache[ i ] == h )
        {
            const gui_rect_t* t = &s_draw.clip_table[ i ];
            if ( t->x == r.x && t->y == r.y && t->w == r.w && t->h == r.h
              && s_draw.clip_radius[ i ] == radius )
                return (u8)i;
        }
    if ( s_draw.clip_table_n < GUI_MAX_CLIP_RECTS - 1u )
    {
        u8 ci = (u8)s_draw.clip_table_n++;
        s_draw.clip_table      [ ci ] = r;
        s_draw.clip_radius     [ ci ] = radius;
        s_draw.clip_hash_cache [ ci ] = h;
        return ci;
    }

    /* Saturation is a VISUAL corruption, not a drop: every clip past the cap shares slot cap-2's
       rect, so content scissors against some other region's box.  Without a report this reads as
       an inexplicable clipping glitch. */
    GUI_WARN_ONCE( "clip table full (%u distinct scissor rects this frame) -- "
                   "further clips share a wrong rect. Raise GUI_MAX_CLIP_RECTS (gui.h).\n",
                   (unsigned)GUI_MAX_CLIP_RECTS );
    ORB_ASSERT_MSG_ONCE( false, "gui clip table saturated -- clips share a wrong scissor rect; "
                                "raise GUI_MAX_CLIP_RECTS (gui.h)" );
    return (u8)( GUI_MAX_CLIP_RECTS - 2u );
}

static gui_rect_t
clip_current( void )
{
    return s_draw.clip_stack[ s_draw.clip_depth - 1 ];
}

/* A rect that bounds no pixels (rect_intersect clamps a missed overlap to zero w/h). */
static bool
rect_empty( gui_rect_t r )
{
    return r.w <= 0.0f || r.h <= 0.0f;
}

/*==============================================================================================
  
   - Reject a shape whose axis-aligned bounds cannot touch the current clip. 
   - It would emit a command + geometry the GPU then scissors to nothing.  
   - clip_current() Is the scissor the shape renders under; 
   - Conservative: only a box fully past an edge is dropped (touching counts as visible), 
     but an empty clip rejects everything in it.

==============================================================================================*/

bool draw_cull_box( f32 x, f32 y, f32 w, f32 h )
{
    gui_rect_t c = clip_current();
    if ( rect_empty( c ) )                  return true;   /* nothing in an empty clip is visible */
    if ( x + w <= c.x || x >= c.x + c.w )   return true;   /* fully left / right of the clip      */
    if ( y + h <= c.y || y >= c.y + c.h )   return true;   /* fully above / below the clip        */
    return false;
}

/*==============================================================================================
  
    draw_push_clip_body - The shared push body.  
    
    `radius` rounds the clip's own corners -- the per-fragment cut the scissor could 
    never express (gui_fx.hlsli, clip_coverage).  
    
    It applies to THIS entry only: a clip nested inside a rounded one intersects against the
    parent's RECT (the corner arcs do not compose through rect_intersect), which errs by letting a child paint into its parent's corner
    arc -- the parent's own chrome overpaints there in practice.

==============================================================================================*/

static void
draw_push_clip_body( f32 x, f32 y, f32 w, f32 h, f32 radius )
{
    /* Intersect with the enclosing clip so a nested region can never clip outside its parent.
       The push always happens -- a clipped-out region pushes a zero rect and draws nothing --
       so every push has a matching pop and the stack stays balanced. */
    gui_rect_t c  = rect_intersect( ( gui_rect_t ){ x, y, w, h }, clip_current() );
    u8         ci = clip_append( c, radius );

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
draw_push_clip_rect( f32 x, f32 y, f32 w, f32 h )
{
    draw_push_clip_body( x, y, w, h, 0.0f );
}

void
draw_push_clip_rect_rounded( f32 x, f32 y, f32 w, f32 h, f32 radius )
{
    draw_push_clip_body( x, y, w, h, radius );
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
   its viewport's drawable size (window_end restores the main display).  The table entry comes
   from clip_append -- never an in-place overwrite of the old root's slot, since commands already
   emitted reference that index -- and dedup there makes the common set/restore ping-pong (main-
   display windows swap display -> display) reuse one slot instead of minting entries per window.
   clip_idx_stack[0] is updated so subsequent pushes inherit the new root. */
void
draw_set_root_clip( f32 w, f32 h )
{
    gui_rect_t r              = ( gui_rect_t ){ 0.0f, 0.0f, w, h };
    s_draw.clip_stack[ 0 ]    = r;
    u8 ci                     = clip_append( r, 0.0f );
    s_draw.clip_idx_stack[ 0 ] = ci;
    s_draw.cur_clip_idx        = ci;
}

/* Re-tag subsequent commands with (win, z, vp, band), cutting a new command segment at the
   boundary.  Each draw_set_* setter passes the current value for the other three axes, so any one
   of them changing opens a fresh span.  If
   the current segment is still empty (no command emitted since it opened) its tag is simply rewritten
   in place, so back-to-back set_window / set_sort_key / set_viewport calls before any draw never spawn
   empty spans.  On overflow the open segment is just extended (its tag may then be stale, but only in
   the pathological >1024-segment case, which cache_tess_window already falls back to natural order). */
static void
draw_seg_retag( gui_id_t win, u32 z, i32 vp, u32 band )
{
    bool same_tag = win == s_draw.cur_win && z == s_draw.cur_z && vp == s_draw.cur_vp
                    && band == s_draw.cur_band;
    if ( same_tag )
        return;   /* no real change -- keep the open segment as is */

#ifdef GUI_CMD_STEPPER
    if ( win != s_draw.cur_win )
        s_draw.cur_owner = 0;   /* window transition: chrome until the first widget stamps */
#endif

    /* No open segment yet -- called outside a frame (e.g. font_use during startup setup, before the
       first draw_reset).  Just track the tag; draw_reset re-seeds segs[0] from it next frame. */
    if ( s_draw.seg_count == 0 )
    {
        s_draw.cur_win  = win;
        s_draw.cur_z    = z;
        s_draw.cur_vp   = vp;
        s_draw.cur_band = band;
        return;
    }

    gui_cmd_seg_t* cur = &s_draw.segs[ s_draw.seg_count - 1 ];
    if ( cur->lo == s_draw.cmd_count )
    {
        cur->win  = win;   /* segment empty so far: retag in place rather than splitting */
        cur->z    = z;
        cur->vp   = (u8)vp;
        cur->band = (u8)band;
    }
    else if ( s_draw.seg_count < GUI_MAX_SEGS )
    {
        cur->hi                           = (u16)s_draw.cmd_count;   /* close the span here */
        s_draw.segs[ s_draw.seg_count++ ] =
            ( gui_cmd_seg_t ){ .win = win, .z = z, .vp = (u8)vp,
                               .band = (u8)band, .lo = (u16)s_draw.cmd_count,
                               .hi = (u16)s_draw.cmd_count };
    }

    s_draw.cur_win  = win;
    s_draw.cur_z    = z;
    s_draw.cur_vp   = vp;
    s_draw.cur_band = band;
}

/*==============================================================================================
    draw_set_window -- stamp subsequent commands with the owning window's stable id (the retained-
    cache key).  Set to win->id in window_begin and back to 0 (background) in window_end; the popup
    layer saves/restores it around an overlay just like the sort key.
==============================================================================================*/

void
draw_set_window( gui_id_t win )
{
    draw_seg_retag( win, s_draw.cur_z, s_draw.cur_vp, s_draw.cur_band );
}

/*==============================================================================================
    draw_set_font -- make `font` the one every subsequent TEXT command is stamped with.

    Bookkeeping only: no segment is cut, and nothing else in the command list notices.  It used to
    cut one, on the reasoning that a font change is a texture switch and therefore a draw-batch
    seam.  Neither half of that survived.  Every font packs into the ONE shared coverage atlas, so
    changing font usually changes no texture whatsoever -- and the case that does (a distance-field
    font, which needs its own atlas because it must be sampled LINEAR) still cannot open a draw
    call, because the texture rides the vertex.  A segment split was buying nothing and costing a
    partition of the command list.

    So the font is what it always actually was -- the lookup a glyph run resolves its metrics and
    UVs from -- and it travels with the run.  Driven by gui_font_use (push / pop / use_font)
    alongside the layout metric rebuild.  A caller mixing regular and bold in one paragraph now
    costs exactly two text commands, not two command segments.
==============================================================================================*/

void
draw_set_font( u32 font )
{
    s_draw.cur_font = font;
}

/* Font subsequent TEXT commands are stamped with -- the save half of a scoped font swap. */

u32
draw_get_font( void )
{
    return s_draw.cur_font;
}

/*==============================================================================================
    draw_set_sort_key -- stamp subsequent commands with this z (window paint order).
    Set to the window's z in window_begin and back to 0 (background) in window_end.
==============================================================================================*/

void
draw_set_sort_key( u32 z )
{
    draw_seg_retag( s_draw.cur_win, z, s_draw.cur_vp, s_draw.cur_band );
}

/*==============================================================================================
    draw_set_viewport -- route subsequent commands to viewport `vp` (the surface a window paints).

    Set to the window's assigned viewport in window_begin and back to 0 (the main swapchain) in
    window_end, exactly as draw_set_sort_key drives the paint order.  flush replays only the
    commands tagged with its own viewport index, so one context can build every window's geometry
    and dispatch each window to the surface hosting it.
==============================================================================================*/

void
draw_set_viewport( i32 vp )
{
    draw_seg_retag( s_draw.cur_win, s_draw.cur_z, vp, s_draw.cur_band );
}

/*==============================================================================================
    draw_set_band -- route subsequent commands into arena band `band` (0 = main UI, 1 = debug).

    Set from GUI_WIN_DEBUG_BAND at the window/region begin seams and back to 0 at window_end,
    exactly as draw_set_sort_key drives the paint order.  The cache packs debug-band slots after
    every main-band slot and excludes them from stats + the any_changed idle-skip signal, so a
    self-measuring diagnostic never pollutes the arena layout or the metrics it displays.
==============================================================================================*/

void
draw_set_band( u32 band )
{
    draw_seg_retag( s_draw.cur_win, s_draw.cur_z, s_draw.cur_vp, band );
}

/* Current band -- saved/restored by the popup layer alongside the sort key, and sampled at popup
   begin so a popup/tooltip opened from inside a debug-band window inherits the band. */
u32
draw_band( void )
{
    return s_draw.cur_band;
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

/*==============================================================================================
    Global alpha -- a per-item opacity multiplier folded into every quad / triangle.

    draw_set_alpha installs the multiplier (clamped to [0,1]); draw_apply_alpha scales a packed
    color's A byte by it.  The item-flag resolver lowers it for the span of a disabled widget so
    the whole widget dims with no per-widget code, and the frame / chrome seams reset it to 1.0
    (chrome is not an item, so it always paints opaque).  At 1.0 the byte is returned unchanged.

    draw_get_alpha reads it back, which a caller needs to NEST a fade inside whatever the ambient
    span already installed -- multiply by the current value and restore it, rather than clobbering
    a disabled widget back to opaque halfway through its own paint.
==============================================================================================*/

void
draw_set_alpha( f32 a )
{
    s_draw.alpha = a < 0.0f ? 0.0f : ( a > 1.0f ? 1.0f : a );
}

f32
draw_get_alpha( void )
{
    return s_draw.alpha;
}

static u32
draw_apply_alpha( u32 abgr )
{
    if ( s_draw.alpha >= 1.0f ) return abgr;                /* opaque -- the common path */
    u32 a = ( abgr >> 24 ) & 0xFFu;
    a = (u32)( (f32)a * s_draw.alpha + 0.5f );              /* scale + round the alpha byte */
    return ( abgr & 0x00FFFFFFu ) | ( a << 24 );
}

/*==============================================================================================
    Corner rounding -- the ambient radius folded into filled / outlined rects.

    draw_set_rounding installs the radius (clamped non-negative); draw_clamp_rounding fits it to a
    given rect so a corner arc never exceeds half a side, and treats a sub-pixel result as 0 so thin
    bars (separators, 1px frames) stay crisply square.  The item / chrome seams drive the radius from
    the resolved rounding category, exactly as draw_set_alpha is driven; grabs and squared marks set
    it locally for one sub-element via draw_set_rounding / draw_rounding (save + restore).
==============================================================================================*/

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

/* The corner PROFILE that rides with the radius: how much of the corner is curved, rather than
   how big the curve is.  0 leaves the arc circular, 1 ramps its curvature across the whole
   corner -- the same control Figma spells "corner smoothing" and the shape a rounded rect has to
   have before it can sit next to a modern OS's chrome without looking pinched.
   The 0..1 knob becomes an exponent HERE, once, because that is the number the field is measured
   with (gui.h): 2 is the circle, and the useful range stops well before the corner is square. */
void
draw_set_corner_smooth( f32 t )
{
    if ( t <= 0.0f )
    {
        s_draw.corner_pow = 0.0f;    /* circular -- the fragment's default branch */
        return;
    }
    if ( t > 1.0f )
        t = 1.0f;
    s_draw.corner_pow = 2.0f + 4.0f * t;
}

/* The curve every animating shape pushed after this call is shaped by, and the parameter that
   curve reads (an exponent, a step count, a duty -- gui_curve_t says which).  Ambient like the
   radius: set it around the shapes it should affect and restore it after.
   GUI_CURVE_LINEAR, the default, means the phase drives the effect unshaped. */
void
draw_set_anim_curve( u32 curve, f32 param )
{
    s_draw.anim_curve       = curve;
    s_draw.anim_curve_param = param;
}

void
draw_get_anim_curve( u32* curve, f32* param )
{
    if ( curve ) *curve = s_draw.anim_curve;
    if ( param ) *param = s_draw.anim_curve_param;
}

/* The cycle offset every animating shape pushed after this is anchored by, in cycles, added to
   whatever offset the call itself states.  This is how a one-shot reaches the draws that take no
   phase of their own -- the spinner, the marching ants. */
void
draw_set_anim_phase( f32 cycles )
{
    s_draw.anim_phase = cycles;
}

f32
draw_anim_phase( void )
{
    return s_draw.anim_phase;
}

/* The installed profile as the 0..1 amount that was authored, so a site can save / override /
   restore it the way it already does with the radius. */
f32
draw_corner_smooth( void )
{
    return ( s_draw.corner_pow <= 2.0f ) ? 0.0f : ( s_draw.corner_pow - 2.0f ) * 0.25f;
}

/* Where a stroked box's band sits against its boundary: 0 inside (the default), 0.5 centred,
   1 outside -- the alignment every design tool offers on a stroke.  Ambient like the radius,
   save/restore around the shapes it should affect.  Resolved at push time by inflating the
   shape by align * width (see s_draw.border_align), so it costs nothing downstream. */
void
draw_set_border_align( f32 a )
{
    s_draw.border_align = a < 0.0f ? 0.0f : ( a > 1.0f ? 1.0f : a );
}

f32
draw_border_align( void )
{
    return s_draw.border_align;
}

/*==============================================================================================
    Text edge -- the ambient second colour painted OUTSIDE the glyph boundary.

    An outline and a drop shadow are the same thing to the fragment: widen the glyph's own distance
    field by `width` pixels and fill the band that opens up.  So this costs a width and a colour on
    the text command and nothing else -- no second run, no offset copy, no extra draw.

    It needs a DISTANCE FIELD to widen, so it applies to SDF fonts only (gui_font_t.sdf_range > 0);
    a coverage font has no signed distance to offset and simply ignores the word.  Practical width
    is bounded by the spread baked into the atlas, past which the field is flat and the outline
    stops growing.  Set with a width of 0 to clear -- bracketed save/restore like draw_set_rounding.
==============================================================================================*/

void
draw_set_text_edge( f32 width, u32 abgr )
{
    s_draw.text_edge_w   = ( width > 0.0f ) ? width : 0.0f;
    s_draw.text_edge_col = ( width > 0.0f ) ? abgr  : 0u;
}

/* Does the ambient edge paint anything?  Both halves matter: a zero width has no band to fill, and
   a transparent colour fills it with nothing.  Read by the transparent-fill drop, which must keep
   an outline-only run alive. */
static bool
draw_text_edge_visible( void )
{
    return s_draw.text_edge_w > 0.0f && ( s_draw.text_edge_col >> 24 ) != 0u;
}

/* Read the ambient pair back, for bracketed save/restore.  It used to be one packed word plus a
   _raw setter that put it back without re-quantizing; the record stores a plain width and colour,
   so a round trip through draw_set_text_edge is now exact and the second setter is gone. */
void
draw_text_edge( f32* out_width, u32* out_abgr )
{
    if ( out_width ) *out_width = s_draw.text_edge_w;
    if ( out_abgr  ) *out_abgr  = s_draw.text_edge_col;
}

/*==============================================================================================
    Ambient text-clip window: a horizontal [x0, x1] pixel window that every subsequent
    draw_push_text / draw_push_text_n hard-clips to at the glyph level (straddling glyphs sliced
    with remapped U, interior glyphs whole, no scissor / no batch split).  A seam that draws text
    into a bounded slot -- a table cell at the scroll viewport edge -- sets the window for the
    span and clears it after, exactly as draw_set_alpha / draw_set_rounding bracket their spans.
    Explicit draw_push_text_clip_n callers (the scrolled text input) bypass this and pass their own.
==============================================================================================*/

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

/*==============================================================================================
    Draw scope -- the paint cursor as one record (gui_draw_scope_t, gui_render.h): the command
    segment tag (window, sort key, viewport, band -- the ambient font stays global by design)
    plus the ambient text-clip window above.  The overlay seam (overlay_detach / overlay_reattach,
    gui_popup.c) saves and restores it wholesale; the restore is a single segment retag.
==============================================================================================*/

gui_draw_scope_t
draw_scope( void )
{
    return ( gui_draw_scope_t ){ .window       = s_draw.cur_win,
                                 .sort_key     = s_draw.cur_z,
                                 .viewport     = s_draw.cur_vp,
                                 .band         = s_draw.cur_band,
                                 .text_clip_x0 = s_draw.text_clip_x0,
                                 .text_clip_x1 = s_draw.text_clip_x1 };
}

void
draw_scope_set( gui_draw_scope_t s )
{
    draw_seg_retag( s.window, s.sort_key, s.viewport, s.band );
    s_draw.text_clip_x0 = s.text_clip_x0;
    s_draw.text_clip_x1 = s.text_clip_x1;
}

/*  Fit a radius to a rect: no corner may eat more than half of either extent, and a radius the
    clamp leaves under half a pixel is not a corner at all.  Split from the ambient reader below so
    the palette bake can ask what a given radius WOULD become over a given rect without touching the
    paint cursor (gui_render_bake.c). */

f32
draw_clamp_round_of( f32 r, f32 w, f32 h )
{
    f32 hw = ( w < 0.0f ? -w : w ) * 0.5f;
    f32 hh = ( h < 0.0f ? -h : h ) * 0.5f;
    if ( r > hw ) r = hw;
    if ( r > hh ) r = hh;
    return r < 0.5f ? 0.0f : r;   /* sub-pixel radius -> square fast path */
}

static f32
draw_clamp_rounding( f32 w, f32 h )
{
    return draw_clamp_round_of( s_draw.rounding, w, h );
}

// clang-format on
/*============================================================================================*/
