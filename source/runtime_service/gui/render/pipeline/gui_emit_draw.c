/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_emit_draw.c -- Draw list accumulation.

    All geometry goes through the draw_push_* entry points, which append semantic gui_cmd_t
    records (no vertices yet).  GPU batching happens later, at tessellation time, in
    tess_ensure_gpu_cmd (gui_build_tess.c).  draw_push_text copies its string into the frame
    text pool and emits one glyph-run command.

    draw_push_icon lives here rather than with the icon resource: it queues a semantic command
    exactly like every other draw_push_*, so EMIT stays the one place a command is born and the
    resource never reaches up into it.

    First of the pipeline includes in gui_render.c.  The resolvers it calls -- font_glyph,
    icon_get, icon_atlas_idx -- are NOT in this unit: fonts and icons are the draw unit's, and
    the server reaches them through the glyph/sprite source contract declared in
    render/gui_render.h, which the draw unit implements.  Nothing here depends on include order
    for them.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    gui_gpu_cmd_t -- backend-private GPU draw command.

    One bounded range of indices sharing a texture slot and scissor rect -- the unit the GPU
    sees.  Not exposed in gui.h.  The public gui_cmd_t carries semantic shapes; the BUILD phase
    (gui_build_tess.c) tessellates those into these.
==============================================================================================*/

typedef struct
{
    u32          elem_count; // number of indices to emit
    /* The texture of the command's FIRST primitive, kept for diagnostics only (the dashboard
       tooltip).  It is no longer a batch key and no longer describes the whole command: the
       texture rides the vertex now (gui.h, gui_draw_vert_t), so one command can span several. */
    u32          tex_idx;    // first primitive's model|slot -- diagnostic, not a batch key
    gui_rect_t   clip_rect;  // scissor rect (pixels)

} gui_gpu_cmd_t;

/*==============================================================================================
    Draw list -- the per-frame command buffer and segment table.

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
       arc is measured in (gui.h, gui_prim_t param_c).  Held as the exponent rather than as the
       0..1 smoothing a caller authors, so the conversion happens once in draw_set_corner_smooth
       instead of per pushed shape.  0 = circular, which is what every shape gets until a theme
       says otherwise -- it is installed once per frame from GUI_VAR_CORNER_SMOOTH, so a caller
       only touches it to override one shape. */
    f32 corner_pow;

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
    draw_hash_cmd below also uses them; fnv1a_u32 is visible in gui_build_cache.c (included
    after this file by gui_render.c).
==============================================================================================*/

static inline u32
fnv1a( u32 h, const void* p, u32 n )
{
    const u8* b = (const u8*)p;
    for ( u32 i = 0; i < n; ++i ) h = ( h ^ b[ i ] ) * 16777619u;
    return h;
}

/* fnv1a_u32 -- fold one u32 into h: 4 explicit byte mixes, no loop or function-call overhead.
   Used wherever a u32 value is the unit being folded (segment metadata, pre-baked clip hashes,
   per-command hash accumulation) so the inner loops in cache_diff_windows stay branch-free. */
static inline u32
fnv1a_u32( u32 h, u32 v )
{
    h = ( h ^ (u8)( v       ) ) * 16777619u;
    h = ( h ^ (u8)( v >>  8 ) ) * 16777619u;
    h = ( h ^ (u8)( v >> 16 ) ) * 16777619u;
    h = ( h ^ (u8)( v >> 24 ) ) * 16777619u;
    return h;
}

/*==============================================================================================
    COMMAND STEPPING

    draw_emit_blocked -- the one gate every draw_push_* entry point checks before spending a
    command slot (and, by early-outing first, any pool space): the command list is full, or the
    command stepper is replaying a frozen frame and live main-band emission is suppressed at the
    source (STEP_EMIT_SUPPRESSED, gui_render.h; capture/replay in render/gui_step_capture.c).
==============================================================================================*/

