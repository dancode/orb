#ifndef GUI_BACKEND_H
#define GUI_BACKEND_H
/*==============================================================================================

    runtime_service/gui/gui_backend.h -- The render-backend interface (the unit seam).

    gui is built as TWO unity translation units that link into one static lib:

        gui.c          -- the UI / core unit: context, layout, widgets, chrome, popups, nav,
                          input, frame lifecycle, the module vtable.  Owns s_build / s_io /
                          s_interaction / g_ctx and the stacks.
        gui_backend.c  -- the render backend unit: fonts, the CPU draw list, path stroking,
                          CPU tessellation, the GPU flush, and the debug overlay.  Owns
                          s_draw / s_tess / s_font / s_render.

    The UI unit produces a semantic draw list by calling the draw_* / font_* primitives below;
    the backend unit tessellates and uploads it.  This header is the entire surface between them
    -- the functions the backend exports to the UI, plus the debug-overlay instrumentation both
    sides share.  The reverse direction is almost nothing: the backend pulls only rect_intersect
    (a stateless helper in gui_internal.h) and, for the debug overlay, the ambient build
    viewport via gui_dbg_build_viewport().

    The module API pointers (rhi() / app()) are NOT redefined here: g_rhi_api_ptr / g_app_api_ptr
    have external linkage, defined and fetched once in gui.c (MOD_USE_RHI / MOD_USE_APP); the
    backend unit reads them through the same inline accessors from rhi_api.h / app_api.h.

    Included once at the top of each unity entry (gui.c and gui_backend.c).

    Sections below are grouped by pipeline stage, matching the include order in gui_backend.c and
    named for the function prefix each stage exports.  Tessellation primitives (gui_build_tess.c)
    have no public surface -- driven entirely from within BUILD -- so there is no section for them.

    0. Backend lifecycle (gui_backend_init/exit)
    1. Fonts (resource registry)
    2. Runtime icon atlas (resource registry)
    3. EMIT -- CPU draw list
    4. BUILD -- retained cache
    5. RENDER -- GPU flush
    6. DEBUG OVERLAY

==============================================================================================*/

#include "runtime_service/gui/gui_internal.h"

// clang-format off
/*==============================================================================================
    Backend lifecycle (gui_backend.c) -- the seam the UI unit calls to stand up / tear down the
    whole render backend.  `caps` (gui_backend_caps_t, gui.h) latches which optional layers are
    active for this run -- gui_init_config_back()'s value, or GUI_CAPS_DEFAULT if never called; see
    s_caps at the top of gui_backend.c for how the rest of the unit reads it.  Internally wraps
    gui_render_init/shutdown (gui_render.c), which are no longer exposed past this header.
==============================================================================================*/

bool gui_backend_init( gui_backend_caps_t caps );
void gui_backend_exit( void );

/*==============================================================================================
    Fonts (resource/gui_font.c; implementation in resource/gui_font_internal.c)
==============================================================================================*/

u32  font_load              ( const char* path );           // load a .orb_font into a new id, activate it (0=fail)
bool font_load_into         ( u32 id, const char* path );   // load a .orb_font into an existing id (id 0 = default)
bool font_load_builtin      ( gui_builtin_font_t font );    // load a built-in preset (gui.h) into slot 0; true no-op for GUI_FONT_NONE
void font_use                ( u32 id );                    // make an already-loaded id the active font
u32  font_active_id         ( void );                       // id of the active font slot (save/restore for push/pop)
bool font_valid              ( void );                      // true once a font is activated -- gate metric/glyph reads on this
u32  font_slot_atlas_idx    ( u32 id );                     // live bindless atlas index backing a font id (0 if empty)
gui_vec2_t font_slot_atlas_size( u32 id );                  // live atlas pixel dimensions backing a font id
bool font_flush_pending     ( void );                       // commit deferred (re)loads; true if the active font changed

f32  font_char_h            ( void );                       // glyph-box height of the active font (ascent+descent)
f32  font_line_h            ( void );                       // line advance of the active font
f32  font_em                ( void );                       // nominal type size (em) -- the layout proportion base
f32  font_char_advance      ( u8 ch );                      // horizontal advance of one glyph
void font_print_active      ( void );                       // log the active font's name and metrics
f32  font_text_w            ( const char* str );            // pixel width of a NUL-terminated run
f32  font_text_w_n          ( const char* str, u32 n );     // pixel width of the first n characters

