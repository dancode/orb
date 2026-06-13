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

    /* Layout -- shape the active region's repeating row template.  A region opens as a single
       flex column of auto height (the classic vertical stack); these replace that template, and
       it persists + repeats for every widget until set again.  Sizes use one overloaded f32:
       >1 px, (0,1] fraction of the available space, 0 flex (equal share of the rest), <0 ends
       the list (IMGUI_END).  Widgets fill whatever cell they are handed, agnostic to the shape.

           imgui()->row_cols( 0, 2 );  imgui()->button("A");  imgui()->button("B");  // two columns
           imgui()->row_track( 24, (f32[]){ 200, 0, IMGUI_END } );                    // 200px + fill

       layout()     -- full flow template (columns, row height, item padding, gaps) in one struct.
       layout_default() -- clear back to the open default (one flex column, no field split); the
                         single "reset everything" verb.  Padding is untouched (use pad()).
       row()        -- single full-width column of height row_h (0 = auto).
       row_cols()   -- n equal columns of height row_h.
       row2/3/4()   -- fixed-arity weighted columns (auto height): row2( 0.3f, 0.7f ).
       row_track()  -- explicit per-column widths (IMGUI_END-terminated).
       field_split()  -- labeled widgets split their cell into a label + control track (overloaded
                         units, label left or right); input_text / slider_float / checkbox then lay
                         out as an aligned "Label  [control]" form from a single call.
       field_label_left() / field_label_right() -- field_split sugar: a fixed-width label column on
                         the left / right with a flex control filling the rest (0 = off).
       pad()        -- region padding: the inset between the region box and the layout start.

       Grid mode -- cols x rows partition a bounded box (the region content from the pen to its
       bottom) into a fixed matrix, both axes resolved up front; widgets fill cells row-major and
       nothing scrolls.  For titlebars, toolbars, split panes (cell -> begin_child), dashboards,
       image grids.  grid() takes the full descriptor (cols + rows); grid_cells() is the uniform
       nc x nr case.

           imgui()->grid_cells( 3, 2 );  for (i<6) imgui()->button(name[i]);  // 3x2 of buttons
       grid()       -- cols x rows from the descriptor (row_h ignored; grid uses rows). */

    void ( *layout         )( imgui_layout_t desc );
    void ( *layout_default )( void );
    void ( *row            )( f32 row_h );
    void ( *row_cols     )( f32 row_h, u32 n );
    void ( *row2         )( f32 a, f32 b );
    void ( *row3         )( f32 a, f32 b, f32 c );
    void ( *row4         )( f32 a, f32 b, f32 c, f32 d );
    void ( *row_track    )( f32 row_h, const f32* cols );
    void ( *field_split  )( imgui_label_side_t side, f32 label, f32 control );
    void ( *field_label_left  )( f32 width );
    void ( *field_label_right )( f32 width );
    void ( *pad          )( imgui_pad_t region_pad );
    void ( *grid       )( imgui_layout_t desc );
    void ( *grid_cells )( u32 ncols, u32 nrows );

    /* Layout metrics -- theme-derived sizes for pre-computing fixed row / column dimensions.
       line_h / text_w are the raw font metrics; h_min / w_min are the standard margin a row /
       cell adds around its content (the "size without content"); calc_row / calc_col add that
       margin to a content pixel size, giving a fixed dimension that fits content plus margin:

           imgui()->row( imgui()->calc_row( 128 ) );             // a row sized for a 128px image
           f32 w = imgui()->calc_col( imgui()->text_w("Name") ); // a column sized to a label */

    f32 ( *line_h   )( void );
    f32 ( *text_w   )( const char* s );
    f32 ( *h_min    )( void );
    f32 ( *w_min    )( void );
    f32 ( *calc_row )( f32 content_h );
    f32 ( *calc_col )( f32 content_w );

    /* Id scope -- disambiguate widgets that would otherwise share an id.  Widget ids are already
       seeded by the enclosing window / child region automatically, so identical labels in
       different regions never collide; push_id adds a temporary scope level for repeated widgets
       within one region (e.g. rows in a list keyed by index).  Always pair with pop_id.

           for ( i = 0; i < n; ++i ) {
               imgui()->push_id_int( i );
               imgui()->selectable( name[i], &sel[i] );   // distinct id even if name[] repeats
               imgui()->pop_id();
           }

       The "##" / "###" label suffixes are the per-call alternative: "Text##key" displays "Text"
       but ids from the whole string; "pre###key" ids only from "###key", so a changing visible
       prefix (a counter) keeps a stable id. */

    void ( *push_id     )( const char* str );
    void ( *push_id_int )( i32 i );
    void ( *pop_id      )( void );

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