static inline bool
draw_emit_blocked( void )
{
    if ( s_draw.cmd_count >= GUI_MAX_CMDS )
    {
        /* Say so.  Every other fixed pool in the gui follows the loud-overflow rule (GUI_WARN_ONCE,
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

    if ( STEP_EMIT_SUPPRESSED() )
        return true;

#ifdef GUI_CMD_STEPPER
    /* Attribution: stamp the emitting widget onto the slot this push will occupy.  Speculative
       (the push may still drop for alpha/cull), but exact: every push checks this gate first,
       so whichever push finally lands at this index was also the last stamper. */

    s_draw.cmd_owner[ s_draw.cmd_count ] = s_draw.cur_owner;

#endif

    return false;
}

#ifdef GUI_CMD_STEPPER

/* Called by item_state (STEP_SET_OWNER, gui_render.h) as each widget registers -- the
   commands it paints right after carry its id.  Reset to 0 (chrome) at window transitions in
   draw_seg_retag; chrome painted after a window's last widget still attributes to that widget
   (a known display-only imprecision). */
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
    Clip stack
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

/* Reject a shape whose axis-aligned bounds cannot touch the current clip -- it would emit a command
   + geometry the GPU then scissors to nothing.  The cull is exact, not heuristic: cur_clip_idx
   records the active scissor at every emit, so clip_current() IS the scissor the shape renders
   under; a box fully outside it lights no pixel.  This rejects at the source -- a scrolled-out widget
   (or a whole region clipped to zero) costs no command slot, no string-pool / point-pool space, and
   no tessellation, not merely no draw call.  Conservative: only a box fully past an edge is dropped
   (touching counts as visible), and an empty clip rejects everything in it. */
bool
draw_cull_box( f32 x, f32 y, f32 w, f32 h )
{
    gui_rect_t c = clip_current();
    if ( rect_empty( c ) )                  return true;   /* nothing in an empty clip is visible */
    if ( x + w <= c.x || x >= c.x + c.w )   return true;   /* fully left / right of the clip      */
    if ( y + h <= c.y || y >= c.y + c.h )   return true;   /* fully above / below the clip        */
    return false;
}

/* The shared push body.  `radius` rounds the clip's own corners -- the per-fragment cut the
   scissor could never express (gui.ps.hlsl, clip_coverage).  It applies to THIS entry only: a
   clip nested inside a rounded one intersects against the parent's RECT (the corner arcs do not
   compose through rect_intersect), which errs by letting a child paint into its parent's corner
   arc -- the parent's own chrome overpaints there in practice. */
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

static f32
draw_clamp_rounding( f32 w, f32 h )
{
    f32 r  = s_draw.rounding;
    f32 hw = ( w < 0.0f ? -w : w ) * 0.5f;
    f32 hh = ( h < 0.0f ? -h : h ) * 0.5f;
    if ( r > hw ) r = hw;
    if ( r > hh ) r = hh;
    return r < 0.5f ? 0.0f : r;   /* sub-pixel radius -> square fast path */
}

/*==============================================================================================
    draw_push_rect_filled / draw_push_image -- emit a filled / textured quad semantic command.

    tex_idx == 0 is the solid-color convention (resolved to the atlas white texel at tessellation
    time).  Pixel-grid snapping and GPU batching happen at flush time in the tessellation pass.

    Two entry points over one body, differing only in whether the ambient rounding radius applies.
    See the comments on each below -- the split is about what the quad MEANS (picture vs glyph),
    which only the caller knows, not about what it carries.
==============================================================================================*/

/*==============================================================================================
    FNV-1a hash helper and per-command hash used by the retained cache.

    draw_hash_cmd hashes a fully-filled gui_cmd_t at emit time while the data is still
    L1-hot.  The hash is stored in s_draw.cmd_hashes and folded per window by
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
    [GUI_CMD_RECT_FILLED]   = sizeof( ( (gui_cmd_t*)0 )->rect ),
    [GUI_CMD_RECT_OUTLINE]  = sizeof( ( (gui_cmd_t*)0 )->rect_outline ),
    [GUI_CMD_TRIANGLE]      = sizeof( ( (gui_cmd_t*)0 )->tri ),
    [GUI_CMD_LINE]          = sizeof( ( (gui_cmd_t*)0 )->line ),
    [GUI_CMD_DASHED_LINE]   = sizeof( ( (gui_cmd_t*)0 )->dash ),
    [GUI_CMD_RECT_GRADIENT] = sizeof( ( (gui_cmd_t*)0 )->gradient ),
    [GUI_CMD_SPRITE]        = sizeof( ( (gui_cmd_t*)0 )->sprite ),
    /* Folds rate whole, so a pulse hashes stable frame-to-frame (it animates in the FRAGMENT off
       pc.time -- the geometry never changes, which is the entire point of the mode). */
    [GUI_CMD_FX_BOX]        = sizeof( ( (gui_cmd_t*)0 )->fx_box ),
    [GUI_CMD_ROUND_RECT_EX] = sizeof( ( (gui_cmd_t*)0 )->round_rect ),
    /* Both sectors fold the same member.  A spinner's start angle moves every frame, so this
       dirties every frame -- honestly, since the geometry really does rotate.  (A spinner that
       wanted free animation would be a shader-clock mode like PULSE, not a re-emit.) */
    [GUI_CMD_ARC]           = sizeof( ( (gui_cmd_t*)0 )->arc ),
    [GUI_CMD_PIE]           = sizeof( ( (gui_cmd_t*)0 )->arc ),
    [GUI_CMD_ARC_DASH]      = sizeof( ( (gui_cmd_t*)0 )->arc_dash ),
    [GUI_CMD_ARC_GRAD]      = sizeof( ( (gui_cmd_t*)0 )->arc_grad ),
    [GUI_CMD_IMAGE_XF]      = sizeof( ( (gui_cmd_t*)0 )->image_xf ),
    [GUI_CMD_CHECKER]       = sizeof( ( (gui_cmd_t*)0 )->checker ),
    [GUI_CMD_GRID]          = sizeof( ( (gui_cmd_t*)0 )->grid ),
    [GUI_CMD_NGON]          = sizeof( ( (gui_cmd_t*)0 )->ngon ),
    /* Folds rate/phase whole like FX_BOX: the ants scroll in the fragment off pc.time, so the
       command hashes stable frame-to-frame while the pattern moves. */
    [GUI_CMD_BOX_DASH]      = sizeof( ( (gui_cmd_t*)0 )->box_dash ),
};

static u32
draw_hash_cmd( const gui_cmd_t* c )
{
    /* Fold type+vp (packed into one u32) then the pre-baked clip hash.  The clip value -- not
       the index -- is what matters so the same rect produces the same hash regardless of which
       table slot it occupies this frame.  clip_hash_cache[i] is baked at push time (4 bytes
       folded here vs 16 for the raw rect).  z is per-segment, folded in cache_diff_windows. */
    u32 h  = 2166136261u;
    u32 tv = (u32)c->type | ( (u32)c->vp << 8 );
    h = fnv1a_u32( h, tv );
    h = fnv1a_u32( h, s_draw.clip_hash_cache[ c->clip_idx ] );
    switch ( c->type )
    {
        case GUI_CMD_TEXT:
            h = fnv1a( h, &c->text.x,       sizeof c->text.x );
            h = fnv1a( h, &c->text.y,       sizeof c->text.y );
            h = fnv1a( h, &c->text.len,     sizeof c->text.len );
            h = fnv1a( h, &c->text.clip_x0, sizeof c->text.clip_x0 );
            h = fnv1a( h, &c->text.clip_x1, sizeof c->text.clip_x1 );
            h = fnv1a( h, &c->text.abgr,    sizeof c->text.abgr );
            h = fnv1a( h, &c->text.edge_w,   sizeof c->text.edge_w );
            h = fnv1a( h, &c->text.edge_col, sizeof c->text.edge_col );
            h = fnv1a( h, &c->text.font,    sizeof c->text.font );
            h = fnv1a( h, s_draw.text_pool + c->text.off, c->text.len );   /* content while L1-hot */
            break;
        /* Folds scale and rot, so a run that spins re-tessellates every frame it moves.  That is
           the honest cost and the difference from PULSE: a pulse animates in the FRAGMENT off
           pc.time and its geometry never changes, while a transform is baked into vertices. */
        case GUI_CMD_TEXT_XF:
            h = fnv1a( h, &c->text_xf.x,     sizeof c->text_xf.x );
            h = fnv1a( h, &c->text_xf.y,     sizeof c->text_xf.y );
            h = fnv1a( h, &c->text_xf.len,   sizeof c->text_xf.len );
            h = fnv1a( h, &c->text_xf.scale, sizeof c->text_xf.scale );
            h = fnv1a( h, &c->text_xf.rot,   sizeof c->text_xf.rot );
            h = fnv1a( h, &c->text_xf.abgr,  sizeof c->text_xf.abgr );
            h = fnv1a( h, &c->text_xf.edge_w,   sizeof c->text_xf.edge_w );
            h = fnv1a( h, &c->text_xf.edge_col, sizeof c->text_xf.edge_col );
            h = fnv1a( h, &c->text_xf.font,  sizeof c->text_xf.font );
            h = fnv1a( h, s_draw.text_pool + c->text_xf.off, c->text_xf.len );
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
            h = fnv1a( h, &c->rect, k_cmd_hash_len[ c->type ] );
            break;
    }
    return h;
}