/* Glyph atlas lookup: UVs, pen offsets, glyph box, and advance for one character. */
void font_glyph             ( u8 ch, f32* u0, f32* v0, f32* u1, f32* v1,
                                     f32* ox, f32* oy, f32* gw, f32* gh, f32* advance );

/*==============================================================================================
    Runtime icon atlas (resource/gui_icon.c)

    A second R8 coverage texture, built at runtime: callers register raw monochrome bitmaps and
    the atlas packs them with stb_rect_pack, handing back an gui_icon_id_t.  draw_push_icon
    (pipeline/gui_emit_draw.c, EMIT section below) draws through the existing textured-quad path
    using icon_get's cached UVs, so icons batch in the same flush as text and tint by vertex
    color.  GPU re-upload is deferred to frame_begin (icon_atlas_flush_upload).
==============================================================================================*/

bool            icon_atlas_init         ( void );   // create the R8 atlas texture + bindless index
void            icon_atlas_shutdown     ( void );   // destroy the atlas, free CPU staging
void            icon_atlas_flush_upload ( void );   // re-upload the CPU atlas to the GPU if dirty

gui_icon_id_t   icon_register           ( const char* name, u32 w, u32 h, const u8* coverage );
gui_icon_id_t   icon_find               ( const char* name );
bool            icon_get                ( gui_icon_id_t id,
                                          f32* u0, f32* v0, f32* u1, f32* v1, u32* w, u32* h );

/*==============================================================================================
    EMIT: CPU draw list (pipeline/gui_emit_draw.c)
==============================================================================================*/

void draw_reset( i32 display_w, i32 display_h );    // clear the list at the top of frame_begin

void draw_set_alpha             ( f32 a );          // global opacity multiplier folded into every pushed shape
void draw_set_rounding          ( f32 r );          // corner radius folded into every pushed filled/outline rect
f32  draw_rounding              ( void );           // current ambient radius (save/restore around a sub-element)
void draw_set_text_clip_x       ( f32 x0, f32 x1 ); // glyph-clip window folded into every pushed text run
void draw_clear_text_clip       ( void );           // restore the no-clip sentinel (unbounded text)
void draw_set_sort_key          ( u32 z );          // paint order stamped on new commands (window z)
void draw_set_viewport          ( u32 vp );         // viewport index stamped on new commands (surface routing)
void draw_set_band              ( u32 band );       // arena band: 0 = main UI, 1 = debug (GUI_WIN_DEBUG_BAND)
u32  draw_band                  ( void );           // current band (sampled for popup band inheritance)
void draw_set_window            ( gui_id_t win );   // stable window id stamped on new commands (cache key)
void draw_set_font              ( u32 font );       // active font id -> per-segment atlas batch context (push/pop/use_font)

gui_draw_scope_t draw_scope     ( void );              // paint cursor + glyph clip as one record
void             draw_scope_set ( gui_draw_scope_t s );// restore it wholesale (the overlay seam)

void draw_push_clip_rect        ( f32 x, f32 y, f32 w, f32 h ); // push clip, intersected with the parent
void draw_pop_clip_rect         ( void );                       // pop the top clip
void draw_push_clip_root        ( void );                       // push the full-display clip (popup escape)
void draw_set_root_clip         ( f32 w, f32 h );               // set clip_stack[0] to a surface size

void draw_push_rect_filled      ( f32 x, f32 y, f32 w, f32 h,
                                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr );

/* Push `count` solid rects as ONE semantic command (per-frame rect pool, one quad each at
   flush) -- the dense-shape escape valve for callers that would otherwise exhaust GUI_MAX_CMDS
   (timeline bars, graph columns).  Square, current clip, per-entry color. */
void draw_push_rect_list        ( const gui_rect_col_t* rects, u32 count );

/* Push one registered icon quad (atlas tex_idx + cached UVs from resource/gui_icon.c) into the
   draw list; no-op for an invalid id.  Reuses draw_push_rect_filled -- an icon is just a textured
   quad sourced from the icon atlas instead of the font atlas. */
void draw_push_icon             ( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, u32 abgr );

void draw_push_rect_gradient    ( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal );

