#ifndef GUI_RENDER_H
#define GUI_RENDER_H
/*==============================================================================================

    runtime_service/gui/render/gui_render.h -- THE RENDER SERVER's surface (the unit seam).

    A 2d batch renderer with a narrow push-primitive foundation any 2d utility can emit to.
    Knows nothing of ids-as-identity, interact state, style, or
    layout: the units above produce a semantic draw list through the draw_push_* primitives
    below; this server tessellates, caches, and uploads it.  This header is the server's
    entire surface -- self-standing on the public gui types + the engine APIs
    (never a gui unit header: the two servers must not see each other).

    The reverse direction is almost nothing: the glyph/sprite source contract (implemented
    by the draw unit over tables beside the atlas), the volatile replay-scope pair, and the
    debug overlay's ambient build viewport (dbg_build_viewport) -- each one documented at
    its declaration.

    The module API pointers (rhi() / app()) are NOT redefined here: g_rhi_api_ptr / g_app_api_ptr
    have external linkage, defined and fetched once in gui.c (MOD_USE_RHI / MOD_USE_APP); the
    render unit reads them through the same inline accessors from rhi_api.h / app_api.h.

    Sections below are grouped by pipeline stage, matching the include order in gui_render.c and
    named for the function prefix each stage exports.  Tessellation primitives (gui_build_tess.c)
    have no public surface -- driven entirely from within BUILD -- so there is no section for them.

    0. Backend lifecycle (backend_init/exit)
    1. Glyph / sprite source contract + the shared atlas
    2. EMIT -- CPU draw list
    3. BUILD -- retained cache
    4. RENDER -- GPU flush
    5. DEBUG OVERLAY / DASHBOARD / STEPPER captures

==============================================================================================*/

#include "engine/app/app_api.h"                 // APP_WIN_MAX -- the per-surface fan-out bound
#include "runtime_service/gui/gui_host.h"       // public gui types: gui_rect_t, gui_id_t, flags, enums
#include "runtime_service/gui/font/gui_font.h"  // font resources
#include "runtime_service/rhi/rhi_api.h"        //  

/* The debug unit's header leads every unit (severable instrumentation over public types): it
   computes the Debug-build switches (GUI_DEBUG_OVERLAY, GUI_CMD_STEPPER) the capture sections
   below key off, so it is the one sanctioned above-layer include. */
#include "runtime_service/gui/debug/gui_debug.h"

/* Render-surface ceiling: one gui viewport rides one OS window + one rhi context, so the
   per-surface capture tables here and the viewport pool default (frame/gui_context.c) are
   sized by the platform pair -- derived, not repeated. */

#define GUI_MAX_VIEWPORTS APP_WIN_MAX       
ORB_STATIC_ASSERT( APP_WIN_MAX == RHI_CTX_MAX,
        "a gui viewport pairs an OS window with an rhi context; the maxes must agree" );

// clang-format off
/*==============================================================================================
    Backend lifecycle (gui_render.c) -- the seam the frame orchestrator calls to stand up / tear
    down the whole render backend.  Internally wraps render_init/shutdown (pipeline/
    gui_render_submit.c), which are not exposed past this header.
==============================================================================================*/

bool backend_init( void );
void backend_exit( void );

/*==============================================================================================
    Glyph / sprite source contract -- the data the server resolves at tess/emit time

    The render server renders from the shared atlas that is PUSHED to it; it does not know
    what a font or an icon IS.  What it does need, mid-pipeline, is a resolver: the
    tessellator turns a text command into quads via font_glyph and re-activates the segment's
    font by id (font_use / font_active_id); the emit layer resolves an icon id to its cached
    UVs (icon_get).  These are implemented by the DRAW unit's resources (draw/gui_glyph.c,
    draw/gui_icon.c) over tables that live beside the atlas -- the server consumes the
    installed source and never manages it.
==============================================================================================*/

/* Glyph lookup: UVs, pen offsets, glyph box, and advance for one character (defined in the DRAW
   unit's font resources, draw/gui_glyph.c).  The active-font selection + metrics the tessellator
   also reads (font_use / font_active_id / font_valid / font_line_h) are the font/ resource, pulled in
   above -- measuring text is sizes-and-math, not a render resource. */
void font_glyph    ( u32 cp, f32* u0, f32* v0, f32* u1, f32* v1,
                             f32* ox, f32* oy, f32* gw, f32* gh, f32* advance );

/* The tex_idx a glyph draw of the ACTIVE font must carry: its backing atlas's bindless slot with
   the sampling model already in the mode field (gui.h).  The tessellator asks rather than reaching
   for res_atlas_idx(), because which atlas a font lives in is a property of the FONT -- a
   distance-field font packs elsewhere and samples differently.  0 when the atlas is not up yet. */
u32  font_tex      ( void );

/* The glyph UV table (draw/gui_glyph_table.c).  A glyph quad carries a table ID rather than a
   baked atlas rect, so the vertex stage resolves the rect and cached text geometry survives an
   atlas repack -- IDs address (registry slot, glyph) and never move, where a baked UV goes stale
   the moment a tenant's page origin does.

   glyph_table_sync must run before tessellation each frame: it rebuilds when a font's tenant
   changed or an atlas generation bumped, so the IDs a run emits and the rects behind them come
   from one build.  The generation is the upload's staleness key. */
void                  glyph_table_sync      ( void );
void                  glyph_table_mark_dirty( void );
u32                   font_glyph_id         ( u32 cp );   /* ID for cp in the ACTIVE font slot */

/* The table path's per-character dispatch: ID plus placement from ONE record lookup.  What the
   text tessellator calls instead of font_glyph -- the atlas rect it would compute is the table's
   job now, and resolving the ID separately would search ext[] twice per character. */
void font_glyph_placed( u32 cp, u32* id, f32* ox, f32* oy, f32* gw, f32* gh, f32* advance );
const gui_glyph_uv_t* glyph_table_data      ( void );
u32                   glyph_table_count     ( void );
u32                   glyph_table_generation( void );

/* Icon lookup: cached UVs (+ optional pixel size) for a registered icon id. */
bool icon_get      ( gui_icon_id_t id,
                     f32* u0, f32* v0, f32* u1, f32* v1, u32* w, u32* h );