/*==============================================================================================
    draw_cmd_open / draw_cmd_seal -- the shared preamble and postamble of every shape push.

    open runs the four gates every push owes, in the one order that is correct (blocked first so
    the stepper's owner stamp lands; a fully transparent shape contributes nothing under alpha
    blending, src*0 + dst = dst; the cull is exact against the active scissor), then allocates
    the slot and stamps the header.  `vis_col` is the shape's colour with the ambient alpha
    ALREADY folded (a multi-colour shape passes the OR of its folded colours -- visible if any
    end is).  `pad` grows the cull box on every side for shapes whose geometry reaches past the
    authored rect (the SDF AA skirt, a shadow's feather).  Returns NULL when the shape must not
    spend a slot; otherwise the caller fills the payload and calls seal, which bakes the
    retained-cache hash while the bytes are L1-hot.

    The four pool-backed pushes (text, text_xf, polyline via gui_emit_path.c, rect_list) keep
    their own preambles: each has a pool copy that must succeed BEFORE a slot may be spent, and
    a cull that is not an axis-aligned box test.  They still owe the same transparent drop this
    preamble runs -- alpha 0 is the free visibility toggle everywhere, with one text nuance: a
    visible TEXT_EDGE keeps a transparent-fill run alive (the outline paints outside the glyph).
==============================================================================================*/

/* Claim the next command slot and stamp the ambient (clip_idx, vp) pair onto it.  vp is the
   batch key; clip_idx names the rect the tessellator resolves into the slot's local clip table
   (the vertex clip band -- a clip change cannot cut a draw call either).  Stamping both is the
   one thing every command must do and no command may get wrong.  Split out of draw_cmd_open
   below because the pool-backed pushes cannot use that function's preamble (their pool copy has
   to succeed before a slot is spent, and their cull is not an axis-aligned box test) but they owe
   the identical stamp.  Seven sites open-coded these four lines before it had a name. */

static gui_cmd_t*
draw_cmd_claim( u8 type )
{
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

/* The shared body.  `rounding` arrives explicit and already resolved -- the wrappers below fold
   the ambient radius in (or not: see the roundable rule on each), and the disc passes its own.
   `corner_pow` travels the same way and for the same reason: a disc's corner IS the shape, so
   the one caller that names its own radius names its own profile too, and gets the circle. */
static void
draw_rect_cmd( f32 x, f32 y, f32 w, f32 h,
               f32 u0, f32 v0, f32 u1, f32 v1,
               u32 tex_idx, u32 abgr, f32 rounding, f32 corner_pow )
{
    /* A rounded quad becomes an SDF surface whose AA skirt reaches past the authored rect
       (tess_fx_box), so cull it with one pixel of slack -- a shape flush against the clip edge
       keeps its feathered edge.  Square quads cull tight. */
    f32 pad = ( rounding > 0.0f ) ? 1.0f : 0.0f;
    u32 col = draw_apply_alpha( abgr );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_RECT_FILLED, col, x, y, w, h, pad );
    if ( !c )
        return;
    c->rect.x        = x;
    c->rect.y        = y;
    c->rect.w        = w;
    c->rect.h        = h;
    c->rect.u0       = u0;
    c->rect.v0       = v0;
    c->rect.u1       = u1;
    c->rect.v1       = v1;
    c->rect.tex_idx  = tex_idx;
    c->rect.abgr       = col;
    c->rect.rounding   = rounding;
    c->rect.corner_pow = ( rounding > 0.0f ) ? corner_pow : 0.0f;
    draw_cmd_seal();
}

/*  The general quad.  Rounds SOLID fills only, and the reason is not that the tessellator cannot
    round a texture -- it can, and tess_fx_box interpolates UVs across the authored box and clamps
    them over the falloff skirt precisely so a rounded textured quad cannot bleed into its atlas
    neighbour.  The reason is what else comes through here: draw_push_icon routes every icon quad
    to this function, and an icon is a COVERAGE glyph, not a picture.  Rounding it would cut the
    corners off the symbol rather than off a frame, and it would happen silently to any icon that
    happened to be drawn inside a draw_set_rounding scope.
    A caller that really is drawing a picture says so by calling draw_push_image below. */
void
draw_push_rect_filled( f32 x, f32 y, f32 w, f32 h,      /* rect */
                       f32 u0, f32 v0, f32 u1, f32 v1,  /* uv */
                       u32 tex_idx, u32 abgr )          /* texture slot + color */
{
    draw_rect_cmd( x, y, w, h, u0, v0, u1, v1, tex_idx, abgr,
                   ( tex_idx == 0 ) ? draw_clamp_rounding( w, h ) : 0.0f, s_draw.corner_pow );
}

/*  An IMAGE: an arbitrary bindless texture the caller is showing as a picture (a scene render
    target, a loaded photo).  Identical to the above in every respect except that the ambient
    radius reaches it, which is the whole point of the split -- "a picture can have rounded
    corners, a glyph cannot" is the rule, and the call site is the only place that knows which of
    the two it is holding.  The rounded corner is exact, not a mask: the fragment resolves the
    boundary from the same signed-distance field a rounded fill uses, with the texture sampling
    underneath it (gui.h, the effect band). */
void
draw_push_image( f32 x, f32 y, f32 w, f32 h,
                 f32 u0, f32 v0, f32 u1, f32 v1,
                 u32 tex_idx, u32 abgr )
{
    draw_rect_cmd( x, y, w, h, u0, v0, u1, v1, tex_idx, abgr,
                   draw_clamp_rounding( w, h ), s_draw.corner_pow );
}

/*==============================================================================================
    draw_push_circle_filled -- a filled disc, which IS a rounded rect whose radius reached the
    half-extent.  Not a command type of its own: the tessellator already derives everything a
    disc needs from that shape (the SDF boundary, and the no-snap rule -- a square box whose
    radius reached its half-extent has no straight edge to keep crisp, and quantizing a moving
    dot's centre is exactly what must not happen).  The radius is passed EXPLICIT, bypassing the
    ambient rounding -- a disc is fully round by definition, not by scope.
==============================================================================================*/

void
draw_push_circle_filled( f32 cx, f32 cy, f32 r, u32 abgr )
{
    draw_rect_cmd( cx - r, cy - r, r * 2.0f, r * 2.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0, abgr, r, 0.0f );
}

/*==============================================================================================
    draw_push_rect_list -- emit N solid rects as ONE semantic command.

    The dense-shape escape valve: a caller drawing hundreds of small fills (timeline bars, graph
    columns, heatmap cells) would otherwise spend one command slot per rect and exhaust
    GUI_MAX_CMDS long before the vertex budget.  Entries are copied into the per-frame rect pool
    (the CMD_POLYLINE point-pool pattern) and tessellated into one quad each at flush time.
    Per-entry alpha fold + clip cull happens here so the pool holds only visible work.  Entries
    share the current clip; always square (no rounding), solid color (white texel).
==============================================================================================*/

void
draw_push_rect_list( const gui_rect_col_t* rects, u32 count )
{
    if ( !rects || draw_emit_blocked() )
        return;

    u32 offset = s_draw.rect_count;
    for ( u32 i = 0; i < count && s_draw.rect_count < GUI_MAX_RECT_ENTRIES; ++i )
    {
        u32 col = draw_apply_alpha( rects[ i ].abgr );
        if ( ( col >> 24 ) == 0u )   /* invisible under alpha blending (draw_push_rect_filled rule) */
            continue;
        if ( draw_cull_box( rects[ i ].x, rects[ i ].y, rects[ i ].w, rects[ i ].h ) )
            continue;
        s_draw.rect_pool[ s_draw.rect_count ]      = rects[ i ];
        s_draw.rect_pool[ s_draw.rect_count ].abgr = col;
        s_draw.rect_count++;
    }
    if ( s_draw.rect_count == offset )
        return;   /* everything culled: no command slot spent */

    gui_cmd_t* c        = draw_cmd_claim( GUI_CMD_RECT_LIST );
    c->rect_list.offset = offset;
    c->rect_list.count  = s_draw.rect_count - offset;
    draw_cmd_seal();   /* entries are L1-hot here */
}