void draw_push_rect_outline     ( f32 x, f32 y, f32 w, f32 h, f32 t, u32 tex_idx, u32 abgr );
void draw_push_triangle         ( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 tex_idx, u32 abgr );
void draw_push_circle_filled    ( f32 cx, f32 cy, f32 r, u32 segments, u32 abgr );
void draw_push_text             ( f32 x, f32 y, u32 abgr, const char* str );
void draw_push_text_n           ( f32 x, f32 y, u32 abgr, const char* str, u32 n );
void draw_push_text_clip_n      ( f32 x, f32 y, u32 abgr, const char* str, u32 n,
                                  f32 clip_x0, f32 clip_x1 );

/*==============================================================================================
    BUILD: retained frame-geometry cache (pipeline/gui_build_cache.c)
==============================================================================================*/

/* Retained-cache capacities.  Here (not in the .c files) because the dashboard snapshot types
   below size their arrays with them; both unity units see one definition. */
#define RENDER_MAX_WIN    32    // distinct windows tracked per frame
#define GUI_MAX_VOLATILE  16    // registered volatile sub-slot rows
#define SLOT_VERT_PAD     64u   // per-slot vertex headroom: absorbs minor growth in-place
#define SLOT_IDX_PAD      128u  // per-slot index headroom (~2x vertex count for quads)

/* Drop the once-per-frame tessellation cache so the next flush rebuilds the shared geometry.
   The frame's semantic list is tessellated + z-sorted exactly once (lazily, on the first
   surface flush); every other live surface that frame reuses the result.  Called by
   gui_frame_begin right after draw_reset, before the build emits any new commands. */

void                gui_build_frame_reset( void );

/* Per-frame render stats: gui_render_stats returns the last published frame's totals;
   gui_build_stats_publish promotes the in-progress accumulator to the published value and
   resets it -- called once per frame by gui_frame_begin (the UI unit), before draw_reset. */

gui_render_stats_t  gui_render_stats        ( void );
void                gui_build_stats_publish( void );

/* Retained-skip optimization: when on (default), an unchanged frame (all per-window hashes match
   the previous frame) skips tessellation and reuses s_tess.  Toggle for benchmarking or debugging. */

void                gui_build_set_retained_skip( bool on );
bool                gui_build_retained_skip( void );

/* True when the PREVIOUS frame's render produced any change (a window appeared, vanished, or
   changed content).  Read from the UI unit during frame_begin (before this frame's cache_build_frame
   runs) so s_cache.any_changed still holds last frame's result.  Used with io_dirty and wants_redraw
   to decide whether to skip the widget emit phase entirely (Level 3 retained skip). */

bool                gui_build_any_changed( void );

/* Debug: print the cached-geometry slot table (window, z/vp, vertex/index/command bounds) to
   stdout.  On-demand companion to the per-frame disjoint-layout assert that runs in debug builds. */

void                gui_build_dump_geometry( void );

/*----------------------------------------------------------------------------------------------
    Volatile widgets -- an inline-emit callback replayed in place on frames the UI build is
    skipped, so a purely cosmetic animation never forces the whole UI to re-run every frame.

    The feature's actual logic is entirely in two files, one per unit -- read those for the full
    picture; this header is only the boundary between them:

        widgets/gui_volatile.c            -- UI unit: gui()->volatile_cb/_begin/_end (gui_api.h),
                                             the replay scope (layout + id), gui_replay_scope_enter/_exit.
        backend/pipeline/gui_build_volatile.c -- BUILD unit: the registry, capture at real emit, and
                                             gui_update_volatile (run internally by frame_end).

    Forward direction (core -> backend, the normal call direction for this header): gui_volatile_cb
    (widgets/gui_volatile.c) wraps one real-emit invocation of a callback with these three calls --
    gui_volatile_cb_open records where its commands start, gui_volatile_stamp (called from inside
    the callback body, by gui_volatile_begin) records the window/z/vp/font/clip context and the
    layout cursor position, and gui_volatile_cb_close records where they end and tags the range.
    tess_dispatch (gui_build_tess.c) then reserves the block a padded region of its window's slot
    (vertices, indices, and its own GPU commands, each with headroom past the live geometry).
    gui_update_volatile is called internally by gui_frame_end on frames where
    frame_dirty() is false: it re-invokes each row's callback standalone, re-tessellates the
    result, and patches it into the reserved region -- any output that FITS the reservation is
    accepted (text may grow/shrink etc); only outgrowing it falls back to one real frame, which
    recaptures at the larger size.

    Reverse direction (backend -> core): gui_update_volatile needs a valid layout/id scope for the
    callback to emit into, which only the UI unit owns (lf(), the id stack).  gui_replay_scope_enter
    / _exit are the two functions that cross back -- the same kind of unit-seam exception as
    gui_dbg_build_viewport above, just two of them instead of one.
----------------------------------------------------------------------------------------------*/