/* The tex_idx an icon quad must carry, with the sampling model already in its mode field -- the
   icon twin of font_tex, and for the same reason: which atlas an icon lives in is a property of
   the ICON.  A coverage icon and a distance-field one differ in what a texel means, so they cannot
   share a texture (the sampler is per draw), but they still share a DRAW CALL, because this number
   travels in the vertex.  0 when the backing atlas is not up yet. */
u32  icon_tex      ( gui_icon_id_t id );

/* Sprite lookup: UVs, pixel size, and the nine-slice insets for a registered sprite (defined in
   draw/gui_sprite.c).  Resolved at TESSELLATION time rather than emit time -- unlike an icon,
   whose UVs the emit layer bakes into its command -- because the slice expansion needs the source
   pixel size and insets anyway, and resolving late means a sprite-atlas repack corrects itself
   through the ordinary re-tessellate path (res_sprite_generation is folded into the window hash).
   A NULL out-param is skipped; `slice` is {0,0,0,0} for a sprite with no insets. */
bool sprite_get    ( gui_sprite_id_t id,
                     f32* u0, f32* v0, f32* u1, f32* v1,
                     u32* w, u32* h, gui_pad_t* slice );

/*==============================================================================================
    Shared resource atlas (resource/gui_res_atlas.c)

    THE one R8 texture core UI draws from: fonts, icons and the solid/dash assists all pack into it,
    so they share a bindless slot and batch into one draw per clip/viewport scope.  Past this seam
    only the deferred-upload flush is needed; everything else (packing, sampling accessors) is
    backend-internal and reached through the source-contract accessors above.
==============================================================================================*/

bool            res_atlas_flush_upload  ( void );   // re-upload if dirty; true when pixels were sent

/* Occupancy diagnostics (mem stats print): percent of the packable region covered, live tenant
   count, current dimensions (the atlases grow under pressure; 0-dims = never created). */
void            res_atlas_occupancy     ( f32* pct, u32* tenants, u32* w, u32* h );
void            res_sprite_occupancy    ( f32* pct, u32* tenants, u32* w, u32* h );
void            res_sdf_occupancy       ( f32* pct, u32* tenants, u32* w, u32* h );

/*==============================================================================================
    EMIT: CPU draw list (pipeline/gui_emit_draw.c)
==============================================================================================*/

void draw_reset( i32 display_w, i32 display_h );    // clear the list at the top of frame_begin

void draw_set_alpha             ( f32 a );          // global opacity multiplier folded into every pushed shape
f32  draw_get_alpha             ( void );           // ...read back, so a nested fade can multiply and restore
void draw_set_rounding          ( f32 r );          // corner radius folded into every pushed filled/outline rect
f32  draw_rounding              ( void );           // current ambient radius (save/restore around a sub-element)
void draw_set_corner_smooth     ( f32 t );          // 0..1 corner profile riding with the radius; 0 = circular arc
f32  draw_corner_smooth         ( void );           // ...read it back, same save/restore rule as the radius
void draw_set_anim_curve        ( u32 curve, f32 param );   // gui_curve_t shaping every animating shape pushed after
void draw_get_anim_curve        ( u32* curve, f32* param ); // ...read it back, for save/restore
void draw_set_anim_phase        ( f32 cycles );     // cycle offset added to every animating shape pushed after
f32  draw_anim_phase            ( void );           // ...read it back, same save/restore rule
void draw_set_border_align      ( f32 a );          // stroked-box band alignment: 0 inside, 0.5 centred, 1 outside
f32  draw_border_align          ( void );           // ...read it back, same save/restore rule as the radius
void draw_set_text_edge         ( f32 width, u32 abgr ); // second colour outside the glyph edge (SDF fonts)
void draw_text_edge             ( f32* width, u32* abgr );  // read it back (save/restore around a run)
void draw_set_text_clip_x       ( f32 x0, f32 x1 ); // glyph-clip window folded into every pushed text run
void draw_clear_text_clip       ( void );           // restore the no-clip sentinel (unbounded text)
void draw_set_sort_key          ( u32 z );          // paint order stamped on new commands (window z)
void draw_set_viewport          ( i32 vp );    // viewport stamped on new commands (surface routing)
void draw_set_band              ( u32 band );       // arena band: 0 = main UI, 1 = debug (GUI_WIN_DEBUG_BAND)
u32  draw_band                  ( void );           // current band (sampled for popup band inheritance)
void draw_set_window            ( gui_id_t win );   // stable window id stamped on new commands (cache key)
void draw_set_font              ( u32 font );       // active font id, stamped onto each TEXT command (push/pop/use_font)
u32  draw_get_font              ( void );           // current stamp font (save/restore around a scoped swap)

/* The paint cursor as one record (state in gui_emit_draw.c; here -- the definer's
   side of the seam): the command segment tag (owning window, sort key, viewport, arena band --
   the ambient font stays global by design) plus the ambient glyph-clip window (a table cell
   sets it for its span).  draw_scope / draw_scope_set read and write it wholesale for the
   overlay seam. */

typedef struct
{
    gui_id_t    window;         // s_draw.cur_win (retained-cache key)
    u32         sort_key;       // s_draw.cur_z (paint order)
    i32         viewport;       // s_draw.cur_vp (target surface routing)
    u32         band;           // s_draw.cur_band (arena band: debug UI isolation)
    f32         text_clip_x0;   // ambient glyph-clip window
    f32         text_clip_x1;   // ambient glyph-clip window

} gui_draw_scope_t;

gui_draw_scope_t draw_scope     ( void );              // paint cursor + glyph clip as one record
void             draw_scope_set ( gui_draw_scope_t s );// restore it wholesale (the overlay seam)

void draw_push_clip_rect        ( f32 x, f32 y, f32 w, f32 h ); // push clip, intersected with the parent
void draw_push_clip_rect_rounded( f32 x, f32 y, f32 w, f32 h, f32 radius ); // same, corners rounded in the
                                                                //   FRAGMENT (clip_coverage) -- radius
                                                                //   clamped to the half-extent
void draw_pop_clip_rect         ( void );                       // pop the top clip
void draw_push_clip_root        ( void );                       // push the full-display clip (popup escape)
void draw_set_root_clip         ( f32 w, f32 h );               // set clip_stack[0] to a surface size

void draw_push_rect_filled      ( f32 x, f32 y, f32 w, f32 h,
                                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr );

/* The same quad, declared to be a PICTURE rather than a glyph -- so the ambient rounding radius
   applies to it.  draw_push_rect_filled rounds solid fills only, because icons come through it
   too and an icon must not have its corners cut.  Only a caller showing an arbitrary texture as
   an image should use this (gui_draw_texture_in). */
void draw_push_image            ( f32 x, f32 y, f32 w, f32 h,
                                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr );

/* Push `count` solid rects as ONE semantic command (per-frame rect pool, one quad each at
   flush) -- the dense-shape escape valve for callers that would otherwise exhaust GUI_MAX_CMDS
   (timeline bars, graph columns).  Square, current clip, per-entry color. */
void draw_push_rect_list        ( const gui_rect_col_t* rects, u32 count );

/* Push one registered icon quad into the draw list; no-op for an invalid id.  UVs come from
   icon_get (the sprite source contract above); reuses draw_push_rect_filled -- an icon is just a
   textured quad, sampled from the same shared atlas as text. */
void draw_push_icon             ( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, u32 abgr );

/* The same textured quad under a rotation about its CENTRE (radians, screen space) -- the text_xf
   treatment for one quad.  UVs interpolate across the turned quad exactly as they would upright;
   what makes it look right at any angle is the icon's sampling model (an SDF icon resolves its
   edge from the screen-space derivative, a coverage icon shows its texels -- the text_xf rule). */
void draw_push_image_xf         ( f32 x, f32 y, f32 w, f32 h,
                                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, f32 rot, u32 abgr );
void draw_push_icon_xf          ( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, f32 rot, u32 abgr );

/* Push one sprite over a rect.  `nine` asks for the slice expansion (up to 9 quads from this ONE
   command, all in the same batch); a sprite with no authored insets draws as a single stretched
   quad regardless.  `scale` multiplies the slice insets and the tile pitch so one piece of art
   serves several UI scales (0 or 1 = authored size), `flags` is gui_brush_flags_t (tile / flips),
   and `abgr` tints (0 = untinted).  Nothing is resolved here: the id travels to the tessellator. */
void draw_push_sprite           ( f32 x, f32 y, f32 w, f32 h, gui_sprite_id_t id,
                                  u32 abgr, f32 scale, u16 flags, bool nine );

void draw_push_rect_gradient    ( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal );

/* The antialiasing band a shape gets when nothing asked for a softer one -- one pixel, centred on
   the boundary.  Named because it is the difference between "rounded" and "rounded and crisp":
   every rounded fill and frame tessellates with it, and the emit side bakes it into the commands
   (a pulse's feather) so a retained command re-tessellates with the width it was authored at. */
#define TESS_FX_AA  1.0f

/* Push a soft rounded box -- the SDF surface behind draw_shadow.  `feather` is the TOTAL width of
   the falloff band and it straddles the boundary, so the geometry reaches feather/2 past the box
   on every side while the shape itself stays exactly where it was authored. */
void draw_push_shadow           ( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, u32 abgr );

/* The same surface with its interior cut away: same outward falloff, nothing painted inside the
   boundary.  What a DROP shadow is -- a filled one's core is only visible through the thing casting
   it, so a translucent panel ends up dimming itself.  Emits a band of quads around the frame rather
   than the whole box.  Use draw_push_shadow for a glow meant to be seen THROUGH its subject.
   x,y,w,h is the CASTER and (ox, oy) is how far the shadow falls from it: the falloff is measured
   from the shadow's outline while the cut is taken against the caster's, which is what makes the
   cast DIRECTIONAL.  (0, 0) is the even cast on all four sides. */
void draw_push_skirt            ( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather,
                                  f32 ox, f32 oy, u32 abgr );

/* The inner shadow -- the falloff turned INWARD, painting from the boundary `depth` px in and
   nothing outside it.  The pressed well / recessed field a drop shadow cannot express. */
void draw_push_inset            ( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 depth, u32 abgr );
void draw_push_pulse            ( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 rate, f32 depth,
                                  f32 phase, u32 abgr );

/* The same SDF box surface under a rotation about its CENTRE (radians, screen space).  The fx
   coordinate is box-local and affine, so only the four corner positions turn -- same quadrant
   quads, same field, no snap (a rotated box has no axis-aligned edge to keep crisp). */
void draw_push_box_xf           ( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 feather, f32 rot,
                                  u32 abgr );

/* Push a filled box with four independent corner radii (tab / notch / asymmetric card).  Ignores
   the ambient rounding -- the caller names every corner.  Filled only; the stroked form stays a
   perimeter polyline.  `feather` widens the falloff band exactly as draw_push_shadow's does
   (0 = the standard 1 px AA) -- the per-corner soft shadow.
   col_b != abgr makes it a ramp, shaped by `grad_kind` (gui_grad_t) and oriented by `grad_ang`. */
void draw_push_round_rect_ex    ( f32 x, f32 y, f32 w, f32 h,
                                  f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather,
                                  u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind,
                                  f32 grad_mid );

/* Push a circular sector -- a stroked arc with round caps, or a filled wedge with sharp radial
   edges.  Angles are radians in screen space (0 points +x, positive turns clockwise); a reversed
   range or a sweep past a full turn is normalized at tessellation.  One quad either way.
   draw_push_arc's thickness is unbounded: the record's tube is a plain float, so a fat arc no
   longer falls back to a stroked polyline the way it did under the packed word's 15.875 px cap. */
void draw_push_arc              ( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1, u32 abgr );
void draw_push_pie              ( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 abgr );

/* The arc under GUI_OP_SPIN: the whole frame rotates at `rate` turns/sec on pc.time, so a
   spinner's bytes never change while it runs -- the pulse contract for rotation.  `phase` is the
   start in turns.  The caller presents frames with gui()->request_redraw(). */
void draw_push_arc_spin         ( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                                  f32 rate, f32 phase, u32 abgr );

/* The arc cut by an angular dash pattern.  `dash` / `gap` are arc-length PIXELS at radius r (the
   draw_dashed_line vocabulary); the push converts to an angular period and quantizes it so a WHOLE
   number of cycles fits the sweep -- a closed dashed ring meets itself without a seam.  Animate the
   pattern by rotating a0/a1 together: the dashes ride the sector's local frame (marching ants). */
void draw_push_arc_dashed       ( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                                  f32 dash, f32 gap, u32 abgr );

/* The arc whose colour sweeps col_a (at a0) -> col_b (at a1) by ANGLE -- the gradient a 4-corner
   vertex colour cannot express.  col_b belongs to the shape, so it rides the STYLE record
   (gui_prim_t.col_b), quantized through the uv pair's unorm16 on the way (GUI_FX_ARC_GRAD). */