/*==============================================================================================
    draw_push_icon -- push one registered icon quad into the draw list.

    An icon is just a textured quad sourced from the icon atlas instead of the font atlas, so
    this reuses draw_push_rect_filled wholesale; icon_get (the sprite source contract, supplied
    by the draw unit) hands back the cached UVs.  No-op for an invalid id.

    The texture comes from icon_tex( id ) rather than res_atlas_idx(), which is the entire draw-side
    cost of icons being able to be distance fields: an icon names its own atlas AND its own sampling
    model, and one draw call still holds a coverage icon, an SDF icon, a glyph run and a fill.
==============================================================================================*/

void
draw_push_icon( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, u32 abgr )
{
    f32 u0, v0, u1, v1;
    if ( !icon_get( id, &u0, &v0, &u1, &v1, NULL, NULL ) )
        return;
    u32 tex = icon_tex( id );
    if ( tex == 0u )
        return;   /* SDF atlas not stood up yet -- skip the quad, as a glyph run does */
    draw_push_rect_filled( x, y, w, h, u0, v0, u1, v1, tex, abgr );
}

/*==============================================================================================
    draw_push_image_xf / draw_push_icon_xf -- one textured quad turned about its centre.

    The text_xf treatment applied to a single quad: four positions rotate, the UVs interpolate
    exactly as they would upright, and what makes the result legible at any angle is the sampling
    model riding the tex word -- an SDF icon resolves its edge from the screen-space derivative
    and turns cleanly, a coverage icon shows its texels (the same rule the two font bakes follow).
    Compass needles, minimap markers, spinner glyphs.
==============================================================================================*/

void
draw_push_image_xf( f32 x, f32 y, f32 w, f32 h,
                    f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, f32 rot, u32 abgr )
{
    /* Rotated-AABB cull, the draw_push_box_xf rule; the quad has no skirt, so 1 px of slack. */
    f32 cs = cosf( rot ), sn = sinf( rot );
    f32 hx = w * 0.5f, hy = h * 0.5f;
    f32 ex = fabsf( hx * cs ) + fabsf( hy * sn );
    f32 ey = fabsf( hx * sn ) + fabsf( hy * cs );
    u32 col = draw_apply_alpha( abgr );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_IMAGE_XF, col,
                                  x + hx - ex, y + hy - ey, ex * 2.0f, ey * 2.0f, 1.0f );
    if ( !c )
        return;
    c->image_xf.x       = x;
    c->image_xf.y       = y;
    c->image_xf.w       = w;
    c->image_xf.h       = h;
    c->image_xf.u0      = u0;
    c->image_xf.v0      = v0;
    c->image_xf.u1      = u1;
    c->image_xf.v1      = v1;
    c->image_xf.rot     = rot;
    c->image_xf.tex_idx = tex_idx;
    c->image_xf.abgr    = col;
    draw_cmd_seal();
}

void
draw_push_icon_xf( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, f32 rot, u32 abgr )
{
    f32 u0, v0, u1, v1;
    if ( !icon_get( id, &u0, &v0, &u1, &v1, NULL, NULL ) )
        return;
    u32 tex = icon_tex( id );
    if ( tex == 0u )
        return;   /* SDF atlas not stood up yet -- skip the quad, as draw_push_icon does */
    draw_push_image_xf( x, y, w, h, u0, v0, u1, v1, tex, rot, abgr );
}

/*==============================================================================================
    draw_push_sprite -- emit one sprite quad (optionally nine-sliced) as ONE semantic command.

    The command carries the sprite ID, not its UVs.  An icon resolves at emit because a quad is all
    it will ever be; a sprite must not, for two reasons that both point the same way: the slice
    expansion needs the source's pixel size and insets (which only the registry has), and a
    sprite-atlas repack moves UVs under a command that may live in the retained cache for many
    frames.  Resolving in the tessellator puts both facts in one place and lets the ordinary
    generation-fold re-tessellate path correct a repack with no re-emit.

    One command however many quads it becomes, which is the point: a nine-slice frame costs one
    command slot and one batch, so a window can afford an authored border on every panel.
==============================================================================================*/

void
draw_push_sprite( f32 x, f32 y, f32 w, f32 h, gui_sprite_id_t id,
                  u32 abgr, f32 scale, u16 flags, bool nine )
{
    if ( id == GUI_SPRITE_NONE )
        return;

    /* A tint of 0 means UNTINTED -- the sprite's own colours at full alpha (the gui_brush_t rule).
       Only an explicit tint can fade a sprite, and one faded to zero alpha is invisible under
       blending, so it drops exactly as a transparent fill does. */
    u32 col = draw_apply_alpha( abgr ? abgr : 0xFFFFFFFFu );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_SPRITE, col, x, y, w, h, 0.0f );
    if ( !c )
        return;
    c->sprite.x      = x;
    c->sprite.y      = y;
    c->sprite.w      = w;
    c->sprite.h      = h;
    c->sprite.scale  = ( scale > 0.0f ) ? scale : 1.0f;
    c->sprite.sprite = id;
    c->sprite.abgr   = col;
    c->sprite.flags  = flags;
    c->sprite.nine   = nine ? 1u : 0u;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_rect_gradient -- emit a two-color gradient rectangle as one semantic command.

    col_a / col_b sit on opposite edges (horizontal = left->right, else top->bottom); the GPU
    interpolates the per-vertex color between them, so one quad replaces the old banded fill.
    Always square (no rounding) -- the per-vertex blend has no rounded-fan variant.
==============================================================================================*/