void     gui_volatile_cb_open ( gui_id_t id );                 // (re)open row `id`; cmd_lo = current cmd_count
void     gui_volatile_stamp   ( f32 x, f32 y, f32 w );          // fill win/z/vp/font/clip + cursor stamp for the open row
void     gui_volatile_cb_close( gui_volatile_fn fn );           // cmd_hi + fn for the open row; tags the command range
void     gui_update_volatile  ( void );

/* Implemented in the UI unit (widgets/gui_volatile.c); called only from gui_update_volatile. */
void     gui_replay_scope_enter( gui_id_t id, f32 x, f32 y, f32 w );
void     gui_replay_scope_exit ( bool force_redraw );

/*==============================================================================================
    RENDER: GPU resources + flush (pipeline/gui_render.c)

    gui_render_init/shutdown are NOT declared here -- they're an implementation detail of
    gui_backend_init/exit (above) now, called directly within the gui_backend.c unity TU.
==============================================================================================*/

void                gui_render_flush        ( gui_viewport_t* vp, u32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h );

/* Fill the backend-owned buckets of the memory breakdown: GPU geometry buffers (scaled by the
   caller-supplied live-surface count), the font atlas texture, and every fixed CPU .bss buffer
   (draw list, tessellation staging, retained cache, font registry).  The CPU-heap context bytes
   and the totals are filled by the frontend (gui_mem_stats), which owns the context pool. */
gui_mem_stats_t     gui_render_memory       ( u32 live_viewports );

/* Debug render mode (normal / wireframe / batch-tint) -- backs gui()->debug_set/get_render_mode.
   The flush reads it to pick the fill vs. wireframe pipeline and the per-draw debug push constants. */

void                gui_render_set_mode     ( gui_render_mode_t mode );
gui_render_mode_t   gui_render_get_mode     ( void );

bool                viewport_create         ( gui_viewport_t* vp, rhi_texture_t target, i32 win_id ); // a surface's vb/ib
void                viewport_destroy        ( gui_viewport_t* vp );                                   // free its vb/ib

/*==============================================================================================
    DEBUG OVERLAY (gui_debug_overlay.c) -- Debug builds only.

    A second draw list, captured from the UI via the DBG_* macros and flushed last, on top.  The
    build switch is computed here so BOTH units agree before the macros / capture decls are used:
    the build tool defines GUI_DEBUG_OVERLAY for the Debug config; the MSVC _DEBUG macro is a
    fallback so the feature tracks the configuration even before a build_tool regen.  Define
    GUI_NO_DEBUG_OVERLAY to force it off.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( GUI_DEBUG_OVERLAY ) && !defined( GUI_NO_DEBUG_OVERLAY )
    #define GUI_DEBUG_OVERLAY
#endif
#if defined( GUI_NO_DEBUG_OVERLAY ) && defined( GUI_DEBUG_OVERLAY )
    #undef GUI_DEBUG_OVERLAY
#endif

#ifdef GUI_DEBUG_OVERLAY

    /* Lifecycle, driven by gui_frame.c (UI unit) under the same #ifdef. */
    bool gui_debug_init    ( void );
    void gui_debug_shutdown( void );
    void gui_debug_reset   ( void );
    void gui_debug_flush   ( gui_vp_t vp, rhi_cmd_t cmd, i32 win_w, i32 win_h );

    /* Capture entry points -- called from both units via the DBG_* macros below.  Each tags its
       command with the ambient build viewport (gui_dbg_build_viewport, gui_ctx.c). */
    void dbg_capture_widget( gui_id_t id, gui_rect_t r, bool hover, bool active );
    void dbg_capture_clip  ( gui_rect_t r, u32 depth );
    void dbg_capture_window( gui_rect_t r, bool is_hover );
    void dbg_capture_resize( gui_rect_t band, u8 hot_edges );
    void dbg_capture_layout( gui_rect_t r );

    /* Name registry -- records the source string behind an id as it is minted (widget label,
       window/popup title, region/child/table id string), so gui_state_overlay() can show a
       readable name instead of a hash.  See gui_debug_name() in gui_host.h for the reader. */
    void dbg_name_register( gui_id_t id, const char* str );

    #define DBG_WIDGET( id, r, hov, act ) dbg_capture_widget( ( id ), ( r ), ( hov ), ( act ) )
    #define DBG_CLIP( r, depth )          dbg_capture_clip( ( r ), ( depth ) )
    #define DBG_WINDOW( r, is_hover )     dbg_capture_window( ( r ), ( is_hover ) )
    #define DBG_RESIZE( band, hot )       dbg_capture_resize( ( band ), ( hot ) )
    #define DBG_LAYOUT( r )               dbg_capture_layout( ( r ) )
    #define DBG_NAME( id, str )           dbg_name_register( ( id ), ( str ) )

    /* Ambient build viewport (s_build.win.viewport, gui_ctx.c) -- the capture functions live in/usage
       the backend unit, so they read it through this accessor rather than the UI-unit static. */
    u32 gui_dbg_build_viewport( void );