void draw_push_arc_gradient     ( f32 cx, f32 cy, f32 r, f32 thickness, f32 a0, f32 a1,
                                  u32 col_a, u32 col_b );

/* The framebuffer-tiling pattern quads -- ONE quad each at any area and any cell (gui.h).
   CHECKER alternates col_a / col_b in cell-sized squares anchored at the box origin.  GRID draws
   a `thickness` px line every `cell` px OVER NOTHING (layer it on your own fill); the lattice
   anchors to (ox, oy) in screen px, so a panning canvas passes its content origin. */
void draw_push_checker          ( f32 x, f32 y, f32 w, f32 h, f32 cell, u32 col_a, u32 col_b );
void draw_push_grid             ( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 angle,
                                  bool stripes,
                                  f32 cell, f32 thickness, u32 abgr );

void draw_push_rect_outline     ( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr );
void draw_push_frame            ( f32 x, f32 y, f32 w, f32 h, f32 t, u32 col_bg, u32 col_border );
void draw_push_triangle         ( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr );

/* A regular polygon as one GUI_FX_NGON quad: `sides` flat edges inscribed in circumradius r,
   rotated by `rot` (0 = a vertex up), corners rounded by `rounding` px.  thickness > 0 strokes
   it (GUI_OP_BAND, border-align aware); 0 fills. */
void draw_push_ngon             ( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, f32 rounding,
                                  f32 thickness, u32 abgr );

/* A rounded-box outline cut by a perimeter dash -- the dashed border / marching ants.  dash/gap
   in arc-length px; `rate` scrolls the pattern in px/sec on pc.time, `phase` is a static px
   offset.  The period is snapped at tessellation so the pattern meets itself. */
void draw_push_box_dashed       ( f32 x, f32 y, f32 w, f32 h, f32 rounding, f32 t,
                                  f32 dash, f32 gap, f32 rate, f32 phase, u32 abgr );

/* A filled disc IS a rounded rect whose radius reached the half-extent -- this pushes
   GUI_CMD_RECT_FILLED with rounding = r, not a command of its own. */
void draw_push_circle_filled    ( f32 cx, f32 cy, f32 r, u32 abgr );
void draw_push_text             ( f32 x, f32 y, u32 abgr, const char* str );
void draw_push_text_n           ( f32 x, f32 y, u32 abgr, const char* str, u32 n );
void draw_push_text_clip_n      ( f32 x, f32 y, u32 abgr, const char* str, u32 n,
                                  f32 clip_x0, f32 clip_x1 );

/* A run scaled about, and rotated about, (x, y).  Separate from the three above because a
   transformed run is a different SHAPE, not a parameterized one -- see the text_xf command. */
void draw_push_text_xf          ( f32 x, f32 y, u32 abgr, const char* str, f32 scale, f32 rot );

/* True when a box cannot touch the active clip -- the exact scissor test every draw_push_* runs
   before spending a command slot.  Exposed so a widget can skip its whole PAINT PREP (value
   snprintf, measure walks, fit logic) for a scrolled-out rect in one test, instead of paying the
   prep and having each push culled individually.  Layout, state, and interaction must still run:
   this is a paint gate only.  False when no clip is active (an unclipped surface always paints). */
bool draw_cull_box              ( f32 x, f32 y, f32 w, f32 h );

/*==============================================================================================
    TEXT-SELECTION run capture (render/gui_select_capture.c)

    The backend half of window text selection (GUI_WIN_TEXT_SELECT): at the build seam --
    segments closed, every emit pool complete -- the GUI_CMD_TEXT commands of each window
    marked this frame are copied into a persistent run buffer that survives draw_reset, so
    the chrome unit's selection controller (chrome/window/gui_select.c) can hit-test, highlight and
    copy against them one frame behind the emit that produced them (the standard
    self-measurement lag).  ONE shared buffer serves every flagged window; runs are tagged
    with their owning window id.  Always compiled -- a product feature, not a debug layer.
==============================================================================================*/

#define GUI_SELECT_MAX_RUNS   512      /* text runs held for selection across flagged windows */
#define GUI_SELECT_TEXT_POOL  32768    /* bytes of captured run text (runs past the cap drop) */

typedef struct
{
    gui_id_t   win;      /* owning window (segment tag) */
    i32        vp;       /* viewport the run renders on */
    u32        font;     /* the run's own font id (measure with THIS font) */
    f32        x, y;     /* glyph-run origin (top-left of the glyph box) */
    u32        off, len; /* byte range into the capture text pool (select_run_text) */
    gui_rect_t clip;     /* scissor rect the run rendered under */

} gui_select_run_t;

void select_capture_mark   ( gui_id_t win );  /* flag `win` for capture at this frame's build */
void select_capture_build  ( void );          /* hook at cache_build_frame (segments closed)  */
u32  select_capture_serial ( void );          /* bumped per capture; revalidate anchors on change */
u32  select_run_count      ( void );
const gui_select_run_t* select_run     ( u32 i );                        /* NULL past count  */
const char*             select_run_text( const gui_select_run_t* run );  /* NUL-terminated   */

/*==============================================================================================
    BUILD: retained frame-geometry cache (pipeline/gui_build_cache.c)
==============================================================================================*/

/* Retained-cache capacities.  Here (not in the .c files) because the dashboard snapshot types
   below size their arrays with them; both unity units see one definition. */
#define RENDER_MAX_WIN    32    // distinct windows tracked per frame (32)
#define GUI_MAX_VOLATILE  16    // registered volatile sub-slot rows
#define SLOT_QUAD_PAD     16u   // per-slot quad headroom: absorbs minor growth in-place
#define SLOT_PRIM_PAD     8u    // per-slot record headroom; records are per state change, not
                                //   per primitive, so a window holds few and grows by few

/* One entry of a window slot's LOCAL clip table: the rects this window's primitive records name
   through their `clip` member (gui.h, gui_prim_t).  Records hold ABSOLUTE frame-region entry
   indices -- the window's fixed slab (its id-keyed cache slot * GUI_WIN_CLIP_MAX) plus a local
   first-seen index -- so the flush uploads each slab at its fixed offset, and only when its
   content changed (s_clip_slab_pending).  The fragment resolves them against the frame clip
   buffer (gui_fx.hlsli, clip_coverage) with clip_base flush-constant at the region origin.
   Lives beside the slot's cached geometry so a cache-hit frame replays baked indices against the
   exact rects they meant, however the per-frame global clip table shuffled its indices. */