void
draw_push_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    /* Visible if EITHER end is: the OR'd alpha is the visibility word draw_cmd_open tests. */
    u32 ca = draw_apply_alpha( col_a );
    u32 cb = draw_apply_alpha( col_b );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_RECT_GRADIENT, ca | cb, x, y, w, h, 0.0f );
    if ( !c )
        return;
    c->gradient.x          = x;
    c->gradient.y          = y;
    c->gradient.w          = w;
    c->gradient.h          = h;
    c->gradient.col_a      = ca;
    c->gradient.col_b      = cb;
    c->gradient.horizontal = horizontal;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_shadow / draw_push_pulse -- the two faces of GUI_CMD_FX_BOX, one body.

    A shadow is the surface with a WIDE feather: the falloff band straddles the shape's boundary
    (solid feather/2 inside the box, gone feather/2 outside), and because the effect travels per
    vertex it merges into whatever GPU batch is already open -- a shadow behind every floating
    panel costs no draw calls.

    draw_push_skirt is the same surface with its interior cut away (GUI_OP_CUT) -- identical
    outward falloff, nothing painted inside the boundary.  That is what a DROP shadow is: the core
    of a filled one can only ever be seen through the thing casting it, so on a translucent panel
    it reads as the panel dimming itself.  Cutting it also makes the tessellator's interior hole
    unconditional, taking a window-sized plate down to a band of quads around the frame.  Keep
    draw_push_shadow for a glow or halo that is MEANT to be seen through its subject.

    A pulse is the surface whose alpha breathes on pc.time in the FRAGMENT.  Geometrically it is
    a plain rounded fill, and that identity is the feature: the command's bytes never change, so
    its hash never changes, so the window's cached geometry stays valid and the pulse costs zero
    re-tessellation while it runs.  `rate` is in Hz,
    `depth` the 0..1 fraction of alpha removed at the trough.  The caller still owes one
    request_redraw per frame: the clock advancing is not what schedules a frame (GUI_FX_TIME_WRAP).
==============================================================================================*/

static void
draw_fx_box_cmd( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, u32 variant,
                 f32 rate, f32 depth, f32 phase, f32 rot, f32 cut_dx, f32 cut_dy, u32 abgr )
{
    /* Cull against the GROWN box: the falloff skirt is real geometry (feather/2 past the rect,
       plus the tessellator's pixel of slack), and a shadow whose box is just off screen still
       paints a band on it.  A rotated box culls against its rotated AABB -- computed here rather
       than approximated with the diagonal, because the exact box is four multiplies and the
       diagonal wrongly keeps every long thin rotated bar on screen. */
    f32 pad = feather * 0.5f + 1.0f;
    f32 bx = x, by = y, bw = w, bh = h;
    if ( rot != 0.0f )
    {
        f32 cs = cosf( rot ), sn = sinf( rot );
        f32 hx = w * 0.5f, hy = h * 0.5f;
        f32 ex = fabsf( hx * cs ) + fabsf( hy * sn );
        f32 ey = fabsf( hx * sn ) + fabsf( hy * cs );
        bx = x + hx - ex;  by = y + hy - ey;
        bw = ex * 2.0f;    bh = ey * 2.0f;
    }
    u32 col = draw_apply_alpha( abgr );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_FX_BOX, col, bx, by, bw, bh, pad );
    if ( !c )
        return;
    c->fx_box.x        = x;
    c->fx_box.y        = y;
    c->fx_box.w        = w;
    c->fx_box.h        = h;
    c->fx_box.rounding = rounding;
    c->fx_box.corner_pow = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    c->fx_box.feather  = feather;
    c->fx_box.rate     = rate;
    c->fx_box.depth    = depth;
    c->fx_box.phase    = phase;
    c->fx_box.rot      = rot;
    c->fx_box.abgr     = col;
    c->fx_box.variant  = variant;
    c->fx_box.cut_dx   = cut_dx;
    c->fx_box.cut_dy   = cut_dy;
    draw_cmd_seal();
}

void
draw_push_shadow( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, feather, 0u, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

/* x,y,w,h is the CASTER; (ox, oy) is how far the shadow falls from it.  The command carries the
   shadow's own rect, so the cut offset is the trip back to the caster -- the shape states where it
   is drawn and the offset states what it is drawn under. */
void
draw_push_skirt( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather,
                 f32 ox, f32 oy, u32 abgr )
{
    draw_fx_box_cmd( x + ox, y + oy, w, h, rounding, feather, 1u,
                     0.0f, 0.0f, 0.0f, 0.0f, -ox, -oy, abgr );
}

/* The inner shadow: the same surface with its falloff turned inward (GUI_OP_INSET), painting
   from the boundary `depth` px in and nothing outside it.  A pressed well, a recessed field, the
   inner edge of a scroll area -- the shapes a drop shadow cannot make because they belong to the
   inside of their subject rather than to the ground under it. */
void
draw_push_inset( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 depth, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, depth, 2u, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, abgr );
}

void
draw_push_pulse( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 rate, f32 depth, f32 phase,
                 u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding, TESS_FX_AA, 0u, rate, depth, phase, 0.0f,
                     0.0f, 0.0f, abgr );
}

/* The rotated box: same surface, four corner positions turned about the box centre.  The default
   1 px AA is folded in here (a caller passing feather 0 wants a crisp edge, not a hard one) --
   the same bake draw_push_pulse does. */
void
draw_push_box_xf( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, f32 rot, u32 abgr )
{
    draw_fx_box_cmd( x, y, w, h, rounding,
                     ( feather > TESS_FX_AA ) ? feather : TESS_FX_AA,
                     0u, 0.0f, 0.0f, 0.0f, rot, 0.0f, 0.0f, abgr );
}

/*==============================================================================================
    draw_push_round_rect_ex -- emit a filled box with four independent corner radii.

    The ambient rounding does NOT apply here and is not consulted: a caller reaching for this is
    naming every corner, and silently folding in a scope-level radius is how a tab ends up rounded
    on the two corners it wanted square.  The radii are clamped to the box at tessellation time
    (tess_fx_box_core), so an over-large one degenerates to a capsule rather than inverting.

    `feather` widens the falloff band exactly as draw_push_shadow's does: 0 gets the standard 1 px
    AA, wider makes the per-corner SOFT SHADOW -- the drop shadow under a tab or an asymmetric
    card, which draw_push_shadow (one radius) could not shape.  The quadrants agree at any feather
    (tess_fx_box_core's centre-line proof), so softness places no per-corner restriction.
==============================================================================================*/