#else
    #define DBG_WIDGET( id, r, hov, act ) ( (void)0 )
    #define DBG_CLIP( r, depth )          ( (void)0 )
    #define DBG_WINDOW( r, is_hover )     ( (void)0 )
    #define DBG_RESIZE( band, hot )       ( (void)0 )
    #define DBG_LAYOUT( r )               ( (void)0 )
    #define DBG_NAME( id, str )           ( (void)0 )
#endif

/*==============================================================================================
    PIPELINE DASHBOARD (backend/gui_dash_capture.c + gui_dashboard.c) -- Debug builds only.

    A visual diagnostic of the render pipeline itself: memory maps of the shared vertex/index
    arena (per-window geometry slots with their padded reservations, volatile sub-slots, the
    debug-band boundary, high-water marks), the per-surface frames-in-flight regions and upload
    spans, the dispatch-order draw batches, and the EMIT buffer usage vs caps.

    Split across the two units the same way the feature itself is split:

        gui_dashboard.c (UI unit)          -- the WINDOW + every panel painter: an ordinary
                                              GUI_WIN_DEBUG_BAND window drawn with the standard
                                              draw API and normal tooltips.  The band system
                                              (GUI_WIN_DEBUG_BAND, gui.h) is what keeps it
                                              honest: its geometry packs after every main-band
                                              slot and the stats/any_changed signals ignore it.
        backend/gui_dash_capture.c         -- the CAPTURE: copies the snapshot types below at
                                              defined pipeline points (end of cache_build_frame,
                                              end of each surface's flush) for the shell to read
                                              one frame later through gui_dash_snapshot().

    The build switch mirrors GUI_DEBUG_OVERLAY: auto-on for Debug builds, force-off with
    GUI_NO_PIPELINE_DASHBOARD.  Computed here so BOTH units agree.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( GUI_PIPELINE_DASHBOARD ) && !defined( GUI_NO_PIPELINE_DASHBOARD )
    #define GUI_PIPELINE_DASHBOARD
#endif
#if defined( GUI_NO_PIPELINE_DASHBOARD ) && defined( GUI_PIPELINE_DASHBOARD )
    #undef GUI_PIPELINE_DASHBOARD
#endif

/* The dashboard window's id (id_hash of its title), defined in gui_dashboard.c (UI unit); stays
   0 when the feature is compiled out or the window has never been emitted.  Read by the dash
   capture to mark the dashboard's own slot in the memory map ("observer marked, not hidden");
   its stats/idle-skip exemption now comes from GUI_WIN_DEBUG_BAND, not from this id. */
extern gui_id_t g_gui_dash_window_id;