typedef struct
{
    gui_rect_t rect;      // the clip rect, unsnapped pixels (the flush snaps edges on upload)
    f32        radius;    // corner radius; 0 = the hard scissor-parity cut

} gui_clip_entry_t;

/* Distinct clips per window slot.  This used to trade against RENDER_MAX_WIN, because the product
   of the two had to fit a 9-bit band in the vertex's tex word -- which is why the stress bench ran
   on 4 clips per window while the shipping build had 16.  The record's `clip` is a full 32-bit
   member, so there is no band to fit and no trade to make: both builds get 16, and the only cost
   of raising it further is the clip region's own size. */
#define GUI_WIN_CLIP_MAX  16

/* Drop the once-per-frame tessellation cache so the next flush rebuilds the shared geometry.
   The frame's semantic list is tessellated + z-sorted exactly once (lazily, on the first
   surface flush); every other live surface that frame reuses the result.  Called by
   gui_frame_begin right after draw_reset, before the build emits any new commands. */

void                build_frame_reset       ( void );

/* Per-frame render stats: gui_render_stats returns the last published frame's totals;
   build_stats_publish promotes the in-progress accumulator to the published value and
   resets it -- called once per frame by gui_frame_begin (frame/gui_frame_loop.c), before draw_reset. */

gui_render_stats_t  gui_render_stats        ( void );
void                build_stats_publish     ( void );

/* Retained-skip optimization: when on (default), an unchanged frame (all per-window hashes match
   the previous frame) skips tessellation and reuses s_tess.  Toggle for benchmarking or debugging. */

void                build_set_retained_skip ( bool on );
bool                build_retained_skip     ( void );

/* True when the PREVIOUS frame's render produced any change (a window appeared, vanished, or
   changed content).  Read during frame_begin (before this frame's cache_build_frame
   runs) so s_cache.any_changed still holds last frame's result.  Used with io_dirty and wants_redraw
   to decide whether to skip the widget emit phase entirely (Level 3 retained skip). */

bool                build_any_changed       ( void );

/* Debug: print the cached-geometry slot table (window, z/vp, quad/command bounds) to stdout.
   On-demand companion to the per-frame disjoint-layout assert that runs in debug builds. */

void                build_dump_geometry     ( void );

/* Debug: the style-record census (render/gui_prim_census.c).  `tag` labels the dump in the log and
   is what makes two runs comparable; NULL dumps nothing, so passing (NULL, true) is a bare clear.
   Always callable -- a build without GUI_PRIM_CENSUS says so and does nothing. */

void                build_style_census      ( const char* tag, bool clear );

/* Re-derive the style palette when the landed style, DPI or atlas slot has moved, and publish it
   (render/pipeline/gui_render_bake.c).  Cheap and self-gating: a frame whose inputs are unchanged
   costs one fold over the style vars.  Must run with the tessellation arena idle -- its rows emit
   through the real tessellators and rewind the counters afterwards. */

bool                pal_bake                ( void );

/* Which palette entry holds this record, or GUI_PAL_NONE.  Content-addressed and confirmed by a
   full compare, so a hit is a byte-for-byte equal record and can never be the wrong shape. */

u32                 pal_find                ( const gui_prim_t* rec );

/* A/B switch for the palette lookup.  Off, every style takes a per-slot arena record exactly as it
   did before the palette existed -- more style bloat, identical pixels.  Flipping it re-places all
   geometry (cached quads hold the answers the old setting gave), so it is a debug lever, not a
   per-frame one.  Debug selector menu: "Style palette". */

bool                pal_enabled             ( void );
void                pal_set_enabled         ( bool on );

/* Note a landed style as one the palette must cover -- GUI_VAR_COUNT floats in gui_style_var_t
   order, already through the em scale.  The one-way seam gui_render_set_time crosses: this unit
   does not read the style grid, it is told what the grid says.

   Called once per building frame for the primary surface, and again from every mixed-DPI landing
   (gui_dpi_land): two monitors at different scales put two live style scales in one frame, and each
   needs its own rows because a scale reaches the record as scaled lane values.  Repeats are free --
   a scale already noted is recognised by content.

   pal_style_reset drops the set; run it only at the top of a frame that will EMIT, since a clean
   frame lands nothing and an emptied set would re-bake and discard the very geometry the idle skip
   exists to keep. */

void                pal_style_set           ( const f32* vars, u32 count );
void                pal_style_reset         ( void );

/* Log the table the last bake produced, in the census's own record spelling -- so a baked entry and
   the census row it is meant to cover read as the same line.  Printed with every census dump. */

void                pal_dump                ( void );

/* Fit a corner radius to a rect, the way every rounded emit does (pipeline/gui_emit_draw.c). */

f32                 draw_clamp_round_of     ( f32 r, f32 w, f32 h );

/*==============================================================================================
    Volatile widgets -- an inline-emit callback replayed in place on frames the UI build is
    skipped, so a purely cosmetic animation never forces the whole UI to re-run every frame.

    The feature's actual logic is entirely in two files, one per unit -- read those for the full
    picture; this header is only the boundary between them:

        chrome/widgets/gui_volatile.c        -- CHROME side: gui()->volatile_cb/_begin/_end (gui_api.h),
                                                the replay scope (layout + id), replay_scope_enter/_exit.
        render/pipeline/gui_build_volatile.c -- RENDER side: the registry, capture at real emit, and
                                                volatile_update (run internally by frame_end).

    Forward direction (core -> backend, the normal call direction for this header): gui_volatile_cb
    (chrome/widgets/gui_volatile.c) wraps one real-emit invocation of a callback with these three calls --
    volatile_cb_open records where its commands start, volatile_stamp (called from inside
    the callback body, by gui_volatile_begin) records the window/z/vp/font/clip context and the
    layout cursor position, and volatile_cb_close records where they end, tags the range, and
    CONFINES it to the cell the block just measured (see there -- the clip the block should always
    have had, and the reason its geometry can batch apart from the rest of the window).
    tess_dispatch (gui_build_tess.c) then reserves the block a padded region of its window's slot
    (quads, style records, and its own GPU commands, each with headroom past the live geometry).
    volatile_update is called internally by gui_frame_end on frames where
    frame_dirty() is false: it re-invokes each row's callback standalone, re-tessellates the
    result, and patches it into the reserved region -- any output that FITS the reservation is
    accepted (text may grow/shrink etc); only outgrowing it falls back to one real frame, which
    recaptures at the larger size.

    The block's LAYOUT footprint travels the same seam as a second, independent measurement:
    gui_volatile_cb reports the extent each real emit claimed (volatile_footprint), and each
    idle replay reports its own (replay_scope_measure).  Geometry that outgrows its reservation
    and layout that outgrows its cell are the two ways a block can exceed what the cache holds for
    it, and both now cost exactly one real frame instead of drawing wrong -- see gui.h's
    fixed-footprint contract.

    Reverse direction (backend -> frontend): volatile_update needs a valid layout/id scope for
    the callback to emit into, which only the frontend owns (lf(), the id stack).  replay_scope_enter
    / _exit / _measure are the functions that cross back -- the same kind of unit-seam exception as
    dbg_build_viewport above, just three of them instead of one.
==============================================================================================*/