void
draw_push_round_rect_ex( f32 x, f32 y, f32 w, f32 h,
                         f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather,
                         u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind, f32 grad_mid )
{
    /* The ramp midpoint, authored 0..1 (where the 50/50 blend lands along the ramp), mapped to
       the exponent the record carries: t^e crosses 0.5 at mid when e = ln 0.5 / ln mid.  0.5 and
       0 are the linear default and store 0, which is also what keeps two identical linear ramps
       authored either way deduping onto one record. */
    f32 mid_e = 0.0f;
    if ( grad_mid > 0.001f && grad_mid < 0.999f && grad_mid != 0.5f )
        mid_e = -0.69314718f / logf( grad_mid );
    /* Cull against the grown box: the falloff skirt is real geometry (feather/2 past the rect,
       plus the tessellator's pixel of slack) -- the draw_push_shadow rule. */
    f32 pad = ( feather > 0.0f ? feather * 0.5f : 0.0f ) + 1.0f;
    u32 col = draw_apply_alpha( abgr );
    u32 cb  = draw_apply_alpha( col_b );

    /* The transparent drop must see the WHOLE ramp: a gradient fading in from nothing has a
       transparent first endpoint and is still a real shape, the same nuance an outline-only
       text run relies on.  Visibility is therefore the stronger of the two alphas. */
    u32 vis = ( ( cb >> 24 ) > ( col >> 24 ) ) ? cb : col;

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_ROUND_RECT_EX, vis, x, y, w, h, pad );
    if ( !c )
        return;
    c->round_rect.x        = x;
    c->round_rect.y        = y;
    c->round_rect.w        = w;
    c->round_rect.h        = h;
    c->round_rect.rtl      = rtl;
    c->round_rect.rtr      = rtr;
    c->round_rect.rbr      = rbr;
    c->round_rect.rbl      = rbl;
    c->round_rect.feather  = feather;
    c->round_rect.corner_pow = s_draw.corner_pow;
    c->round_rect.abgr     = col;
    c->round_rect.col_b    = cb;
    c->round_rect.grad_ang  = grad_ang;
    c->round_rect.grad_kind = grad_kind;
    c->round_rect.grad_mid  = mid_e;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_arc / draw_push_pie -- emit a circular sector as one semantic command.

    Angles are radians in screen space (0 points +x, positive turns clockwise).  A reversed range
    and a sweep past a full turn are both normalized at tessellation, so a caller animating an
    angle never has to wrap it.

    The cull box is the whole circle rather than the sector's own bounds.  It is a conservative
    test against the scissor and nothing more -- computing the tight rotated extent to reject a few
    more off-screen arcs would cost every on-screen one two transcendentals it does not otherwise
    need at emit time.  The TESSELLATOR does compute the tight box, where it pays for itself in
    fragments rather than in a rejection that usually fails anyway.
==============================================================================================*/

static void
draw_sector_cmd( u8 type, f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                 f32 spin_rate, f32 spin_phase, u32 abgr )
{
    u32 col = draw_apply_alpha( abgr );
    f32 g   = r + thickness * 0.5f;   /* the tessellator's own AA pad rides draw_cmd_open's `pad` */

    gui_cmd_t* c = draw_cmd_open( type, col, cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !c )
        return;
    c->arc.cx         = cx;
    c->arc.cy         = cy;
    c->arc.r          = r;
    c->arc.thickness  = thickness;
    c->arc.a0         = a0;
    c->arc.a1         = a1;
    c->arc.spin_rate  = spin_rate;
    c->arc.spin_phase = spin_phase;
    c->arc.abgr       = col;
    draw_cmd_seal();
}

void
draw_push_arc( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1, u32 abgr )
{
    draw_sector_cmd( GUI_CMD_ARC, cx, cy, r, thickness, a0, a1, 0.0f, 0.0f, abgr );
}

/* The arc under GUI_OP_SPIN: its whole frame rotates at `rate` turns/sec on the shader clock, so
   the command's bytes are identical every frame it runs -- the spinner that re-tessellates
   nothing.  `phase` is the starting angle in turns.  The caller still presents frames
   (gui()->request_redraw(), the pulse contract). */
void
draw_push_arc_spin( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                    f32 rate, f32 phase, u32 abgr )
{
    draw_sector_cmd( GUI_CMD_ARC, cx, cy, r, thickness, a0, a1, rate, phase, abgr );
}

void
draw_push_pie( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 abgr )
{
    draw_sector_cmd( GUI_CMD_PIE, cx, cy, r, 0.0f, a0, a1, 0.0f, 0.0f, abgr );
}

/*==============================================================================================
    draw_push_arc_dashed / draw_push_arc_gradient -- the self-sampled sector variants.

    Both are the plain arc's geometry with one extra word of parameters riding the quad's flat uv
    to the fragment (GUI_FX_ARC_DASH / GUI_FX_ARC_GRAD, gui.h).  Emit's share of the work:

    DASH quantizes the caller's pixel vocabulary (dash/gap arc-length px at radius r, the
    draw_dashed_line terms) into an angular period that divides the sweep a WHOLE number of times.
    Snapping here rather than in the fragment is what keeps a closed dashed ring from showing a
    seam where the pattern meets itself -- and it costs one round() per push, not per pixel.

    GRADIENT folds the ambient alpha into BOTH ends; visibility is the OR of the folded colours,
    the draw_push_rect_gradient rule.
==============================================================================================*/

#define DRAW_TAU  6.28318530717959f

void
draw_push_arc_dashed( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                      f32 dash, f32 gap, u32 abgr )
{
    if ( r <= 0.0f || dash <= 0.0f )
        return;

    /* Angular period from the pixel vocabulary, then snapped so N whole cycles fit the sweep. */
    f32 sweep = a1 - a0;
    if ( sweep < 0.0f ) sweep = -sweep;
    if ( sweep > DRAW_TAU ) sweep = DRAW_TAU;
    f32 period = ( dash + ( gap > 0.0f ? gap : dash ) ) / r;
    f32 n      = floorf( sweep / period + 0.5f );
    if ( n < 1.0f ) n = 1.0f;
    period = sweep / n;

    u32 col = draw_apply_alpha( abgr );
    f32 g   = r + thickness * 0.5f;

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_ARC_DASH, col, cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !c )
        return;
    c->arc_dash.cx        = cx;
    c->arc_dash.cy        = cy;
    c->arc_dash.r         = r;
    c->arc_dash.thickness = thickness;
    c->arc_dash.a0        = a0;
    c->arc_dash.a1        = a1;
    c->arc_dash.period    = period;
    c->arc_dash.duty      = dash / ( dash + ( gap > 0.0f ? gap : dash ) );
    c->arc_dash.abgr      = col;
    draw_cmd_seal();
}

void
draw_push_arc_gradient( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                        u32 col_a, u32 col_b )
{
    if ( r <= 0.0f )
        return;

    /* Normalize a reversed range HERE, not at tessellation: the tessellator's swap is invisible
       for a symmetric arc but this one is not -- swapping the endpoints without swapping the
       colours would silently flip the gradient. */
    if ( a1 < a0 )
    {
        f32 t = a0; a0 = a1; a1 = t;
        u32 u = col_a; col_a = col_b; col_b = u;
    }

    /* Visible if EITHER end is -- the OR'd alpha is the visibility word draw_cmd_open tests. */
    u32 ca = draw_apply_alpha( col_a );
    u32 cb = draw_apply_alpha( col_b );
    f32 g  = r + thickness * 0.5f;

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_ARC_GRAD, ca | cb,
                                  cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !c )
        return;
    c->arc_grad.cx        = cx;
    c->arc_grad.cy        = cy;
    c->arc_grad.r         = r;
    c->arc_grad.thickness = thickness;
    c->arc_grad.a0        = a0;
    c->arc_grad.a1        = a1;
    c->arc_grad.col_a     = ca;
    c->arc_grad.col_b     = cb;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_checker / draw_push_grid -- the framebuffer-tiling pattern quads.

    Each is ONE quad whose fragment tiles the pattern in framebuffer pixels (GUI_FX_CHECKER /
    GUI_FX_GRID, gui.h); the CPU's share -- quantizing the cell pitch and deriving the anchor
    phase against it -- runs at tessellation, where the box has been snapped.  Emit just gates
    and stores the semantic fields.
==============================================================================================*/