#ifdef GUI_PIPELINE_DASHBOARD

    /*------------------------------------------------------------------------------------------
        Pipeline snapshot -- copied at two defined pipeline moments (end of cache_build_frame,
        end of each surface's gui_render_flush) so the dashboard displays a coherent picture,
        never mid-mutation, and can freeze it.  These types live in the seam header because the
        SHELL (gui_dashboard.c, UI unit) now draws every panel itself with the standard draw API,
        reading the snapshot through gui_dash_snapshot(); the backend keeps only the capture
        (backend/gui_dash_capture.c).  Plain data mirrors -- no backend-private type leaks.
    ------------------------------------------------------------------------------------------*/

    typedef struct                       /* win_geo_slot_t + this frame's diff verdict */
    {
        gui_id_t win;
        u32      z, vp, band;
        u32      vert_base, vert_count, vert_alloc;
        u32      idx_base,  idx_count,  idx_alloc;
        u32      cmd_base,  cmd_count;
        u32      tess_gen;
        bool     valid, changed;

    } dash_slot_t;

    typedef struct                       /* gui_gpu_cmd_t + its parallel arrays, flattened */
    {
        u32        elem_count, tex_idx, vp, vbase, ibase;
        gui_rect_t clip;

    } dash_cmd_t;

    typedef struct                       /* gui_volatile_slot_t, display fields only */
    {
        gui_id_t id, win;
        u32      tess_gen;
        u32      lvert_base, vert_count, vert_alloc;
        u32      lidx_base,  idx_count,  idx_alloc;
        u32      cmd_count,  cmd_alloc;
        bool     active, hidden;

    } dash_vol_t;

    typedef struct                       /* one surface's FLUSH capture */
    {
        bool live;
        u32  frame_index;
        u32  vtx_lo, vtx_hi, idx_lo, idx_hi;             /* lo >= hi means nothing uploaded */
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

        u32  tess_verts, tess_idx, vert_hwm, idx_hwm;
        u32  tess_cmds;                                  /* LIVE GPU draw cmds, both bands (dormant/empty excluded) */
        u32  tess_cmds_dbg;                              /* of tess_cmds, the debug band's share     */
        bool overflow_ever;
        u32  band0_vert_end, band0_idx_end;              /* main arena ends here; past = debug band */
        u32  band0_vert_hwm, band0_idx_hwm;              /* lifetime peak of the main band alone     */
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
    const dash_snapshot_t* gui_dash_snapshot   ( void );
    void                   gui_dash_set_enabled( bool on );
    void                   gui_dash_set_freeze ( bool on );
    bool                   gui_dash_frozen     ( void );

    /* Capture hooks, called from the pipeline files (which the unity chain includes before
       gui_dash_capture.c) via the DASH_* macros below:
         dash_capture_build -- end of cache_build_frame: slot table, dispatch order, tess
                               counters, volatile registry, emit counters, stats.
         dash_capture_flush -- end of gui_render_flush: one surface's frame index, upload spans,
                               upload bytes/batches and draw calls. */
    void dash_capture_build( void );
    void dash_capture_flush( u32 vp, u32 frame, u32 vtx_lo, u32 vtx_hi, u32 idx_lo, u32 idx_hi,
                             u32 bytes, u32 batches, u32 draws );

    #define DASH_CAPTURE_BUILD()        dash_capture_build()
    #define DASH_CAPTURE_FLUSH( ... )   dash_capture_flush( __VA_ARGS__ )

#else
    #define DASH_CAPTURE_BUILD()        ( (void)0 )
    #define DASH_CAPTURE_FLUSH( ... )   ( (void)0 )
#endif

/*==============================================================================================
    Command stepper -- freeze one frame's semantic command list and replay a prefix of it, so
    UI generation can be stepped command by command (backend/gui_step_capture.c has the full
    mechanism).  Two halves:

        backend/gui_step_capture.c         -- the CAPTURE + RESTORE: snapshots the band-0
                                              command list at the build seam, then pre-loads
                                              s_draw with the frozen prefix at every draw_reset
                                              while frozen; live band-0 pushes are suppressed
                                              at the source (STEP_EMIT_SUPPRESSED).
        debug/gui_frame_overlay.c hotkeys  -- phase-1 controls (F8 freeze/release, , . step);
                                              a stepper window replaces them in a later phase.

    The build switch mirrors GUI_PIPELINE_DASHBOARD: auto-on for Debug builds, force-off with
    GUI_NO_CMD_STEPPER.  Computed here so BOTH units agree.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( GUI_CMD_STEPPER ) && !defined( GUI_NO_CMD_STEPPER )
    #define GUI_CMD_STEPPER
#endif
#if defined( GUI_NO_CMD_STEPPER ) && defined( GUI_CMD_STEPPER )
    #undef GUI_CMD_STEPPER
#endif

#ifdef GUI_CMD_STEPPER

    /* Frozen-replay flag, read per push by the emit hot path (STEP_EMIT_SUPPRESSED below).
       Defined in gui_step_capture.c; written only at the frame seams. */
    extern bool g_gui_step_frozen;

    /* Shell seam (the stepper window + debug_hotkeys).  capture/release/seek LATCH: capture
       applies at the next cache_build_frame, release and the cursor at the next frame's
       draw_reset -- a frame is never half live, half frozen.  Every latched request self-raises
       gui_step_pending(), which frame_begin folds into frame_dirty (STEP_FRAME_PENDING) so the
       serving emit always runs -- per-context wants_redraw is NOT reliable for this (any later
       ctx_begin wipes it).  count is the frozen band-0 command total (0 while live); seek
       clamps to it. */
    void gui_step_capture( void );
    void gui_step_release( void );
    bool gui_step_pending( void );
    bool gui_step_frozen ( void );
    u32  gui_step_count  ( void );
    u32  gui_step_cursor ( void );
    void gui_step_seek   ( u32 cursor );

    /* Display/replay order: emit (generation order, the default) or paint (segments z-sorted,
       approximating dispatch).  The cursor, both info queries below, and the replay itself all
       live in the active order; toggling keeps the cursor's numeric position. */
    void gui_step_set_paint_order( bool on );
    bool gui_step_paint_order    ( void );

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
        u32         z, vp, font;

    } step_cmd_info_t;

    typedef struct
    {
        gui_id_t   win;
        u32        z, vp, font;
        u32        lo, hi;    /* frozen command range [lo, hi) -- seek targets */
        gui_rect_t bounds;    /* union of the member commands' bboxes */

    } step_seg_info_t;

    bool gui_step_cmd_info ( u32 index, step_cmd_info_t* out );
    u32  gui_step_seg_count( void );
    bool gui_step_seg_info ( u32 index, step_seg_info_t* out );

    /* Pick: topmost VISIBLE frozen command whose bounds contain the point on viewport `vp` --
       "what drew this pixel".  Walks the visible prefix top-down in the active display order
       (exact topmost in paint order, an approximation in emit order) and respects each
       command's frozen scissor.  False while live or on a miss. */
    bool gui_step_pick( f32 x, f32 y, u32 vp, u32* out_index );

    /* Attribution stamp (defined in gui_emit_draw.c): widget_behavior marks each registering
       widget so the commands it paints carry its id -- STEP_SET_OWNER compiles away with the
       feature.  Window transitions and draw_reset clear it back to 0 (chrome). */
    void draw_set_cmd_owner( gui_id_t id );
    #define STEP_SET_OWNER( id )      draw_set_cmd_owner( id )

    /* Pipeline hooks, called via the STEP_* macros below (defined in gui_step_capture.c, which
       the unity chain includes LAST):
         step_capture_build -- start of cache_build_frame: segments closed, pools complete.
         step_restore_emit  -- end of draw_reset: pre-load the frozen prefix while frozen. */
    void step_capture_build( void );
    void step_restore_emit ( void );

    #define STEP_CAPTURE_BUILD()      step_capture_build()
    #define STEP_RESTORE_EMIT()       step_restore_emit()
    #define STEP_FRAME_PENDING()      gui_step_pending()
    /* True while a frozen frame is replayed and the current emission targets the main band --
       every such push is dropped at the source so the live UI underneath cannot disturb the
       replay.  Expanded only inside the emit unit, where s_draw is in scope. */
    #define STEP_EMIT_SUPPRESSED()    ( g_gui_step_frozen && s_draw.cur_band == 0 )

#else
    #define STEP_CAPTURE_BUILD()      ( (void)0 )
    #define STEP_RESTORE_EMIT()       ( (void)0 )
    #define STEP_FRAME_PENDING()      ( false )
    #define STEP_EMIT_SUPPRESSED()    ( false )
    #define STEP_SET_OWNER( id )      ( (void)0 )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GUI_BACKEND_H