void     volatile_cb_open   ( gui_id_t id );                    // (re)open row `id`; cmd_lo = current cmd_count
void     volatile_stamp     ( f32 x, f32 y, f32 w,              // fill win/z/vp/font/clip + cursor stamp for the open
                              const gui_rect_t* view, gui_pad_t pad );   //   row, plus the region view/pad the replay frame installs
void     volatile_footprint ( f32 w, f32 h );                   // layout extent this real emit claimed, for the reflow check
void     volatile_cb_close  ( gui_volatile_fn fn, const gui_rect_t* cell );   // cmd_hi + fn; tags + confines the range
void     volatile_update    ( void );
u32      volatile_row_count ( void );                           // registered registry rows (perf overlay, vs GUI_MAX_VOLATILE)
bool     gui_volatile_live  ( void );                           // any row patchable RIGHT NOW -- gui_boot_pace must keep
                                                                //   presenting at cadence instead of block-waiting on input

/* Implemented in chrome/widgets/gui_volatile.c; called only from volatile_update.  view/pad are
   the stamped region context the replay layout frame installs (see volatile_stamp). */
void     replay_scope_enter  ( gui_id_t id, f32 x, f32 y, f32 w,
                               const gui_rect_t* view, gui_pad_t pad );
void     replay_scope_measure( f32* out_w, f32* out_h );    // extent the replay claimed, in volatile_footprint terms
void     replay_scope_exit   ( bool force_redraw );

/*==============================================================================================
    RENDER: GPU resources + flush (pipeline/gui_render_submit.c)

    render_init/shutdown are NOT declared here -- they are TU-local statics, an implementation
    detail of backend_init/exit (above) called directly within the gui_render.c unity TU.
==============================================================================================*/

void                gui_render_flush        ( rhi_texture_t target, i32 vp_index, rhi_cmd_t cmd,
                                              i32 win_w, i32 win_h );

/* Fill the backend-owned buckets of the memory breakdown: GPU device memory (atlas textures,
   the storage-buffer tables, debug-overlay buffers) and every fixed CPU static the backend TU
   defines (see render/gui_render_mem.c).  The CPU-heap context bytes and the totals are filled
   by the frontend (gui_mem_stats), which owns the context pool. */
gui_mem_stats_t     backend_memory          ( u32 live_viewports );

/* Log every capped backend pool's LIFETIME PEAK against its cap -- the diagnostic to read before
   moving a GUI_MAX_* number, and the one that says which cap a reported overflow actually hit.
   Emitted as part of gui_print_mem_stats, which cannot reach the pools itself. */
void                backend_pool_report     ( void );

/* The accounting seam: the font/icon resources live one unit up (draw), so this server
   fills its font bucket by asking the draw unit for its fixed footprint.  Home declaration
   in draw/gui_draw.h; redeclared here because the server cannot see a library header. */
u32                 draw_unit_mem_bytes     ( void );

/* Debug render mode (normal / wireframe / batch-tint) -- backs gui()->debug_set/get_render_mode.
   The flush reads it to pick the fill vs. wireframe pipeline and the per-draw debug push constants. */

void                gui_render_set_mode     ( gui_render_mode_t mode );
gui_render_mode_t   gui_render_get_mode     ( void );

/* The effect band's frame clock (gui.h, GUI_FX_TIME_WRAP) -- pushed to the shader as pc.time.
   Set once per app frame from frame_begin; the caller wraps.  Same one-way frontend -> backend
   seam as set_mode: this server has no view of the IO snapshot that carries the clock. */

void                gui_render_set_time     ( f32 seconds );

/*==============================================================================================
    DEBUG OVERLAY (gui_debug_overlay.c) -- Debug builds only.

    The GUI_DEBUG_OVERLAY switch, the DBG_* capture macros, and the capture/lifecycle decls
    live in debug/gui_debug.h: they are cross-server debug tooling (the
    interact server stamps DBG_WIDGET / DBG_NAME) and the servers never include each other's
    headers -- the debug header reaches every unit through the umbrella.  The implementation
    stays in this unit (gui_debug_overlay.c batches into GPU buffers).
==============================================================================================*/