void
draw_push_checker( f32 x, f32 y, f32 w, f32 h, f32 cell, u32 col_a, u32 col_b )
{
    if ( cell < 1.0f )
        cell = 1.0f;

    /* Visible if EITHER colour is -- the OR'd alpha, the two-colour rule (draw_push_rect_gradient). */
    u32 ca = draw_apply_alpha( col_a );
    u32 cb = draw_apply_alpha( col_b );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_CHECKER, ca | cb, x, y, w, h, 0.0f );
    if ( !c )
        return;
    c->checker.x     = x;
    c->checker.y     = y;
    c->checker.w     = w;
    c->checker.h     = h;
    c->checker.cell  = cell;
    c->checker.col_a = ca;
    c->checker.col_b = cb;
    draw_cmd_seal();
}

void
draw_push_grid( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 angle, bool stripes,
                f32 cell, f32 thickness, u32 abgr )
{
    /* A lattice denser than its own line width is a fill; keep the parameters meaning what they
       say rather than letting the fragment resolve a moire. */
    if ( thickness < 1.0f ) thickness = 1.0f;
    if ( cell < 2.0f ) cell = 2.0f;
    if ( cell < thickness ) cell = thickness;

    u32 col = draw_apply_alpha( abgr );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_GRID, col, x, y, w, h, 0.0f );
    if ( !c )
        return;
    c->grid.x         = x;
    c->grid.y         = y;
    c->grid.w         = w;
    c->grid.h         = h;
    c->grid.cell      = cell;
    c->grid.thickness = thickness;
    c->grid.ox        = ox;
    c->grid.oy        = oy;
    c->grid.angle     = angle;
    c->grid.stripes   = stripes ? 1u : 0u;
    c->grid.abgr      = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_ngon -- a regular polygon as one GUI_FX_NGON quad, filled or stroked.

    The polyline fan this replaces sampled up to 64 perimeter points; the field is exact at any
    size and the corner can round.  `rounding` shrinks the polygon and inflates the field back
    out, so the stated circumradius is the size drawn.  The border-align ambient applies to the
    stroked form exactly as it does to the rect outline -- same inflation, same reasoning.
==============================================================================================*/

void
draw_push_ngon( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, f32 rounding,
                f32 thickness, u32 abgr )
{
    if ( r <= 0.0f )
        return;
    if ( sides < 3u )  sides = 3u;
    if ( sides > 64u ) sides = 64u;

    if ( thickness > 0.0f )
    {
        f32 ba = s_draw.border_align * thickness;
        r += ba;
        if ( rounding > 0.0f ) rounding += ba;
    }
    if ( rounding > r * 0.5f ) rounding = r * 0.5f;   /* the field needs a real core to inflate from */

    u32 col = draw_apply_alpha( abgr );
    f32 g   = r + 1.0f;

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_NGON, col, cx - g, cy - g, g * 2.0f, g * 2.0f, 1.0f );
    if ( !c )
        return;
    c->ngon.cx        = cx;
    c->ngon.cy        = cy;
    c->ngon.r         = r;
    c->ngon.rounding  = rounding;
    c->ngon.rot       = rot;
    c->ngon.thickness = thickness;
    c->ngon.sides     = sides;
    c->ngon.abgr      = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_box_dashed -- a rounded-box outline cut by a perimeter dash (the marching ants).

    dash/gap are arc-length px, the draw_dashed_line vocabulary; the tessellator snaps the period
    so whole cycles fit the perimeter and the pattern meets itself.  `rate` scrolls it in px/sec
    on the shader clock (0 = static), so the ants' command bytes never change while they march.
==============================================================================================*/

void
draw_push_box_dashed( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 t,
                      f32 dash, f32 gap, f32 rate, f32 phase, u32 abgr )
{
    if ( dash <= 0.0f || t <= 0.0f )
        return;

    /* Border alignment: the same push-time inflation the plain outline runs. */
    f32 ba = s_draw.border_align * t;
    if ( ba > 0.0f )
    {
        x -= ba;  y -= ba;  w += ba * 2.0f;  h += ba * 2.0f;
        if ( rounding > 0.0f ) rounding += ba;
    }

    u32 col = draw_apply_alpha( abgr );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_BOX_DASH, col, x, y, w, h, 1.0f );
    if ( !c )
        return;
    c->box_dash.x        = x;
    c->box_dash.y        = y;
    c->box_dash.w        = w;
    c->box_dash.h        = h;
    c->box_dash.rounding = rounding;
    c->box_dash.t        = t;
    c->box_dash.dash     = dash;
    c->box_dash.gap      = gap;
    c->box_dash.rate     = rate;
    c->box_dash.phase    = phase;
    c->box_dash.abgr     = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_rect_outline -- emit a hollow rectangle semantic command.
==============================================================================================*/

void
draw_push_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    /* Rounded outlines become GUI_OP_BAND surfaces with an AA skirt past the authored rect --
       the same 1 px cull slack the rounded fill takes (see draw_rect_cmd). */
    f32 rounding = draw_clamp_rounding( w, h );

    /* Border alignment, resolved here and nowhere else: an aligned band is the INSIDE band of
       the shape inflated by align * width, with the radius growing by the same amount so the
       corners stay concentric with the authored ones.  A square outline (radius 0) keeps its
       sharp corner -- the mitre join -- because the inflation moves edges, not arcs. */
    f32 ba = s_draw.border_align * t;
    if ( ba > 0.0f )
    {
        x -= ba;  y -= ba;  w += ba * 2.0f;  h += ba * 2.0f;
        if ( rounding > 0.0f )
        {
            rounding += ba;
            f32 lim = ( ( w < h ) ? w : h ) * 0.5f;
            if ( rounding > lim ) rounding = lim;
        }
    }
    f32 pad      = ( rounding > 0.0f ) ? 1.0f : 0.0f;
    u32 col      = draw_apply_alpha( abgr );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_RECT_OUTLINE, col, x, y, w, h, pad );
    if ( !c )
        return;
    c->rect_outline.x        = x;
    c->rect_outline.y        = y;
    c->rect_outline.w        = w;
    c->rect_outline.h        = h;
    c->rect_outline.t        = t;
    c->rect_outline.abgr     = col;
    c->rect_outline.rounding = rounding;
    c->rect_outline.corner_pow = ( rounding > 0.0f ) ? s_draw.corner_pow : 0.0f;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_triangle -- emit a solid triangle semantic command.
==============================================================================================*/

void
draw_push_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    u32 col = draw_apply_alpha( abgr );

    /* Cull against the bounding box of the three vertices. */
    f32 minx = ax < bx ? ( ax < cx ? ax : cx ) : ( bx < cx ? bx : cx );
    f32 maxx = ax > bx ? ( ax > cx ? ax : cx ) : ( bx > cx ? bx : cx );
    f32 miny = ay < by ? ( ay < cy ? ay : cy ) : ( by < cy ? by : cy );
    f32 maxy = ay > by ? ( ay > cy ? ay : cy ) : ( by > cy ? by : cy );

    gui_cmd_t* c = draw_cmd_open( GUI_CMD_TRIANGLE, col, minx, miny,
                                  maxx - minx, maxy - miny, 0.0f );
    if ( !c )
        return;
    c->tri.ax   = ax; c->tri.ay = ay;
    c->tri.bx   = bx; c->tri.by = by;
    c->tri.cx   = cx; c->tri.cy = cy;
    c->tri.abgr = col;
    draw_cmd_seal();
}

