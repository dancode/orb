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
                      Surface dimensions are owned by each viewport (set at open and on resize);
                      no size argument is needed here.
       render()    -- flush one viewport's geometry partition to GPU; opens a LOAD render pass on
                      that viewport's swapchain, emits all draw calls, and closes the pass.  Also
                      paints the debug overlay when vp is the primary (index 0).
                      Call once per live viewport, each with the matching context cmd. */

    void ( *new_frame )( f32 dt );
    void ( *render    )( imgui_vp_t vp, rhi_cmd_t cmd );

    /* Viewport management.  A viewport is a render surface backed by an OS window.  One new_frame()
       builds every window's geometry into a single draw list; render() dispatches each window's
       partition to the viewport it is assigned to (set_next_window_viewport, or inherited from
       whichever viewport was most recently emitted into this frame).

       viewport_open()   -- open a surface for OS window win_id with initial drawable size w x h.
                            Returns a handle (imgui_vp_t >= 0) or IMGUI_VP_INVALID if the pool is full.
                            The first call creates the primary (index 0); call before any frames.
                            win_id routes mouse events from that OS window to this surface.
       viewport_close()  -- close a non-primary viewport and release its GPU geometry buffers.
                            Windows on the closed viewport automatically fall back to the primary.
                            The host owns the OS window and rhi context; imgui owns only the geometry.
       viewport_resize() -- update a viewport's drawable size.  Call on OS resize BEFORE new_frame.
                            Works identically for primary and secondary viewports. */

    imgui_vp_t ( *viewport_open   )( i32 win_id, i32 w, i32 h );
    void       ( *viewport_close  )( imgui_vp_t vp );
    void       ( *viewport_resize )( imgui_vp_t vp, i32 w, i32 h );

    /* imgui-OWNED floater surfaces.  Where viewport_open hands imgui a host-created window+context
       to flush into, these own the OS window + rhi context end to end -- imgui creates them on
       spawn and tears them down on close.  This is the lifecycle the tear-off gesture will drive;
       a host may also call viewport_spawn directly to place a panel in its own OS window.

       viewport_spawn()          -- open a floater hosting its own OS window at (x,y) sized w x h;
                                    returns its viewport handle (assign windows via
                                    set_next_window_viewport) or IMGUI_VP_INVALID.  Between frames.
       update_platform_windows() -- reconcile owned floaters with their OS windows; call once per
                                    frame AFTER the UI build and BEFORE rendering (the safe point to
                                    tear a surface down -- destroys those the user closed).
       render_floaters()         -- present every owned floater from the shared draw list, each on
                                    its own rhi context (frame_begin/clear/flush/frame_end).  The
                                    host still presents the main surface (index 0) via render(). */

    imgui_vp_t ( *viewport_spawn          )( const char* title, i32 x, i32 y, i32 w, i32 h );
    void       ( *update_platform_windows )( void );
    void       ( *render_floaters         )( void );

    /* Host input -- the host owns the app event ring drain and forwards each
       event here before new_frame() for the same frame.
       event() -- forward one drained app_event_t; imgui unpacks the input events
                  it cares about (text + scroll) and returns true if it consumed
                  the event, letting the host skip its own handling for it. */

    bool ( *event )( const app_event_t* ev );

    /* Panels -- open a window panel; must be matched with end_window().
       flags is a bitmask of imgui_win_flags_t (0 / IMGUI_WIN_NONE for the defaults) that
       switches off built-in behavior per window -- title bar, collapse, or edge resize.

       begin_window() returns false when the window is collapsed (title bar only).  Guard
       the body widgets with it -- skipped widgets cost nothing -- but always call
       end_window() regardless of the return value:

           if ( imgui()->begin_window( "Tools", IMGUI_WIN_NONE ) )
           {
               imgui()->text( "..." );          // skipped while collapsed
           }
           imgui()->end_window();               // always called */

    /* set_next_window_pos / _size -- queue geometry for the NEXT begin_window, applied per the
       condition (imgui_cond_t) and then cleared.  Decouples the value from when it is applied:
       ONCE seeds an initial position/size (apply once on first appearance, then user-owned),
       ALWAYS forces it every frame (layout managers, snapping, animation -- pair with NOMOVE /
       NORESIZE), APPEARING re-applies it each time the window is shown after being absent.
       Call immediately before begin_window. */
    void ( *set_next_window_pos  )( f32 x, f32 y, imgui_cond_t cond );
    void ( *set_next_window_size )( f32 w, f32 h, imgui_cond_t cond );

    /* set_next_window_viewport -- assign the NEXT begin_window to a specific viewport.  Sticky: it
       lands on the window record and persists across frames until reassigned.  Omit to inherit the
       ambient viewport -- the one most recently emitted into this frame -- so windows created from
       within a viewport's panels naturally land on the same surface without explicit assignment.
       If the assigned viewport is later closed, the window automatically reverts to the primary. */
    void ( *set_next_window_viewport )( imgui_vp_t vp );

    /* set_next_window_size_constraints -- queue a one-shot [min,max] size box for the NEXT
       begin_child, then cleared.  The Dear ImGui SetNextWindowSizeConstraints analogue, in its
       most useful form: it bounds the child's resolved width / height, so an auto-sized (h <= 0)
       box grows with its content up to max_h and then scrolls, never collapses below min_h, and a
       CHILD_RESIZE_* drag cannot leave the range.  A bound <= 0 is "unconstrained" on that side
       (e.g. 0, 0, 0, max_h to cap height only).  Call immediately before begin_child. */
    void ( *set_next_window_size_constraints )( f32 min_w, f32 min_h, f32 max_w, f32 max_h );

    bool ( *begin_window )( const char* title, imgui_win_flags_t flags );
    void ( *end_window   )( void );

    /* Docking -- tile + tab windows into a dock tree that fills a viewport (the DockSpaceOverViewport
       analogue).  Phase 1 is programmatic: build a layout in code, then windows whose titles were
       dock_window'd render into their node (no per-window title bar -- the node draws a shared tab
       strip) instead of free-floating.  Free-floating windows still overlap on top of the dockspace.

       dockspace_over_viewport() -- ensure viewport vp hosts a dock tree, lay it out over the surface,
                                    draw + interact its splitters, and return the tree ROOT node id.
                                    Call once per frame at the TOP of the build, before the docked
                                    windows' begin_window (which read their resolved node rects).
       dock_split()              -- split a LEAF node in two; returns the NEW empty leaf on the `dir`
                                    side and writes the REMAINING node id to *out_remain (may be NULL).
                                    `ratio` is the new side's fraction of the axis.  The DockBuilder
                                    idiom -- keep splitting the returned remainder to carve a layout.
       dock_window()             -- add a window (matched to begin_window by title) as a tab in a leaf,
                                    moving it out of any node it was already in; it becomes active.
       dock_undock()             -- remove a window from its node, returning it to free-floating.
       is_window_docked()        -- true while the window is tabbed into some node.

           imgui_dock_id_t root  = imgui()->dockspace_over_viewport( 0, IMGUI_DOCKSPACE_NONE );
           imgui_dock_id_t left  = imgui()->dock_split( root, IMGUI_DIR_LEFT, 0.25f, &root );
           imgui()->dock_window( "Scene Tree", left );
           imgui()->dock_window( "Viewport",   root );   // center; tab more windows here with root */

    imgui_dock_id_t ( *dockspace_over_viewport )( imgui_vp_t vp, imgui_dockspace_flags_t flags );
    imgui_dock_id_t ( *dock_split )( imgui_dock_id_t node, imgui_dir_t dir, f32 ratio,
                                     imgui_dock_id_t* out_remain );
    void ( *dock_window )( const char* title, imgui_dock_id_t node );
    void ( *dock_undock )( const char* title );
    bool ( *is_window_docked )( const char* title );

    /* Layout persistence.  dock_save() serializes viewport vp's dock tree into buf as a small ASCII
       blob and returns the byte count a full write needs (like snprintf -- pass a 0 bufsz to size
       first).  dock_load() rebuilds the tree from such a blob; returns false on a bad header.  The
       host owns the file: write the blob on change, read + load it at startup.  CALL dock_load at a
       safe point -- between frames or at the top of the build before any docked window's begin_window
       -- never from inside a docked window (it frees + rebuilds the tree). */
    u32  ( *dock_save )( imgui_vp_t vp, char* buf, u32 bufsz );
    bool ( *dock_load )( imgui_vp_t vp, const char* text );

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
           imgui()->set_item_tooltip( "Does the thing" );

       help_marker draws a dim "(?)" hint that pops `text` on hover -- the Dear ImGui footnote,
       typically emitted on the same line after a control:

           imgui()->checkbox( "No mouse", &flag );
           imgui()->same_line( 0.0f );
           imgui()->help_marker( "Disable mouse inputs and interactions." ); */

    void ( *set_item_tooltip )( const char* text );
    bool ( *begin_tooltip    )( void );
    void ( *end_tooltip      )( void );
    void ( *help_marker      )( const char* text );

    /* Menus -- a coordination layer over the popup stack.  A menu bar holds begin_menu entries;
       each opens a submenu popup that holds menu_items and further begin_menu entries (nesting on
       the popup stack).  Disabled state reuses the item-flag stack: push_item_flag(IMGUI_ITEM_DISABLED).

       begin_main_menu_bar pins a bar across the top of the display; begin_menu_bar fills the strip a
       window reserved with IMGUI_WIN_MENUBAR (and returns false on a window without the flag).  Both
       return true only when visible -- guard the entries on the return and call the matching end only
       then, exactly like begin_window / begin_popup.

           if ( imgui()->begin_main_menu_bar() ) {
               if ( imgui()->begin_menu( "File" ) ) {
                   if ( imgui()->menu_item( "Open", "Ctrl+O", NULL ) ) { ... }
                   imgui()->menu_item( "Show grid", NULL, &show_grid );   // checkable
                   if ( imgui()->begin_menu( "Recent" ) ) {              // submenu
                       imgui()->menu_item( "a.txt", NULL, NULL );
                       imgui()->end_menu();
                   }
                   imgui()->end_menu();
               }
               imgui()->end_main_menu_bar();
           }

       begin_menu renders horizontally in a bar (its popup drops below) and as a full-width row with
       a submenu arrow inside a menu (its popup opens to the side); the orientation follows the active
       layout mode, so no flag is needed.  menu_item returns true on the clicked frame and dismisses
       the whole menu chain; shortcut is display-only (may be NULL); selected may be NULL (a plain
       command) or a bool* (a checkable item, toggled on click). */

    bool ( *begin_main_menu_bar )( void );
    void ( *end_main_menu_bar   )( void );
    bool ( *begin_menu_bar      )( void );
    void ( *end_menu_bar        )( void );
    bool ( *begin_menu )( const char* label );
    void ( *end_menu   )( void );
    bool ( *menu_item  )( const char* label, const char* shortcut, bool* selected );

    /* Child regions -- a nested scrollable layout box inside the current window (or another
       child).  begin_child carves a box of height h (width w, or the remaining content width
       when w <= 0) from the layout pen, clips and scrolls its contents independently, and
       gives it its own scrollbar; flags take the IMGUI_WIN_*SCROLL policy bits.  h <= 0
       auto-sizes the height to the content (AutoResizeY).  IMGUI_WIN_CHILD_RESIZE_X / _Y add a
       draggable grip on the right / bottom border (flow children only): that axis becomes
       user-owned and persisted, seeded from w/h then driven by the drag, the way a window owns
       its size.  set_next_window_size_constraints (above) bounds the resolved size, so an
       auto-sized box can grow with its content up to a max height and then scroll.  Always pair
       with end_child -- the parent layout resumes directly below the box.  Fill it with any
       widgets (e.g. selectable rows for a list box).  Always returns true. */

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

    /* canvas() -- reserve a full-width drawing area of `height` px in the layout (height <= 0 fills
       the rest of the region) and return its screen rect, for custom geometry drawn with the
       draw_* / path_* calls.  It flows like any widget and the window clips it. */
    imgui_rect_t ( *canvas )( f32 height );

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

    /* cursor_screen_pos -- screen position where the next item would land (GetCursorScreenPos): anchor
       custom draw_* geometry to the pen.  dummy -- reserve a w x h block and return its screen rect
       (Dummy): blank space, or a slot to fill with custom draw / make clickable with invisible_button.
       `w` is the main-axis size (honored in pack / same_line; column flow sizes to the track). */
    imgui_vec2_t ( *cursor_screen_pos )( void );
    imgui_rect_t ( *dummy )( f32 w, f32 h );

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

    /* begin_disabled / end_disabled -- named-scope shorthand for IMGUI_ITEM_DISABLED (BeginDisabled
       / EndDisabled).  begin_disabled( true ) dims + inerts the bracketed widgets; ( false ) pushes
       a no-op scope so a conditional disable still balances.  Nests: an inner ( false ) never
       re-enables widgets an outer ( true ) disabled. */
    void ( *begin_disabled )( bool disabled );
    void ( *end_disabled   )( void );

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

    /* set_nav_window() -- aim keyboard navigation at a window by title (the explicit-focus entry).
       Clears the nav cursor so the window's first item takes focus and engages the nav highlight.
       Nav otherwise follows the front-most window automatically; Ctrl+Tab cycles among windows and
       Alt enters the main menu bar.  An open popup / menu always captures nav while it is open. */
    void ( *set_nav_window )( const char* title );

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched begin_window / end_window pair, and only
       when begin_window returned true -- a collapsed window draws no clip, so widgets emitted
       into it render straight onto the screen.  The bool guard is the caller's job. */

    void ( *text        )( const char* str );
    void ( *textf       )( const char* fmt, ... );
    void ( *bullet_text )( const char* str );

    /* text_colored / text_disabled -- a text run in an explicit colour / the dim secondary colour.
       text_wrapped -- a run word-wrapped to the region content width (paragraphs, help blurbs).
       bullet -- a standalone bullet glyph; new_line -- break + one blank text line (undo same_line). */
    void ( *text_colored  )( u32 abgr, const char* str );
    void ( *text_disabled )( const char* str );
    void ( *text_wrapped  )( const char* str );
    void ( *bullet        )( void );
    void ( *new_line      )( void );

    /* label_text -- a read-only "value + label" row that lays out like the labeled value widgets
       (label track / control track under a form or field_split, trailing label otherwise) but is
       pure display.  For information rows that align with the editable widgets around them. */
    void ( *label_text  )( const char* label, const char* value );
    bool ( *button      )( const char* label );

    /* small_button -- a compact button with no vertical frame padding (a text-height row), for
       inline controls packed onto a text line.  progress_bar -- a filled completion track showing
       `fraction` (0..1) with a centered caption (NULL = "NN%" percentage, "" = no text). */
    bool ( *small_button )( const char* label );
    void ( *progress_bar )( f32 fraction, const char* overlay );

    /* arrow_button -- a square, framed, non-text button drawing a triangle pointing `dir`.  The id
       comes from the label (use a "##id" string, nothing is displayed).  Combine with
       push_item_flag( IMGUI_ITEM_BUTTON_REPEAT, true ) for press-and-hold stepping (spin buttons). */
    bool ( *arrow_button )( const char* id_str, imgui_dir_t dir );

    /* invisible_button -- standard button interaction (hover, press-capture, click) on an explicit
       rect the caller already holds (a canvas() cut, a dummy() slot, any custom-drawn region); returns
       true on the click frame.  Owns no layout reservation, so it composes with the rect helpers:
       cut/draw a region, then make it clickable.  For only a hover tint, use is_mouse_hovering_rect. */
    bool ( *invisible_button )( const char* id_str, imgui_rect_t r );

    bool ( *checkbox    )( const char* label, bool* v );

    /* radio_button -- one option of a mutually-exclusive set: shows on while *v == value, a click
       sets *v = value.  Emit several against the same v (same_line between them for a row) to form
       a group; returns true only on the frame a click changes the selection. */
    bool ( *radio_button )( const char* label, i32* v, i32 value );
    /* slider_float -- draggable [lo,hi] slider; returns true while dragging.  The current value is
       drawn centered on the track by default ("%.3f"); set IMGUI_ITEM_NO_VALUE_TEXT (push or
       next_item_flag) to hide it for a bare slider. */
    bool ( *slider_float)( const char* label, f32* v, f32 lo, f32 hi );

    /* slider_float_step -- slider_float that quantizes the value to `step` (e.g. 0.25 snaps to the
       quarter marks); step <= 0 is continuous, identical to slider_float. */
    bool ( *slider_float_step)( const char* label, f32* v, f32 lo, f32 hi, f32 step );

    /* slider_int -- integer slider over [lo,hi]; every track position lands on a whole value, drawn
       centered ("%d").  Same IMGUI_ITEM_NO_VALUE_TEXT suppression as slider_float. */
    bool ( *slider_int  )( const char* label, i32* v, i32 lo, i32 hi );

    /* drag_int -- a framed integer field driven by a left/right drag (the DragInt analogue): no
       track, so no max travel -- v_speed units of value per pixel.  v_min < v_max bounds it; both
       equal leaves it unbounded.  format is the printf form of the shown value ("%d" when NULL,
       e.g. "HP: %d").  Returns true only on frames the drag changes the value. */
    bool ( *drag_int    )( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format );

    /* drag_float -- the floating-point DragFloat: a framed value changed by a left/right drag,
       v_speed units per pixel, no track travel.  v_min < v_max bounds it; both equal is unbounded.
       fmt is the printf form ("%.3f" when NULL).  drag_float2/3/4 lay N equal sub-boxes (vector edit). */
    bool ( *drag_float  )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float2 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float3 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float4 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );

    bool ( *input_text    )( const char* label, char* buf, u32 bufsz );

    /* input_text_ex -- like input_text but with an on_change callback fired after any frame
       that modifies the buffer.  Pass NULL for on_change to suppress.  cb_user is forwarded
       verbatim to the callback. */
    bool ( *input_text_ex )( const char* label, char* buf, u32 bufsz,
                              imgui_text_cb_fn on_change, void* cb_user );

    /* input_text_with_hint -- like input_text but shows `hint` in dim text inside the box
       when the buffer is empty and the field is not focused.  The hint is never written to buf. */
    bool ( *input_text_with_hint )( const char* label, const char* hint, char* buf, u32 bufsz );

    /* input_int / _float / _double -- numeric text field that parses on Enter or focus loss.
       step != 0 shows [-][+] buttons at the right of the box; Ctrl uses step_fast.
       fmt is the snprintf format for display and focus-seed ("%.3f" / "%d" when NULL).
       Scientific notation is accepted ("1e+8").  Returns true when the value changes. */
    bool ( *input_int    )( const char* label, i32* v, i32 step, i32 step_fast );
    bool ( *input_float  )( const char* label, f32* v, f32 step, f32 step_fast, const char* fmt );
    bool ( *input_double )( const char* label, f64* v, f64 step, f64 step_fast, const char* fmt );

    /* input_floatN -- N-component float row: N equal text boxes across the control track.
       fmt applies to every component (NULL -> "%.3f").  Returns true if any component changes. */
    bool ( *input_float2 )( const char* label, f32* v, const char* fmt );
    bool ( *input_float3 )( const char* label, f32* v, const char* fmt );
    bool ( *input_float4 )( const char* label, f32* v, const char* fmt );

    /* selectable -- a full-width row that highlights on hover and fills when selected; the
       list-box building block.  A click toggles *selected (pass NULL for click-only); returns
       true on the clicked frame so a caller managing single-selection can set its own index. */
    bool ( *selectable  )( const char* label, bool* selected );

    /* Combo box -- a framed preview box (selected text + a down arrow) with a trailing label that
       drops a popup of rows below it on click.  begin_combo opens the dropdown: it returns true
       only while the dropdown is open, so -- like begin_window's collapse -- guard the rows on the
       return and call end_combo only then.  preview_value is the text shown in the closed box (the
       caller's current selection, usually items[current]).  A row clicked in the body dismisses the
       combo automatically, so emit selectables and set your selection from their return:

           if ( imgui()->begin_combo( "mode", items[cur], IMGUI_COMBO_NONE ) ) {
               for ( i32 i = 0; i < n; ++i )
                   if ( imgui()->selectable( items[i], NULL ) ) cur = i;
               imgui()->end_combo();
           }

       flags is imgui_combo_flags_t: the HEIGHT_* group caps the dropdown to a fixed row count
       (then it scrolls), 0 (IMGUI_COMBO_NONE) is the ~8-row default.  combo() is the one-liner over
       an array of strings (*current_item is the selected index; out of range shows an empty
       preview).  Both return true on the frame the selection changes. */
    bool ( *begin_combo )( const char* label, const char* preview_value, imgui_combo_flags_t flags );
    void ( *end_combo   )( void );
    bool ( *combo       )( const char* label, i32* current_item, const char* const items[], i32 count );

    /* List box -- a framed, independently scrolling box of selectable rows with a trailing label.
       begin_listbox opens the box (w / h in pixels; w <= 0 fills the line after the label, h <= 0
       is ~7 rows tall) and always returns true -- always pair with end_listbox, and fill it with
       selectables exactly like a begin_child:

           if ( imgui()->begin_listbox( "items", 0, 0 ) ) {
               for ( i32 i = 0; i < n; ++i ) {
                   bool sel = ( cur == i );
                   if ( imgui()->selectable( names[i], &sel ) ) cur = i;
               }
               imgui()->end_listbox();
           }

       listbox() is the one-liner over an array of strings; height_in_items <= 0 picks
       min(count, 7).  Returns true on the frame the selection changes. */
    bool ( *begin_listbox )( const char* label, f32 w, f32 h );
    void ( *end_listbox   )( void );
    bool ( *listbox       )( const char* label, i32* current_item, const char* const items[],
                             i32 count, i32 height_in_items );

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

    /* text_size -- laid-out pixel size of s (widest line x line span; '\n' breaks).  CalcTextSize. */
    imgui_vec2_t ( *text_size )( const char* s );

    /* draw_text_in -- draw s aligned within rect r (imgui_align_t; multi-line, each line aligned).
       The placement primitive: "right-align this caption in the canvas" with no hand-computed edge.
       draw_text_clipped is the single-line variant that ellipsizes to r's width. */
    void ( *draw_text_in      )( imgui_rect_t r, imgui_align_t align, u32 col, const char* s );
    void ( *draw_text_clipped )( imgui_rect_t r, imgui_align_t align, u32 col, const char* s );

    /* Line / path stroking (imgui_stroke_align_t; see imgui.h for the pixel model).
       draw_line     -- one segment, CENTER_BIASED: H/V lines render pixel-crisp, others antialiased.
       draw_polyline -- a connected point array with miter-limited corners (always antialiased);
                        `closed` joins the last point back to the first (rect / polygon outlines).
       path_*        -- the retained form: clear, append points with path_line_to, then path_stroke
                        (which strokes and clears the buffer).  Up to IMGUI_PATH_MAX points.

           imgui()->draw_line( 10, 10, 200, 80, 2.0f, col );      // a 2px antialiased diagonal
           imgui()->path_line_to( x0, y0 ); imgui()->path_line_to( x1, y1 ); ...
           imgui()->path_stroke( 1.5f, IMGUI_STROKE_CENTER, false, col ); */

    void ( *draw_line     )( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr );
    void ( *draw_polyline )( const imgui_vec2_t* pts, u32 count, f32 thickness,
                             imgui_stroke_align_t align, bool closed, u32 abgr );
    void ( *path_clear    )( void );
    void ( *path_line_to  )( f32 x, f32 y );
    void ( *path_stroke   )( f32 thickness, imgui_stroke_align_t align, bool closed, u32 abgr );

    void ( *push_clip )( f32 x, f32 y, f32 w, f32 h );
    void ( *pop_clip  )( void );

    /* Debug overlay -- a separate draw list painted last, on top of the UI.  Pass a bitmask
       of imgui_dbg_layer_t to debug_set_layers() to choose which visualizations show; pass
       IMGUI_DBG_NONE (0) to turn it off.  Compiled in for Debug builds only: in Release,
       set_layers is a no-op and get_layers returns 0.  The two slots stay in the vtable in
       every build so func_api_size is identical across a hot-reload. */

    void ( *debug_set_layers )( u32 layers );
    u32  ( *debug_get_layers )( void );

    /* IO accessors -- the frame-coherent input snapshot the widgets see, for UI / tool code that
       would otherwise re-query app() and so bypass imgui's frame timing and its input capture.

       want_capture_mouse / want_capture_keyboard are the fence: a true return means imgui owns the
       device this frame (the cursor is over a window, a widget is dragging, or a field is focused),
       so non-UI code should NOT also act on it.  Gate direct app() input reads in gameplay / tools
       on the inverse:

           if ( !imgui()->want_capture_keyboard() && app()->key_pressed( APP_KEY_SPACE ) )
               jump();

       The is_key_* / is_mouse_* / get_* readers return the same per-frame state the widgets use
       (keyed by app_key_t / app_mouse_button_t).  is_key_pressed / is_mouse_clicked are the down-
       edge this frame.  get_time is seconds since the first frame (accumulated dt); get_delta_time
       is this frame's.

       Key repeat is per-query (no mode to set): is_key_pressed is the initial press only, while
       is_key_pressed_repeat also fires on each OS auto-repeat tick at the user's system rate -- the
       Dear ImGui IsKeyPressed(key, repeat=true) case.  Use the repeat reader for held-key actions
       (text nav, a spinner); use the plain one for discrete actions that must fire once per press. */

    bool ( *want_capture_mouse       )( void );
    bool ( *want_capture_keyboard    )( void );

    /* is_mouse_hovering_rect -- cursor is over r and r is interactable (front-most window, inside the
       region clip, no drag in flight): the IsMouseHoveringRect analogue for custom-drawn hit tests. */
    bool ( *is_mouse_hovering_rect   )( imgui_rect_t r );

    /* Last-item introspection (the ImGui IsItem* family) -- each reports on the widget just emitted,
       so call immediately after it.  hovered / active / clicked / focused mirror the widget's own
       interaction; activated / deactivated are the press / release edges (deactivated is the natural
       "commit on release" seam); visible is true when any of the item's rect survives the region
       clip; get_item_rect returns its screen rect (GetItemRectMin/Max/Size in one). */
    bool         ( *is_item_hovered     )( void );
    bool         ( *is_item_active      )( void );
    bool         ( *is_item_clicked     )( void );
    bool         ( *is_item_focused     )( void );
    bool         ( *is_item_activated   )( void );
    bool         ( *is_item_deactivated )( void );
    bool         ( *is_item_visible     )( void );
    imgui_rect_t ( *get_item_rect       )( void );

    bool ( *is_key_down              )( app_key_t key );
    bool ( *is_key_pressed           )( app_key_t key );
    bool ( *is_key_pressed_repeat    )( app_key_t key );
    bool ( *is_key_released          )( app_key_t key );
    bool ( *is_mouse_down            )( app_mouse_button_t b );
    bool ( *is_mouse_clicked         )( app_mouse_button_t b );
    bool ( *is_mouse_released        )( app_mouse_button_t b );
    bool ( *is_mouse_double_clicked  )( app_mouse_button_t b );
    void ( *get_mouse_pos            )( f32* x, f32* y );
    f32  ( *get_mouse_wheel          )( void );
    f32  ( *get_delta_time           )( void );
    f64  ( *get_time                 )( void );

    /* wants_redraw -- true when at least one animated widget has not yet reached its target this frame.
       The host checks this after the UI build to decide whether to skip the editor sleep: while any
       transition is in flight, the loop must keep pumping frames to advance the animation. */
    bool ( *wants_redraw )( void );

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
