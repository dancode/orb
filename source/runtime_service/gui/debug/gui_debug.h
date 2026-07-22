#ifndef GUI_DEBUG_INTERNAL_H
#define GUI_DEBUG_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/debug/gui_debug.h -- server introspection (the debug unit).

    The pipeline dashboard + command stepper: ordinary debug-band windows over the render
    server's capture snapshots.  Emitted by debug_overlays_emit (gui_frame_overlay.c, the
    frame unit).  Severable: a build without the debug unit stubs these three.
    Unit root: gui_debug.c, at the gui root since R10 like every unit.

==============================================================================================*/

// clang-format off

void gui_pipeline_dashboard( bool* open );      /* debug unit: F10 dashboard (stub w/o feature) */
void gui_step_window       ( bool* open );      /* debug unit: F8 command stepper window        */
u32  gui_debug_unit_mem_bytes( void );          /* debug unit: its fixed statics, for mem stats */

/*==============================================================================================
    DEBUG OVERLAY capture seams (gui_debug_overlay.c, render unit) -- Debug builds only.

    A second draw list, captured from every unit via the DBG_* macros and flushed last, on top.
    Moved here from render/gui_render.h at the R4 carve: the capture macros are cross-server
    debug tooling (the interact server's item protocol stamps DBG_WIDGET), and the servers never
    include each other's headers -- this header reaches everyone through the umbrella, exactly
    the severable-introspection role the debug unit owns.  The implementation stays in the
    render unit (it batches into GPU buffers).

    The build switch is computed here so EVERY unit agrees before the macros / capture decls
    are used: the build tool defines GUI_DEBUG_OVERLAY for the Debug config; the MSVC _DEBUG
    macro is a fallback so the feature tracks the configuration even before a build_tool regen.
    Define GUI_NO_DEBUG_OVERLAY to force it off.
==============================================================================================*/

#if defined( _DEBUG ) && !defined( GUI_DEBUG_OVERLAY ) && !defined( GUI_NO_DEBUG_OVERLAY )
    #define GUI_DEBUG_OVERLAY
#endif
#if defined( GUI_NO_DEBUG_OVERLAY ) && defined( GUI_DEBUG_OVERLAY )
    #undef GUI_DEBUG_OVERLAY
#endif

#ifdef GUI_DEBUG_OVERLAY

    /* Lifecycle, driven by gui_frame.c (frame unit) under the same #ifdef. */
    bool gui_debug_init    ( void );
    void gui_debug_shutdown( void );
    void gui_debug_reset   ( void );
    void gui_debug_flush   ( gui_vp_t vp, rhi_cmd_t cmd, i32 win_w, i32 win_h );

    /* Capture entry points -- called from every unit via the DBG_* macros below.  Each tags its
       command with the ambient build viewport (gui_dbg_build_viewport, core/gui_ctx.c). */
    void dbg_capture_widget( gui_id_t id, gui_rect_t r, bool hover, bool active );
    void dbg_capture_clip  ( gui_rect_t r, u32 depth );
    void dbg_capture_window( gui_rect_t r, bool is_hover );
    void dbg_capture_resize( gui_rect_t band, u8 hot_edges );
    void dbg_capture_layout( gui_rect_t r );
    void dbg_capture_region( gui_rect_t view, gui_rect_t hit_clip, f32 sb_w, f32 sb_h );

    /* Name registry -- records the source string behind an id as it is minted (widget label,
       window/popup title, region/child/table id string), so gui_state_overlay() can show a
       readable name instead of a hash.  See gui_debug_name() in gui_host.h for the reader. */
    void dbg_name_register( gui_id_t id, const char* str );

    #define DBG_WIDGET( id, r, hov, act ) dbg_capture_widget( ( id ), ( r ), ( hov ), ( act ) )
    #define DBG_CLIP( r, depth )          dbg_capture_clip( ( r ), ( depth ) )
    #define DBG_WINDOW( r, is_hover )     dbg_capture_window( ( r ), ( is_hover ) )
    #define DBG_RESIZE( band, hot )       dbg_capture_resize( ( band ), ( hot ) )
    #define DBG_LAYOUT( r )               dbg_capture_layout( ( r ) )
    #define DBG_REGION( view, hit, sw, sh ) dbg_capture_region( ( view ), ( hit ), ( sw ), ( sh ) )
    #define DBG_NAME( id, str )           dbg_name_register( ( id ), ( str ) )

    /* Ambient build viewport (s_build.win.viewport, core/gui_ctx.c) -- the capture functions
       live in the render unit, so they read it through this accessor rather than the static. */
    u32 gui_dbg_build_viewport( void );

#else
    #define DBG_WIDGET( id, r, hov, act ) ( (void)0 )
    #define DBG_CLIP( r, depth )          ( (void)0 )
    #define DBG_WINDOW( r, is_hover )     ( (void)0 )
    #define DBG_RESIZE( band, hot )       ( (void)0 )
    #define DBG_LAYOUT( r )               ( (void)0 )
    #define DBG_REGION( view, hit, sw, sh ) ( (void)0 )
    #define DBG_NAME( id, str )           ( (void)0 )
#endif

/*==============================================================================================
    Command-stepper attribution seam -- the one stepper hook the interact server touches.

    The GUI_CMD_STEPPER switch is computed here (same rule as the overlay above) so every unit
    agrees; the capture/replay mechanism and the rest of the STEP_* hooks stay render-side
    (render/gui_render.h).  item_state (core/gui_item.c) marks each registering widget so the
    commands it paints carry its id -- STEP_SET_OWNER compiles away with the feature.  Window
    transitions and draw_reset clear it back to 0 (chrome).
==============================================================================================*/

#if defined( _DEBUG ) && !defined( GUI_CMD_STEPPER ) && !defined( GUI_NO_CMD_STEPPER )
    #define GUI_CMD_STEPPER
#endif
#if defined( GUI_NO_CMD_STEPPER ) && defined( GUI_CMD_STEPPER )
    #undef GUI_CMD_STEPPER
#endif

#ifdef GUI_CMD_STEPPER
    void draw_set_cmd_owner( gui_id_t id );   /* defined in gui_emit_draw.c (render unit) */
    #define STEP_SET_OWNER( id )      draw_set_cmd_owner( id )
#else
    #define STEP_SET_OWNER( id )      ( (void)0 )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GUI_DEBUG_INTERNAL_H
