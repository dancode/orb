#ifndef IMGUI_BACKEND_H
#define IMGUI_BACKEND_H
/*==============================================================================================

    runtime_service/imgui/imgui_backend.h -- The render-backend interface (the unit seam).

    imgui is built as TWO unity translation units that link into one static lib:

        imgui.c          -- the UI / core unit: context, layout, widgets, chrome, popups, nav,
                            input, frame lifecycle, the module vtable.  Owns s_build / s_io /
                            s_interaction / g_ctx and the stacks.
        imgui_backend.c  -- the render backend unit: fonts, the CPU draw list, path stroking,
                            CPU tessellation, the GPU flush, and the debug overlay.  Owns
                            s_draw / s_tess / s_font / s_render.
    
    The UI unit produces a semantic draw list by calling the draw_* / font_* primitives below;
    the backend unit tessellates and uploads it.  This header is the entire surface between them
    -- the functions the backend exports to the UI, plus the debug-overlay instrumentation both
    sides share.  The reverse direction is almost nothing: the backend pulls only rect_intersect
    (a stateless helper in imgui_internal.h) and, for the debug overlay, the ambient build
    viewport via imgui_dbg_build_viewport().

    The module API pointers (rhi() / app()) are NOT redefined here: g_rhi_api_ptr / g_app_api_ptr
    have external linkage, defined and fetched once in imgui.c (MOD_USE_RHI / MOD_USE_APP); the
    backend unit reads them through the same inline accessors from rhi_api.h / app_api.h.

    Included once at the top of each unity entry (imgui.c and imgui_backend.c).

==============================================================================================*/

#include "runtime_service/imgui/imgui_internal.h"

// clang-format off
/*==============================================================================================
    Fonts (imgui_font.c / imgui_font_builtin.c)
==============================================================================================*/

bool tt_font_load       ( const char* path );       // load a TrueType font as the active font
void tt_font_unload     ( void );                   // drop the loaded TrueType font, revert to bitmap

void bitmap_font_select ( imgui_font_t font );      // select a built-in bitmap font as active
void bitmap_scale_set   ( u32 scale );              // integer upscale for the active bitmap font

f32  font_char_h        ( void );                   // glyph-box height of the active font (ascent+descent)
f32  font_line_h        ( void );                   // line advance of the active font
f32  font_em            ( void );                   // nominal type size (em) -- the layout proportion base
f32  font_char_advance  ( u8 ch );                  // horizontal advance of one glyph
bool font_is_tt         ( void );                   // true if a TrueType font is active (vs. a bitmap)
void font_print_active  ( void );                   // log the active font's type, name, and metrics
f32  font_text_w        ( const char* str );        // pixel width of a NUL-terminated run
f32  font_text_w_n      ( const char* str, u32 n ); // pixel width of the first n bytes

/* Glyph atlas lookup: UVs, pen offsets, glyph box, and advance for one character. */
void font_glyph         ( u8 ch,
                          f32* u0, f32* v0, f32* u1, f32* v1,
                          f32* ox, f32* oy, f32* gw, f32* gh,
                          f32* advance );

/*==============================================================================================
    Runtime icon atlas (imgui_icon.c)

    A second R8 coverage texture, built at runtime: callers register raw monochrome bitmaps and
    the atlas packs them with stb_rect_pack, handing back an imgui_icon_id_t.  Icons draw through
    the existing textured-quad path (own bindless tex_idx), so they batch in the same flush as
    text and tint by vertex color.  GPU re-upload is deferred to frame_begin (icon_atlas_flush_upload).
==============================================================================================*/

bool            icon_atlas_init        ( void );   // create the R8 atlas texture + bindless index
void            icon_atlas_shutdown    ( void );   // destroy the atlas, free CPU staging
void            icon_atlas_flush_upload ( void );  // re-upload the CPU atlas to the GPU if dirty

imgui_icon_id_t icon_register          ( const char* name, u32 w, u32 h, const u8* coverage );
imgui_icon_id_t icon_find              ( const char* name );
bool            icon_get               ( imgui_icon_id_t id,
                                         f32* u0, f32* v0, f32* u1, f32* v1, u32* w, u32* h );

/* Push one icon quad (atlas tex_idx + cached UVs) into the draw list; no-op for an invalid id. */
void            draw_push_icon         ( f32 x, f32 y, f32 w, f32 h, imgui_icon_id_t id, u32 abgr );

/*==============================================================================================
    CPU draw list (imgui_draw.c)
==============================================================================================*/

void draw_reset( i32 display_w, i32 display_h );   // clear the list at the top of new_frame

void draw_set_alpha     ( f32 a );    // global opacity multiplier folded into every pushed shape
void draw_set_rounding  ( f32 r );    // corner radius folded into every pushed filled/outline rect
f32  draw_rounding      ( void );     // current ambient radius (save/restore around a sub-element)
void draw_set_sort_key  ( u32 z );    // paint order stamped on new commands (window z)
u32  draw_sort_key      ( void );     // current sort key (saved/restored by the popup layer)
void draw_set_viewport  ( u32 vp );   // viewport index stamped on new commands (surface routing)
u32  draw_viewport      ( void );     // current viewport index