/*==============================================================================================
    draw_push_text -- emit a glyph-run semantic command.

    str is copied into the frame text pool, so stack-local buffers (textf, snprintf labels) are
    fine; nothing about the caller's string needs to outlive the call.
    n == 0xFFFFFFFF means "entire NUL-terminated string"; a smaller n limits the glyph count
    (used to skip "##label" suffixes).
==============================================================================================*/

/* Copy one run into the frame text pool -- the single body behind both text pushes.  Returns
   false (loudly, once) when the pool is exhausted; *out_off is the stored offset.  The pool copy
   is what lets callers pass stack-local buffers (textf, snprintf labels): nothing about the
   caller's string needs to outlive the call. */
static bool
draw_text_pool_copy( const char* str, u32 len, u32* out_off )
{
    if ( s_draw.text_pool_used + len + 1 > GUI_MAX_TEXT_POOL )
    {
        /* Drop the label rather than store a dangling pointer -- but never silently.  Text
           vanishing with rects still painting reads as a font bug, not a pool cap, so name the
           real cause. */
        GUI_WARN_ONCE( "frame text pool full (%u bytes) -- further text this frame "
                       "is dropped. Raise GUI_MAX_TEXT_POOL (gui.h).\n", (unsigned)GUI_MAX_TEXT_POOL );
        ORB_ASSERT_MSG_ONCE( false, "gui text pool exhausted -- labels dropped; raise "
                                    "GUI_MAX_TEXT_POOL (gui.h)" );
        return false;
    }
    u32   off = s_draw.text_pool_used;   /* offset stored in the cmd; pointer stays local */
    char* dst = s_draw.text_pool + off;
    memcpy( dst, str, len );
    dst[ len ]            = '\0';
    s_draw.text_pool_used += len + 1;
    *out_off = off;
    return true;
}

void
draw_push_text_clip_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    if ( !str || draw_emit_blocked() )
        return;

    /* Transparent drop (the draw_cmd_open rule): a run whose folded fill alpha is 0 lights no
       pixel, so alpha doubles as a free visibility toggle -- a hidden label costs no command
       slot, no pool copy, no hash.  The one exception is a visible TEXT_EDGE: the edge band paints
       OUTSIDE the glyph boundary, so outline-only text over a transparent fill is a real shape and
       must survive. */
    u32 col = draw_apply_alpha( abgr );
    if ( ( col >> 24 ) == 0u && !draw_text_edge_visible() )
        return;

    /* Vertical cull: a glyph run lights pixels within roughly one line height of y, so if that band
       sits fully above or below the current clip the run is invisible -- a list row scrolled out of
       its box.  Padded a full line each way so ascenders / descenders are never wrongly dropped;
       horizontal overflow is left to the GPU scissor and the per-glyph clip in tess_text_n.  Done
       before the pool copy so a culled run costs no string-pool space either. */
    {
        gui_rect_t cc = clip_current();
        f32          lh = font_line_h();
        if ( rect_empty( cc ) || y + 2.0f * lh <= cc.y || y - lh >= cc.y + cc.h )
            return;
    }

    /* Resolve length at push time (sentinel means NUL-terminated), then pool the bytes. */
    u32 len = ( n == 0xFFFFFFFFu ) ? (u32)strlen( str ) : n;
    u32 off;
    if ( !draw_text_pool_copy( str, len, &off ) )
        return;

    gui_cmd_t* c    = draw_cmd_claim( GUI_CMD_TEXT );
    c->text.x       = x;
    c->text.y       = y;
    c->text.off     = off;
    c->text.len     = len;   /* always an explicit byte count; never 0xFFFFFFFF after this point */
    c->text.clip_x0 = clip_x0;
    c->text.clip_x1 = clip_x1;
    c->text.abgr    = col;
    c->text.edge_w  = s_draw.text_edge_w;
    c->text.edge_col = s_draw.text_edge_col;
    c->text.font    = (u16)s_draw.cur_font;
    draw_cmd_seal();   /* text bytes are L1-hot here */
}

/* Text that inherits the ambient text-clip window: the common path for widget content.  Normally
   the window is the no-clip sentinel and the tessellator skips the per-glyph clip test entirely; a
   seam (table cell at the viewport edge) can set a real window so this run hard-clips at the slot
   edge without any call-site change. */
void
draw_push_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n )
{
    draw_push_text_clip_n( x, y, abgr, str, n, s_draw.text_clip_x0, s_draw.text_clip_x1 );
}

void
draw_push_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text_n( x, y, abgr, str, 0xFFFFFFFFu );
}

/*==============================================================================================
    draw_push_text_xf -- emit a TRANSFORMED glyph run: scaled uniformly and rotated about (x, y).

    Everything the 1:1 push does with the string is done identically here (pool copy, so a stack
    buffer is fine).  What is deliberately NOT done is the vertical band cull: that test assumes a
    run lights pixels within a line height of y, which is exactly the assumption a scale and a
    rotation break -- a run rotated 90 degrees reaches its own WIDTH away from y.  Computing the
    true footprint would mean measuring the string here, so a transformed run simply relies on the
    GPU scissor, the same way every non-text shape does.  Only the empty-clip case still cuts, and
    that one is free.
==============================================================================================*/

void
draw_push_text_xf( f32 x, f32 y, u32 abgr, const char* str, f32 scale, f32 rot )
{
    if ( !str || scale <= 0.0f || draw_emit_blocked() )
        return;

    /* Transparent drop, with the same TEXT_EDGE exception as draw_push_text_clip_n. */
    u32 col = draw_apply_alpha( abgr );
    if ( ( col >> 24 ) == 0u && !draw_text_edge_visible() )
        return;
    if ( rect_empty( clip_current() ) )
        return;

    u32 len = (u32)strlen( str );
    u32 off;
    if ( !draw_text_pool_copy( str, len, &off ) )
        return;

    gui_cmd_t* c     = draw_cmd_claim( GUI_CMD_TEXT_XF );
    c->text_xf.x     = x;
    c->text_xf.y     = y;
    c->text_xf.off   = off;
    c->text_xf.len   = len;
    c->text_xf.scale = scale;
    c->text_xf.rot   = rot;
    c->text_xf.abgr  = col;
    c->text_xf.edge_w   = s_draw.text_edge_w;
    c->text_xf.edge_col = s_draw.text_edge_col;
    c->text_xf.font  = (u16)s_draw.cur_font;
    draw_cmd_seal();
}

// clang-format on
/*============================================================================================*/
