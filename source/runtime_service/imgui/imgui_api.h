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
       x, y are the panel's top-left pixel position; w, h are its dimensions. */

    void ( *begin_window )( const char* title, f32 x, f32 y, f32 w, f32 h );
    void ( *end_window   )( void );

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched begin_window / end_window pair. */

    void ( *text        )( const char* str );
    bool ( *button      )( const char* label );
    bool ( *checkbox    )( const char* label, bool* v );
    bool ( *slider_float)( const char* label, f32* v, f32 lo, f32 hi );
    bool ( *input_text  )( const char* label, char* buf, u32 bufsz );

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