void draw_push_clip_rect ( f32 x, f32 y, f32 w, f32 h ); // push clip, intersected with the parent
void draw_pop_clip_rect  ( void );                       // pop the top clip
void draw_push_clip_root ( void );                       // push the full-display clip (popup escape)
void draw_set_root_clip  ( f32 w, f32 h );               // set clip_stack[0] to a surface size

void draw_push_rect_filled      ( f32 x, f32 y, f32 w, f32 h,
                                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr );

void draw_push_rect_gradient    ( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal );

void draw_push_rect_outline     ( f32 x, f32 y, f32 w, f32 h, f32 t, u32 tex_idx, u32 abgr );
void draw_push_triangle         ( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 tex_idx, u32 abgr );
void draw_push_circle_filled    ( f32 cx, f32 cy, f32 r, u32 segments, u32 abgr );
void draw_push_text             ( f32 x, f32 y, u32 abgr, const char* str );
void draw_push_text_n           ( f32 x, f32 y, u32 abgr, const char* str, u32 n );

/*==============================================================================================
    GPU resources + flush (imgui_render.c)
==============================================================================================*/

bool imgui_render_init    ( void );
void imgui_render_shutdown( void );
void imgui_render_flush   ( imgui_viewport_t* vp, u32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h );

/* Drop the once-per-frame tessellation cache so the next flush rebuilds the shared geometry.
   The frame's semantic list is tessellated + run-sorted exactly once (lazily, on the first
   surface flush); every other live surface that frame reuses the result.  Called by
   imgui_frame_begin right after draw_reset, before the build emits any new commands. */
void imgui_render_frame_reset( void );

imgui_mem_stats_t imgui_render_memory      ( void );
void              imgui_render_print_memory( void );

/* Per-frame render stats: imgui_render_stats returns the last published frame's totals;
   imgui_render_stats_publish promotes the in-progress accumulator to the published value and
   resets it -- called once per frame by imgui_frame_begin (the UI unit), before draw_reset. */
imgui_render_stats_t imgui_render_stats        ( void );
void                 imgui_render_stats_publish( void );

/* Debug render mode (normal / wireframe / batch-tint) -- backs imgui()->debug_set/get_render_mode.
   The flush reads it to pick the fill vs. wireframe pipeline and the per-draw debug push constants. */
void                imgui_render_set_mode( imgui_render_mode_t mode );
imgui_render_mode_t imgui_render_get_mode( void );

bool viewport_create ( imgui_viewport_t* vp, rhi_texture_t target, i32 win_id ); // a surface's vb/ib
void viewport_destroy( imgui_viewport_t* vp );                                   // free its vb/ib

/*==============================================================================================
    Debug overlay (imgui_debug.c) -- Debug builds only.

    A second draw list, captured from the UI via the DBG_* macros and flushed last, on top.  The
    build switch is computed here so BOTH units agree before the macros / capture decls are used:
    the build tool defines IMGUI_DEBUG_OVERLAY for the Debug config; the MSVC _DEBUG macro is a
    fallback so the feature tracks the configuration even before a build_tool regen.  Define
    IMGUI_NO_DEBUG_OVERLAY to force it off.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( IMGUI_DEBUG_OVERLAY ) && !defined( IMGUI_NO_DEBUG_OVERLAY )
    #define IMGUI_DEBUG_OVERLAY
#endif
#if defined( IMGUI_NO_DEBUG_OVERLAY ) && defined( IMGUI_DEBUG_OVERLAY )
    #undef IMGUI_DEBUG_OVERLAY
#endif

#ifdef IMGUI_DEBUG_OVERLAY

    /* Lifecycle, driven by imgui_frame.c (UI unit) under the same #ifdef. */
    bool imgui_debug_init    ( void );
    void imgui_debug_shutdown( void );
    void imgui_debug_reset   ( void );
    void imgui_debug_flush   ( imgui_vp_t vp, rhi_cmd_t cmd, i32 win_w, i32 win_h );

    /* Capture entry points -- called from both units via the DBG_* macros below.  Each tags its
       command with the ambient build viewport (imgui_dbg_build_viewport, imgui_ctx.c). */
    void dbg_capture_widget( imgui_id_t id, imgui_rect_t r, bool hover, bool active );
    void dbg_capture_clip  ( imgui_rect_t r, u32 depth );
    void dbg_capture_window( imgui_rect_t r, bool is_hover );
    void dbg_capture_resize( imgui_rect_t band, u8 hot_edges );

    #define DBG_WIDGET( id, r, hov, act ) dbg_capture_widget( ( id ), ( r ), ( hov ), ( act ) )
    #define DBG_CLIP( r, depth )          dbg_capture_clip( ( r ), ( depth ) )
    #define DBG_WINDOW( r, is_hover )     dbg_capture_window( ( r ), ( is_hover ) )
    #define DBG_RESIZE( band, hot )       dbg_capture_resize( ( band ), ( hot ) )

    /* Ambient build viewport (s_build.cur_viewport, imgui_ctx.c) -- the capture functions live in/usage
       the backend unit, so they read it through this accessor rather than the UI-unit static. */
    u32 imgui_dbg_build_viewport( void );

#else
    #define DBG_WIDGET( id, r, hov, act ) ( (void)0 )
    #define DBG_CLIP( r, depth )          ( (void)0 )
    #define DBG_WINDOW( r, is_hover )     ( (void)0 )
    #define DBG_RESIZE( band, hot )       ( (void)0 )
#endif

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_BACKEND_H