/*==============================================================================================
    PIPELINE DASHBOARD (render/gui_dash_capture.c + gui_dashboard.c) -- Debug builds only.

    A visual diagnostic of the render pipeline itself: memory maps of the shared quad
    arena (per-window geometry slots with their padded reservations, volatile sub-slots, the
    debug-band boundary, high-water marks), the per-surface frames-in-flight regions and upload
    spans, the dispatch-order draw batches, and the EMIT buffer usage vs caps.

    Split across the two units the same way the feature itself is split:

        debug/gui_dashboard.c              -- the WINDOW + every panel painter: an ordinary
                                              GUI_WIN_DEBUG_BAND window drawn with the standard
                                              draw API and normal tooltips.  The band system
                                              (GUI_WIN_DEBUG_BAND, gui.h) is what keeps it
                                              honest: its geometry packs after every main-band
                                              slot and the stats/any_changed signals ignore it.
        render/gui_dash_capture.c         -- the CAPTURE: copies the snapshot types below at
                                              defined pipeline points (end of cache_build_frame,
                                              end of each surface's flush) for the shell to read
                                              one frame later through dash_snapshot().

    The build switch mirrors GUI_DEBUG_OVERLAY: auto-on for Debug builds, force-off with
    GUI_NO_PIPELINE_DASHBOARD.  Computed here so BOTH units agree.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( GUI_PIPELINE_DASHBOARD ) && !defined( GUI_NO_PIPELINE_DASHBOARD )
    #define GUI_PIPELINE_DASHBOARD
#endif
#if defined( GUI_NO_PIPELINE_DASHBOARD ) && defined( GUI_PIPELINE_DASHBOARD )
    #undef GUI_PIPELINE_DASHBOARD
#endif


#ifdef GUI_PIPELINE_DASHBOARD

    /*------------------------------------------------------------------------------------------
        Pipeline snapshot -- copied at two defined pipeline moments (end of cache_build_frame,
        end of each surface's gui_render_flush) so the dashboard displays a coherent picture,
        never mid-mutation, and can freeze it.  These types live in the seam header because the
        SHELL (debug/gui_dashboard.c) now draws every panel itself with the standard draw API,
        reading the snapshot through dash_snapshot(); the backend keeps only the capture
        (render/gui_dash_capture.c).  Plain data mirrors -- no backend-private type leaks.
    ------------------------------------------------------------------------------------------*/

    typedef struct                       /* win_geo_slot_t + this frame's diff verdict */
    {
        gui_id_t win;
        u32      z, band;
        i32 vp;
        u32      quad_base, quad_count, quad_alloc;   /* absolute position, live count, reservation */
        u32      cmd_base,  cmd_count;
        u32      tess_gen;
        bool     valid, changed;

    } dash_slot_t;

    typedef struct                       /* gui_gpu_cmd_t + its parallel arrays, flattened */
    {
        u32        elem_count, tex_idx, qbase;        /* elem_count and qbase count quads */
        i32        vp;                   /* GUI_VP_INVALID = dormant volatile pad */
        gui_rect_t clip;

    } dash_cmd_t;

    typedef struct                       /* gui_volatile_slot_t, display fields only */
    {
        gui_id_t id, win;
        u32      tess_gen;
        u32      lquad_base, quad_count, quad_alloc;  /* slot-relative position, live count, reservation */
        u32      cmd_count,  cmd_alloc;
        bool     active, hidden;

    } dash_vol_t;

    typedef struct                       /* one surface's FLUSH capture */
    {
        bool live;
        u32  frame_index;
        u32  quad_lo, quad_hi;                             /* quad span; lo >= hi = nothing uploaded */
        u32  up_bytes, up_batches, draw_calls;

    } dash_surf_t;

    typedef struct
    {
        u32  serial;                                     /* bumped per build capture; stale when frozen */

        /* BUILD capture -- end of cache_build_frame. */
        dash_slot_t slots[ RENDER_MAX_WIN ];     u32 slot_count;
        u8          dispatch[ RENDER_MAX_WIN ];  u32 dispatch_count;   /* slot indices, z-sorted */
        dash_cmd_t  cmds[ GUI_MAX_CMDS ];        u32 cmd_count;
        dash_vol_t  vols[ GUI_MAX_VOLATILE ];    u32 vol_count;

        u32  tess_quads, quad_hwm;                       /* quads: live fill + lifetime peak */
        /* Style-arena fill + lifetime peak (both bands -- the arena carries no band split), and how
           much of that fill is FX PAGES rather than styles.  The two share GUI_MAX_PRIMS but fill
           for opposite reasons: a style is one distinct look and dedups hard, an fx page holds four
           per-instance records (turn / phase / border colour / uv rect) and dedups only across a
           consecutive run.  tess_prims - tess_fx_pages is the style count. */
        u32  tess_prims, prim_hwm, tess_fx_pages;
        u32  tess_cmds;                                  /* LIVE GPU draw cmds, both bands (dormant/empty excluded) */
        u32  tess_cmds_dbg;                              /* of tess_cmds, the debug band's share     */
        bool overflow_ever;                              /* any cap spilled this run (see the gui log) */
        u32  band0_quad_end;                             /* main arena ends here; past = debug band */
        u32  band0_quad_hwm;                             /* lifetime peak of the main band alone     */
        u32  text_quads, text_runs;                      /* of live_quads, the glyph share and its runs */
        u32  band0_text_quads, band0_text_runs;          /* the same, main band alone                */
        /* Quads that actually draw: the slots' quad_count summed.  tess_quads above is the arena
           WRITE HEAD and carries every slot's quad_alloc padding, so it is a capacity figure, not a
           count -- read shares of the geometry against these two, never against tess_quads. */
        u32  live_quads, band0_live_quads;
        u32  emit_cmds, emit_segs, emit_pts, emit_rects, emit_text, emit_clips;
        u32  emit_cmds_hwm;                              /* running high-water of emit_cmds across captures */
        /* Debug-band share of each shared emit pool, derived from the segment table at capture (the
           emit hot paths carry no per-band counters).  band-0 usage = total - _dbg. */
        u32  emit_cmds_dbg, emit_segs_dbg, emit_pts_dbg, emit_rects_dbg, emit_text_dbg, emit_clips_dbg;
        u32  diff_unchanged;  bool any_changed;
        u32  tess_gen_next;
        u32  font_atlas;                                 /* live font atlas tex index (batch coloring) */

        gui_render_stats_t stats;                        /* last published frame (one-frame lag) */
        u32  draw_call_hwm;

        /* FLUSH capture -- end of gui_render_flush, per surface. */
        dash_surf_t surf[ GUI_MAX_VIEWPORTS ];

    } dash_snapshot_t;

    /* Shell seam (called from gui_dashboard.c).  set_enabled gates the captures -- call it every
       frame, open or closed, so a closed dashboard costs two branches; snapshot() returns the
       held capture (stable while frozen).  The shell reads it one frame behind the build that
       produced it -- the standard self-measurement lag. */
    const dash_snapshot_t* dash_snapshot   ( void );
    void                   dash_set_enabled( bool on );
    void                   dash_set_freeze ( bool on );
    bool                   dash_frozen     ( void );

    /* Capture hooks, called from the pipeline files (which the unity chain includes before
       gui_dash_capture.c) via the DASH_* macros below:
         dash_capture_build -- end of cache_build_frame: slot table, dispatch order, tess
                               counters, volatile registry, emit counters, stats.
         dash_capture_flush -- end of gui_render_flush: one surface's frame index, upload spans,
                               upload bytes/batches and draw calls. */
    void dash_capture_build( void );
    void dash_capture_flush( i32 vp, u32 frame, u32 quad_lo, u32 quad_hi,
                             u32 bytes, u32 batches, u32 draws );

    #define DASH_CAPTURE_BUILD()        dash_capture_build()
    #define DASH_CAPTURE_FLUSH( ... )   dash_capture_flush( __VA_ARGS__ )

