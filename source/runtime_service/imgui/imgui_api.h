#ifndef IMGUI_API_H
#define IMGUI_API_H
/*==============================================================================================

    runtime_service/imgui/imgui_api.h -- imgui module API struct and gateway macro.
    Always statically linked into the host.

    Function groups (all called through imgui() vtable or as imgui_* direct calls):
        Lifecycle : init / shutdown
        Frame     : new_frame / render
        Panels    : begin_window / end_window
        Widgets   : text / button / checkbox / slider_float / input_text
        Draw      : draw_rect / draw_text / push_clip / pop_clip

==============================================================================================*/

#include "runtime_service/imgui/imgui.h"
#include "runtime_service/rhi/rhi.h"    /* rhi_cmd_t for render()    */
#include "engine/app/app.h"             /* app_event_t for event()   */
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct imgui_api_s
{
    /* GPU resource lifecycle.
       init()      -- call after rhi()->init(); creates pipeline, font atlas, GPU buffers.
       shutdown()  -- call before rhi()->shutdown(); destroys all GPU resources.
       load_font() -- load a pre-baked .orb_font atlas; call after init().
                      Returns true on success; falls back to bitmap font on failure. */

    bool ( *init      )( void );
    void ( *shutdown  )( void );
    bool ( *load_font )( const char* path );

    /* GPU resource memory currently held by imgui, in bytes (buffers + atlases).
       print_mem_stats() dumps the same breakdown to stdout. */
    imgui_mem_stats_t ( *mem_stats       )( void );
    void              ( *print_mem_stats )( void );

    /* Frame lifecycle.
       new_frame() -- reset draw list and translate app input into the IO snapshot.
                      Call once at the top of the frame, before any widget calls.
       render()    -- flush the draw list to GPU; opens a LOAD render pass on the
                      swapchain, emits all draw calls, and closes the pass.
                      Call after all widget calls, before rhi()->frame_end(). */

    void ( *new_frame )( i32 win_w, i32 win_h, f32 dt );
    void ( *render    )( rhi_cmd_t cmd, i32 win_w, i32 win_h );

    /* Host input -- the host owns the app event ring drain and forwards each
       event here before new_frame() for the same frame.
       event() -- forward one drained app_event_t; imgui unpacks the input events
                  it cares about (text + scroll) and returns true if it consumed
                  the event, letting the host skip its own handling for it. */

    bool ( *event )( const app_event_t* ev );

    /* Panels -- open a window panel; must be matched with end_window().
       x, y are the panel's top-left pixel position; w, h are its dimensions.
       flags is a bitmask of imgui_win_flags_t (0 / IMGUI_WIN_NONE for the defaults) that
       switches off built-in behavior per window -- title bar, collapse, or edge resize.

       begin_window() returns false when the window is collapsed (title bar only).  Guard
       the body widgets with it -- skipped widgets cost nothing -- but always call
       end_window() regardless of the return value:

           if ( imgui()->begin_window( "Tools", 10, 10, 240, 320, IMGUI_WIN_NONE ) )
           {
               imgui()->text( "..." );          // skipped while collapsed
           }
           imgui()->end_window();               // always called */

    bool ( *begin_window )( const char* title, f32 x, f32 y, f32 w, f32 h, imgui_win_flags_t flags );
    void ( *end_window   )( void );

    /* Child regions -- a nested scrollable layout box inside the current window (or another
       child).  begin_child carves a box of height h (width w, or the remaining content width
       when w <= 0) from the layout pen, clips and scrolls its contents independently, and
       gives it its own scrollbar; flags take the IMGUI_WIN_*SCROLL policy bits.  Always pair
       with end_child -- the parent layout resumes directly below the box.  Fill it with any
       widgets (e.g. selectable rows for a list box).  begin_child always returns true. */

    bool ( *begin_child )( const char* id, f32 w, f32 h, imgui_win_flags_t flags );
    void ( *end_child   )( void );

    /* set_window_drag() -- select how windows may be dragged (global default TITLEBAR).
       Call between frames; affects every window. */
    void ( *set_window_drag )( imgui_win_drag_t mode );

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched begin_window / end_window pair, and only
       when begin_window returned true -- a collapsed window draws no clip, so widgets emitted
       into it render straight onto the screen.  The bool guard is the caller's job. */

    void ( *text        )( const char* str );
    void ( *textf       )( const char* fmt, ... );
    bool ( *button      )( const char* label );
    bool ( *checkbox    )( const char* label, bool* v );
    bool ( *slider_float)( const char* label, f32* v, f32 lo, f32 hi );
    bool ( *input_text  )( const char* label, char* buf, u32 bufsz );

    /* selectable -- a full-width row that highlights on hover and fills when selected; the
       list-box building block.  A click toggles *selected (pass NULL for click-only); returns
       true on the clicked frame so a caller managing single-selection can set its own index. */
    bool ( *selectable  )( const char* label, bool* selected );

    /* Font -- select the active font; call between frames (outside new_frame / render).
       set_font()      -- select a built-in bitmap font; also unloads any active TrueType font.
                         Widget layout dimensions are recomputed from the new font's char_h.
       set_bmp_scale() -- integer pixel-scale multiplier for bitmap fonts (1 = native, 2 = 2x, ...).
                         Has no effect on TrueType fonts.  Recomputes layout immediately. */

    void ( *set_font      )( imgui_font_t font );
    void ( *set_bmp_scale )( u32 scale );

    /* Low-level draw list access -- may be called anywhere between new_frame and render.
       draw_rect and draw_text push geometry directly into the draw list.
       push_clip / pop_clip set the current scissor rectangle. */

    void ( *draw_rect )( f32 x, f32 y, f32 w, f32 h, u32 abgr );
    void ( *draw_text )( f32 x, f32 y, u32 abgr, const char* str );
    void ( *push_clip )( f32 x, f32 y, f32 w, f32 h );
    void ( *pop_clip  )( void );

    /* Debug overlay -- a separate draw list painted last, on top of the UI.  Pass a bitmask
       of imgui_dbg_layer_t to debug_set_layers() to choose which visualizations show; pass
       IMGUI_DBG_NONE (0) to turn it off.  Compiled in for Debug builds only: in Release,
       set_layers is a no-op and get_layers returns 0.  The two slots stay in the vtable in
       every build so func_api_size is identical across a hot-reload. */

    void ( *debug_set_layers )( u32 layers );
    u32  ( *debug_get_layers )( void );

} imgui_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( IMGUI_STATIC )
    MOD_GATEWAY_STATIC( imgui_api_t, imgui )
#else
    MOD_GATEWAY_DYNAMIC( imgui_api_t, imgui )
#endif

#if defined( BUILD_STATIC ) || defined( IMGUI_STATIC )
    #define MOD_USE_IMGUI    /* static build */
    #define MOD_FETCH_IMGUI  true
#else
    #define MOD_USE_IMGUI    MOD_DEFINE_API_PTR( imgui_api_t, imgui )
    #define MOD_FETCH_IMGUI  MOD_FETCH_API( imgui_api_t, imgui )
#endif

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_API_H
