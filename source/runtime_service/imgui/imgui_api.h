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

    /* set_next_window_pos / _size -- queue geometry for the NEXT begin_window, applied per the
       condition (imgui_cond_t) and then cleared.  Decouples the value from when it is applied:
       ONCE seeds an initial position/size (the same effect as begin_window's x/y/w/h), ALWAYS
       forces it every frame (layout managers, snapping, animation -- pair with NOMOVE / NORESIZE),
       APPEARING re-applies it each time the window is shown after being absent.  Call immediately
       before begin_window; the throwaway x/y/w/h there can stay (ONCE) or be ignored (ALWAYS). */
    void ( *set_next_window_pos  )( f32 x, f32 y, imgui_cond_t cond );
    void ( *set_next_window_size )( f32 w, f32 h, imgui_cond_t cond );

    bool ( *begin_window )( const char* title, f32 x, f32 y, f32 w, f32 h, imgui_win_flags_t flags );
    void ( *end_window   )( void );

    /* Popups -- transient overlay windows on top of everything.  A regular popup auto-closes when
       the user clicks outside it; a modal blocks input behind it and dims the background, closing
       only via close_current_popup.  The string id namespaces both the open request and the body,
       so open_popup("x") and begin_popup("x") must use the same id.  Popups stack (a popup opened
       while inside another nests under it); a click keeps the deepest popup under the cursor and
       closes the rest.  Popup / tooltip bodies lay out like a window body: declare a layout header
       (stack / columns / ...) before emitting widgets.

           if ( imgui()->button( "Open" ) )    imgui()->open_popup( "menu" );
           if ( imgui()->begin_popup( "menu", IMGUI_WIN_NONE ) ) {
               imgui()->stack();
               if ( imgui()->selectable( "Cut",  NULL ) ) { ... }
               if ( imgui()->selectable( "Copy", NULL ) ) { ... }
               imgui()->end_popup();
           }

       begin_popup / begin_popup_modal return true only when the popup is open AND visible -- guard
       the body and call end_popup only on a true return (like begin_window's collapsed contract).
       Auto-sized popups (the default) measure their content on the appearing frame off-screen and
       snap into place the next frame, so there is no first-frame size pop. */

    void ( *open_popup          )( const char* id );
    bool ( *begin_popup         )( const char* id, imgui_win_flags_t flags );
    bool ( *begin_popup_modal   )( const char* id, const char* title, imgui_win_flags_t flags );
    void ( *end_popup           )( void );
    void ( *close_current_popup )( void );
    bool ( *is_popup_open        )( const char* id );

    /* Context menus -- open a popup on a right-click.  _item binds to the previous widget (the one
       emitted just before the call); _window binds to empty space in the current window.  Use them
       in place of the open_popup + begin_popup pair:

           imgui()->selectable( "Row", NULL );
           if ( imgui()->begin_popup_context_item( "row_ctx" ) ) { ...; imgui()->end_popup(); } */

    bool ( *begin_popup_context_item   )( const char* id );
    bool ( *begin_popup_context_window )( const char* id );

    /* Tooltips -- a non-interactive overlay shown at the cursor while the previous widget is
       hovered.  set_item_tooltip is the one-liner; begin_tooltip / end_tooltip wrap a multi-widget
       body (guard the body on the true return, always call end_tooltip).

           imgui()->button( "Hover me" );
           imgui()->set_item_tooltip( "Does the thing" ); */

    void ( *set_item_tooltip )( const char* text );
    bool ( *begin_tooltip    )( void );
    void ( *end_tooltip      )( void );

    /* Child regions -- a nested scrollable layout box inside the current window (or another
       child).  begin_child carves a box of height h (width w, or the remaining content width
       when w <= 0) from the layout pen, clips and scrolls its contents independently, and
       gives it its own scrollbar; flags take the IMGUI_WIN_*SCROLL policy bits.  Always pair
       with end_child -- the parent layout resumes directly below the box.  Fill it with any
       widgets (e.g. selectable rows for a list box).  begin_child always returns true. */

    bool ( *begin_child )( const char* id, f32 w, f32 h, imgui_win_flags_t flags );

    /* Sub-layout -- carve the next cell into its own little layout, the way a window or child hosts
       one, but transient: no scroll, no clip, no persistent state, no frame.  push_layout consumes
       one cell (advancing the parent like any widget), opens a layout filling it (default single
       column; shape it with row / grid / widgets inside), and pop_layout closes it -- the parent
       resumes at the following cell.  The cell is one standard line tall unless the row height was
       declared larger first; the sub-layout does not grow the parent to fit, and does not clip.
       Always pair, like push_id / pop_id.

           imgui()->row_cols( 0, 3 );                       // 3 columns
           imgui()->push_layout();                          // column 0 becomes a sub-layout...
               imgui()->button("A"); imgui()->button("B");  // ...stacked inside that one cell
           imgui()->pop_layout();
           imgui()->text("col 1");  imgui()->text("col 2"); */

    void ( *push_layout )( void );
    void ( *pop_layout  )( void );
    void ( *end_child   )( void );

    /* Layout -- declare the active region's next-item methodology (its "mode"), then shape it.
       A region opens UNDECLARED: the first header below names the mode (stack / columns / grid /
       form / ...), and a widget emitted before any header is a usage error (debug assert; release
       falls back to a stack).  The template then persists + repeats for every widget until set
       again.  Sizes use one overloaded f32: >1 px, (0,1] fraction of the available space, 0 flex
       (equal share of the rest), <0 ends the list (IMGUI_END).  Widgets fill whatever cell they
       are handed, agnostic to the shape.

           imgui()->row_cols( 0, 2 );  imgui()->button("A");  imgui()->button("B");  // two columns
           imgui()->row_track( 24, (f32[]){ 200, 0, IMGUI_END } );                    // 200px + fill

       stack()      -- single full-width flex column, scrolling: the canonical vertical-list header
                         (what a region used to be by default; now declared explicitly).
       columns()    -- N explicit column tracks (IMGUI_END-terminated), auto height, scrolling.
       cols_n()     -- n equal flex columns, auto height.
       form()       -- a stack with a fixed-width label track on `side`: the "Label  [control]"
                         form header (label_w <= 0 = plain stack).  form_split() = field_split.
       layout()     -- full flow template (columns, row height, item padding, gaps) in one struct.
       layout_default() -- clear back to a plain stack (one flex column, no field split); the
                         single "reset everything" verb.  Padding is untouched (use pad()).
       row()        -- a stack with an explicit row height (0 = auto).
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
       nothing scrolls.  For titlebars, split panes (cell -> begin_child), dashboards, image grids.
       grid() takes the full descriptor (cols + rows); grid_cells() is the uniform nc x nr case.

           imgui()->grid_cells( 3, 2 );  for (i<6) imgui()->button(name[i]);  // 3x2 of buttons
       grid()       -- cols x rows from the descriptor (row_h ignored; grid uses rows).

       Pack mode -- the print run: place items one after another along an axis at natural size, the
       widget sizing itself (vs columns/grid, where the cell sizes the widget).  pack_size() overrides
       the next item's main-axis measure (resolved against the space left on the line); pack_nextline()
       breaks to a fresh line.  The toolbar / tag-row / inline-controls case.

           imgui()->bar();  imgui()->button("Save");  imgui()->button("Open");   // a toolbar
       pack()       -- open a run along dir (HORIZONTAL / VERTICAL).
       bar() / strip() -- pack sugar: horizontal (toolbar) / vertical run.
       pack_size()  -- next packed item's main-axis size (0 natural, 1 fill, (0,1) frac, >1 px).
       pack_nextline() -- break the run to a new line. */

    void ( *layout         )( imgui_layout_t desc );
    void ( *layout_default )( void );
    void ( *stack          )( void );
    void ( *row            )( f32 row_h );
    void ( *columns      )( const f32* tracks );
    void ( *cols_n       )( u32 n );
    void ( *row_cols     )( f32 row_h, u32 n );
    void ( *row2         )( f32 a, f32 b );
    void ( *row3         )( f32 a, f32 b, f32 c );
    void ( *row4         )( f32 a, f32 b, f32 c, f32 d );
    void ( *row_track    )( f32 row_h, const f32* cols );
    void ( *form         )( imgui_label_side_t side, f32 label_w );
    void ( *form_split   )( imgui_label_side_t side, f32 label, f32 control );
    void ( *field_split  )( imgui_label_side_t side, f32 label, f32 control );
    void ( *field_label_left  )( f32 width );
    void ( *field_label_right )( f32 width );
    void ( *pad          )( imgui_pad_t region_pad );
    void ( *grid       )( imgui_layout_t desc );
    void ( *grid_cells )( u32 ncols, u32 nrows );
    void ( *pack          )( imgui_pack_dir_t dir );
    void ( *bar           )( void );
    void ( *strip         )( void );
    void ( *pack_size     )( f32 unit );
    void ( *pack_nextline )( void );

    /* align() -- set the content alignment within each cell (imgui_align_t, LEFT | TOP by default).
       Persists like the row template and is independent of the columns: row() / row_cols() leave it
       untouched, layout_default() clears it.  Governs where natural-sized content sits (a text run, a
       checkbox box, a button's label); a frame-filling widget still fills its cell.  The `align`
       field of layout() / grid() sets the same thing as part of a full descriptor.

           imgui()->row2( 0.5f, 0.5f );  imgui()->align( IMGUI_ALIGN_RIGHT );   // right-aligned columns

       same_line() -- keep the next widget on the line just emitted instead of breaking to a new
                      row; it takes its natural width.  `spacing` is the gap in pixels (0 = flush,
                      < 0 = the theme default).  Mirrors ImGui::SameLine.

           imgui()->button("OK");  imgui()->same_line( 0.0f );  imgui()->button("Cancel");

       Spacers -- cell-consuming composition that emits nothing interactive:
       skip()      -- leave one blank cell (a hole; the natural way to step over a grid slot).
       spacing()   -- a blank gap of height h (<= 0 = default gap).
       separator() -- a thin horizontal rule centered in its cell. */

    void ( *align      )( imgui_align_t a );
    void ( *same_line  )( f32 spacing );
    void ( *stack_sameline )( f32 spacing );
    void ( *skip       )( void );
    void ( *spacing    )( f32 h );
    void ( *separator  )( void );

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

    /* content_avail() -- remaining free space in the current region from the layout pen: the width
       a flex widget would fill and the height left before the region bottom.  The ImGui
       GetContentRegionAvail analogue -- size a begin_child to the leftover, or lay out by hand. */
    imgui_vec2_t ( *content_avail )( void );

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

    /* Item flags -- the push-model per-item behavior set (imgui_item_flags_t).  push/pop tune every
       widget until popped (and nest); next_item_flag is a one-shot override the very next widget
       consumes, no pop needed.  The mechanism is callsite-free: widgets read the resolved flags at
       emit time, so a new flag never changes a widget signature.  IMGUI_ITEM_DISABLED is honored
       for every widget today (inert + dimmed).

           imgui()->push_item_flag( IMGUI_ITEM_DISABLED, true );
           imgui()->button( "A" );  imgui()->button( "B" );    // both disabled
           imgui()->pop_item_flag();

           imgui()->next_item_flag( IMGUI_ITEM_DISABLED, true );
           imgui()->button( "Only this one" );                 // disabled, no pop needed */

    void ( *push_item_flag )( imgui_item_flags_t flag, bool enable );
    void ( *pop_item_flag  )( void );
    void ( *next_item_flag )( imgui_item_flags_t flag, bool enable );

    /* Style stacks -- the push-model theme override (imgui_col_t colors, imgui_style_var_t metrics).
       push overrides a slot until the matching pop (pop takes a count, like ImGui); next_style_*
       overrides for just the next widget, no pop.  Colors are abgr (IMGUI_COLOR); vars are f32 px.
       Like the item flags, this is callsite-free: every widget already reads the palette + metrics
       through the resolver, so an override reaches them without any widget change.

           imgui()->push_style_color( IMGUI_COL_WIDGET_BG, IMGUI_COLOR( 0xFF, 0, 0, 0xFF ) );
           imgui()->push_style_var( IMGUI_VAR_WIDGET_PAD, 20.0f );
           imgui()->button( "Big Red" );
           imgui()->pop_style_var( 1 );
           imgui()->pop_style_color( 1 ); */

    void ( *push_style_color )( imgui_col_t slot, u32 abgr );
    void ( *pop_style_color  )( u32 count );
    void ( *next_style_color )( imgui_col_t slot, u32 abgr );
    void ( *push_style_var   )( imgui_style_var_t var, f32 value );
    void ( *pop_style_var    )( u32 count );
    void ( *next_style_var   )( imgui_style_var_t var, f32 value );

    /* set_window_drag() -- select how windows may be dragged (global default TITLEBAR).
       Call between frames; affects every window. */
    void ( *set_window_drag )( imgui_win_drag_t mode );

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched begin_window / end_window pair, and only
       when begin_window returned true -- a collapsed window draws no clip, so widgets emitted
       into it render straight onto the screen.  The bool guard is the caller's job. */

    void ( *text        )( const char* str );
    void ( *textf       )( const char* fmt, ... );
    void ( *bullet_text )( const char* str );

    /* label_text -- a read-only "value + label" row that lays out like the labeled value widgets
       (label track / control track under a form or field_split, trailing label otherwise) but is
       pure display.  For information rows that align with the editable widgets around them. */
    void ( *label_text  )( const char* label, const char* value );
    bool ( *button      )( const char* label );

    /* arrow_button -- a square, framed, non-text button drawing a triangle pointing `dir`.  The id
       comes from the label (use a "##id" string, nothing is displayed).  Combine with
       push_item_flag( IMGUI_ITEM_BUTTON_REPEAT, true ) for press-and-hold stepping (spin buttons). */
    bool ( *arrow_button )( const char* id_str, imgui_dir_t dir );

    bool ( *checkbox    )( const char* label, bool* v );

    /* radio_button -- one option of a mutually-exclusive set: shows on while *v == value, a click
       sets *v = value.  Emit several against the same v (same_line between them for a row) to form
       a group; returns true only on the frame a click changes the selection. */
    bool ( *radio_button )( const char* label, i32* v, i32 value );
    bool ( *slider_float)( const char* label, f32* v, f32 lo, f32 hi );
    bool ( *input_text  )( const char* label, char* buf, u32 bufsz );

    /* selectable -- a full-width row that highlights on hover and fills when selected; the
       list-box building block.  A click toggles *selected (pass NULL for click-only); returns
       true on the clicked frame so a caller managing single-selection can set its own index. */
    bool ( *selectable  )( const char* label, bool* selected );

    /* collapsing_header -- a clickable fold bar (arrow + label) that returns its open state; the
       caller guards the section body with the return ( if ( header(...) ) {...} ), so a closed
       header skips its contents.  Open state persists by id; closed by default.
       separator_text   -- a labeled horizontal rule, "-- Text --------". */
    bool ( *collapsing_header )( const char* label );
    void ( *separator_text    )( const char* label );

    /* tree_node / tree_pop -- a collapsing_header without the frame: an arrow + label row that
       folds and indents a nested block while open (file explorers, outline views).  Guard the body
       with the return and, when true, close it with tree_pop, which removes the indent the open
       node added:

           if ( imgui()->tree_node( "Parent" ) )
           {
               imgui()->text( "Child" );
               imgui()->tree_pop();
           }

       indent / unindent -- shift the content column right (or back) by w pixels (w <= 0 = one row
       height) so a block of widgets lays out inset; the mechanism behind tree_node, usable alone.
       Balance every indent with an unindent of the same width.  Flow layouts only. */
    bool ( *tree_node )( const char* label );
    void ( *tree_pop  )( void );
    void ( *indent    )( f32 w );
    void ( *unindent  )( f32 w );

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