#else
    #define DASH_CAPTURE_BUILD()        ( (void)0 )
    #define DASH_CAPTURE_FLUSH( ... )   ( (void)0 )
#endif

/*==============================================================================================
    Command stepper -- freeze one frame's semantic command list and replay a prefix of it, so
    UI generation can be stepped command by command (render/gui_step_capture.c has the full
    mechanism).  Two halves:

        render/gui_step_capture.c         -- the CAPTURE + RESTORE: snapshots the band-0
                                              command list at the build seam, then pre-loads
                                              s_draw with the frozen prefix at every draw_reset
                                              while frozen; live band-0 pushes are suppressed
                                              at the source (STEP_EMIT_SUPPRESSED).

        frame/gui_frame_overlay.c hotkeys  -- phase-1 controls (F8 freeze/release, , . step);
                                              a stepper window replaces them in a later phase.

    The GUI_CMD_STEPPER switch and the STEP_SET_OWNER attribution seam are computed in
    debug/gui_debug.h -- the interact server's item protocol stamps the owner, so the
    switch must reach every unit through the umbrella.  The rest of the mechanism stays here.
==============================================================================================*/

#ifdef GUI_CMD_STEPPER

    /* Shell seam (the stepper window + debug_hotkeys).  capture/release/seek LATCH: capture
       applies at the next cache_build_frame, release and the cursor at the next frame's
       draw_reset -- a frame is never half live, half frozen.  Every latched request self-raises
       step_pending(), which frame_begin folds into frame_dirty (STEP_FRAME_PENDING) so the
       serving emit always runs -- per-context wants_redraw is NOT reliable for this (any later
       ctx_begin wipes it).  count is the frozen band-0 command total (0 while live); seek
       clamps to it. */
    void step_capture( void );
    void step_release( void );
    bool step_pending( void );
    bool step_frozen ( void );
    u32  step_count  ( void );
    u32  step_cursor ( void );
    void step_seek   ( u32 cursor );

    /* Display/replay order: emit (generation order, the default) or paint (segments z-sorted,
       approximating dispatch).  The cursor, both info queries below, and the replay itself all
       live in the active order; toggling keeps the cursor's numeric position. */
    void step_set_paint_order( bool on );
    bool step_paint_order    ( void );

    /* Inspector read seam -- one frozen command / segment resolved for display, valid only while
       frozen (both return false otherwise).  Resolution happens backend-side because the frozen
       side pools live there: bounds are the command's pixel bbox (pool-walked for polyline /
       rect_list; TEXT walks the ACTIVE font's advances, so a run frozen in another font measures
       approximately), clip is the frozen scissor rect, text the NUL-terminated frozen pool string
       (TEXT only, stable until release), win/z/vp/font the owning segment's tag. */
    typedef struct
    {
        gui_cmd_t   cmd;      /* the raw frozen command; the shell decodes the union per type */
        gui_rect_t  bounds;   /* pixel bbox (highlight aid; TEXT/thick strokes approximate) */
        gui_rect_t  clip;     /* frozen scissor rect the command renders under */
        const char* text;     /* TEXT: frozen pool string; NULL for every other type */
        gui_id_t    win;      /* owning segment tag (the retained-cache window key) */
        gui_id_t    owner;    /* emitting widget id (0 = chrome/background) */
        u32         z;        /* owning segment's paint order */
        i32         vp;       /* owning segment's surface */
        u32         font;     /* TEXT / TEXT_XF: the run's own font id; 0 for every other type */

    } step_cmd_info_t;

    typedef struct
    {
        gui_id_t   win;
        u32        z;         /* the segment key -- no font: it is per COMMAND now (gui.h) */
        i32        vp;        /* the segment's surface */
        u32        lo, hi;    /* frozen command range [lo, hi) -- seek targets */
        gui_rect_t bounds;    /* union of the member commands' bboxes */

    } step_seg_info_t;

    bool step_cmd_info ( u32 index, step_cmd_info_t* out );
    u32  step_seg_count( void );
    bool step_seg_info ( u32 index, step_seg_info_t* out );

    /* Pick: topmost VISIBLE frozen command whose bounds contain the point on viewport `vp` --
       "what drew this pixel".  Always resolves topmost in PAINT order (whatever the display
       mode), respects each command's frozen scissor, and returns the hit's DISPLAY position.
       A topmost hit that is unattributed chrome/background (owner 0) refuses the pick -- a
       missed click is a no-op, never a seek onto a window's body fill.  False while live, on
       a miss, or on a chrome hit. */
    bool step_pick( f32 x, f32 y, i32 vp, u32* out_index );

    /* The attribution stamp (draw_set_cmd_owner / STEP_SET_OWNER) is declared in
       debug/gui_debug.h -- the interact server calls it; the definition stays in
       gui_emit_draw.c. */

    /* Pipeline hooks, called via the STEP_* macros below (defined in gui_step_capture.c, which
       the unity chain includes LAST):
         step_capture_build -- start of cache_build_frame: segments closed, pools complete.
         step_restore_emit  -- end of draw_reset: pre-load the frozen prefix while frozen. */
    void step_capture_build( void );
    void step_restore_emit ( void );

    #define STEP_CAPTURE_BUILD()      step_capture_build()
    #define STEP_RESTORE_EMIT()       step_restore_emit()
    #define STEP_FRAME_PENDING()      step_pending()
    /* True while a frozen frame is replayed and the current emission targets the main band --
       every such push is dropped at the source so the live UI underneath cannot disturb the
       replay.  Expanded only inside the emit unit, where s_draw is in scope. */
    #define STEP_EMIT_SUPPRESSED()    ( step_frozen() && s_draw.cur_band == 0 )

#else
    #define STEP_CAPTURE_BUILD()      ( (void)0 )
    #define STEP_RESTORE_EMIT()       ( (void)0 )
    #define STEP_FRAME_PENDING()      ( false )
    #define STEP_EMIT_SUPPRESSED()    ( false )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GUI_RENDER_H
