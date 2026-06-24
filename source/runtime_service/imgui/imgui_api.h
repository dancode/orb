#ifndef IMGUI_API_H
#define IMGUI_API_H
/*==============================================================================================

    runtime_service/imgui/imgui_api.h -- imgui module API struct and gateway macro.
    Always statically linked into the host.

    Function groups (all called through imgui() vtable or as imgui_* direct calls):
        Lifecycle : init / shutdown
        Frame     : frame_begin / ctx_begin / ctx_end / frame_end / render
        Panels    : window_begin / window_end
        Widgets   : text / button / checkbox / slider_float / input_text
        Draw      : draw_rect / draw_text / push_clip / pop_clip

==============================================================================================*/

#include "runtime_service/imgui/imgui.h"

#include "engine/app/app.h"             /* app_event_t for event()   */
#include "engine/mod/mod_import.h"

/* forward declare so the API can take a cmd argument without including rhi_api.h */
struct rhi_cmd_s; typedef struct rhi_cmd_s* rhi_cmd_t;


// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct imgui_api_s
{
    /* GPU resource lifecycle.
       init()      -- call after rhi()->init(); creates pipeline, font atlas, GPU buffers.
       shutdown()  -- call before rhi()->shutdown(); destroys all GPU resources.
       load_font() -- load a pre-baked .orb_font atlas into a new font id and make it active;
                      call after init().  Returns the new id (>= 1), or 0 on failure. */

    bool ( *init      )( void );
    void ( *shutdown  )( void );
    u32  ( *load_font )( const char* path );

    /* GPU resource memory currently held by imgui, in bytes (buffers + atlases).
       print_mem_stats() dumps the same breakdown to stdout. */
    imgui_mem_stats_t ( *mem_stats       )( void );
    void              ( *print_mem_stats )( void );

    /* Per-frame render statistics (geometry + batch counts) for the LAST completed frame.
       Published at frame_begin, so a read during the build reflects the previous frame -- the
       standard one-frame lag.  Feeds an FPS / performance overlay without re-deriving counts. */
    imgui_render_stats_t ( *render_stats )( void );

    /* Built-in performance overlay -- a hidden-chrome, non-interactive FPS / cost readout pinned to
       the top-left of the primary viewport.  Emit it once per frame inside the UI build (last, so it
       draws on top); it lands in the currently bound context.  `mode` cycles the detail tier, each a
       superset of the one below ( <= 0 draws nothing ):

           1 : FPS (color-graded green/amber/red by health)
           2 : + imgui emit + render CPU time (ms, smoothed)
           3 : + render counts from render_stats() -- verts / tris / batches / cmds

       imgui owns no clock of its own, so the host passes one: `clock` is a monotonic seconds source
       (e.g. sys()->tick_seconds).  imgui adopts it here and uses it to bracket the frame -- the emit
       clock opens at frame_begin() and closes at frame_end() (build cost); the render clock sums the
       render() flush calls -- so the readout trails the work it describes by one frame.  Pass the
       same callback every frame; NULL leaves timing off (mode 2 then reads zero). */
    void ( *perf_overlay )( imgui_clock_fn clock, int mode );

    /* Frame lifecycle.  A frame is four explicit phases -- this is a multi-context system and the
       API does not hide it; even a single-context host names its one context:

         frame_begin(dt)               -- global: reset draw list + snapshot app input.  Binds NO
                                          context; call once at the top of the frame.
           ctx_begin(IMGUI_CTX_DEFAULT) -- bind a context and run its per-frame init; emit its
              ... emit windows ...        windows immediately after.
           ctx_end()                    -- close it, rebinding the previously-bound context.
         frame_end()                   -- seal the build (latches emit cost; asserts ctx balance).

       frame_begin/frame_end and ctx_begin/ctx_end are balanced scopes, exactly like
       window_begin/window_end: every begin has an end, and each end restores the scope its begin
       opened.  render() runs AFTER frame_end and consumes the sealed draw list.

       render()    -- flush one viewport's geometry partition to GPU; opens a LOAD render pass on
                      that viewport's swapchain, emits all draw calls, and closes the pass.  Also
                      paints the debug overlay when vp is the primary (index 0).
                      Call once per live viewport, each with the matching context cmd. */

    void ( *frame_begin )( f32 dt );
    void ( *frame_end   )( void );
    void ( *render      )( imgui_vp_t vp, rhi_cmd_t cmd );

    /* Viewport management.  A viewport is a render surface backed by an OS window.  One frame's build
       gathers every window's geometry into a single draw list; render() dispatches each window's
       partition to the viewport it is assigned to (window_set_next_viewport, or inherited from
       whichever viewport was most recently emitted into this frame).

       viewport_open()   -- open a surface for OS window win_id with initial drawable size w x h.
                            Returns a handle (imgui_vp_t >= 0) or IMGUI_VP_INVALID if the pool is full.
                            The first call creates the primary (index 0); call before any frames.
                            win_id routes mouse events from that OS window to this surface.
       viewport_close()  -- close a non-primary viewport and release its GPU geometry buffers.
                            Windows on the closed viewport automatically fall back to the primary.
                            The host owns the OS window and rhi context; imgui owns only the geometry.
       viewport_resize() -- update a viewport's drawable size.  Call on OS resize BEFORE frame_begin.
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
                                    window_set_next_viewport) or IMGUI_VP_INVALID.  Between frames.
       update_platform_windows() -- reconcile owned floaters with their OS windows; call once per
                                    frame AFTER the UI build and BEFORE rendering (the safe point to
                                    tear a surface down -- destroys those the user closed).
       render_floaters()         -- present every owned floater from the shared draw list, each on
                                    its own rhi context (frame_begin/clear/flush/frame_end).  The
                                    host still presents the main surface (index 0) via render(). */

    imgui_vp_t ( *viewport_spawn          )( const char* title, i32 x, i32 y, i32 w, i32 h );
    void       ( *update_platform_windows )( void );
    void       ( *render_floaters         )( void );

    /* Multi-context -- isolated per-context retained state (windows, nav, popups, keyed widget state,
       id namespace).  The primary context (IMGUI_CTX_DEFAULT / 0) is always live after init().

       ctx_create()       -- allocate a fresh secondary context, sized to `cfg` (NULL = editor defaults).
                             Each gets a unique id_salt so same-named widgets in different contexts
                             never alias.  Returns IMGUI_CTX_INVALID on pool exhaustion.  Between frames.
       ctx_destroy()      -- free a secondary context; rebinds the default if it was current.  Never
                             destroys IMGUI_CTX_DEFAULT.  Call between frames.
       ctx_bind()         -- make ctx the current context with no per-frame init: a mid-build "switch
                             retained state" escape hatch.  ctx_begin/ctx_end are the normal scope;
                             reach for ctx_bind only to peek at another context's state mid-frame.
                             IMGUI_CTX_DEFAULT (0) or any invalid handle rebinds the default.
       ctx_set_listening() -- set whether a context receives hover/click/nav input.  The default context
                             starts listening; secondary contexts start deaf.  Multiple contexts may
                             listen simultaneously; a deaf context renders but returns inert widget state.
                             Call between frames.
       ctx_begin()/ctx_end() -- bind a context for the frame and run its per-frame init, then close it.
                             A balanced scope: ctx_end rebinds whatever ctx_begin found bound.  ctx_begin
                             always runs the full frame init (hover promotion, nav, popup stale-close)
                             regardless of the listening flag, and leaves g_ctx pointing at the context,
                             so emit its windows IMMEDIATELY after the call.

       FRAME CONTRACT:
         frame_begin(dt)                -- once: input poll + draw-list reset; binds no context.
           ctx_begin(IMGUI_CTX_DEFAULT) -- bind + init the default context; emit its windows.
           ctx_end()                    -- close it.
           ctx_begin(ctx2)              -- a second context, if any; emit its windows.
           ctx_end()
         frame_end()                    -- seal the build.
       A single-context host runs exactly one ctx_begin(IMGUI_CTX_DEFAULT)/ctx_end pair. */

    imgui_ctx_t ( *ctx_create        )( const imgui_ctx_config_t* cfg );
    void        ( *ctx_destroy       )( imgui_ctx_t ctx );
    void        ( *ctx_bind          )( imgui_ctx_t ctx );
    void        ( *ctx_set_listening )( imgui_ctx_t ctx, bool listen );
    void        ( *ctx_begin         )( imgui_ctx_t ctx );
    void        ( *ctx_end           )( void );

    /* Host input -- the host owns the app event ring drain and forwards each
       event here before frame_begin() for the same frame.
       event() -- forward one drained app_event_t; imgui unpacks the input events
                  it cares about (text + scroll) and returns true if it consumed
                  the event, letting the host skip its own handling for it. */

    bool ( *event )( const app_event_t* ev );

    /* Panels -- open a window panel; must be matched with window_end().
       flags is a bitmask of imgui_win_flags_t (0 / IMGUI_WIN_NONE for the defaults) that
       switches off built-in behavior per window -- title bar, collapse, or edge resize.

       window_begin() returns false when the window is collapsed (title bar only).  Guard
       the body widgets with it -- skipped widgets cost nothing -- but always call
       window_end() regardless of the return value:

           if ( imgui()->window_begin( "Tools", IMGUI_WIN_NONE ) )
           {
               imgui()->text( "..." );          // skipped while collapsed
           }
           imgui()->window_end();               // always called */

    /* window_set_next_pos / _size -- queue geometry for the NEXT window_begin, applied per the
       condition (imgui_cond_t) and then cleared.  Decouples the value from when it is applied:
       ONCE seeds an initial position/size (apply once on first appearance, then user-owned),
       ALWAYS forces it every frame (layout managers, snapping, animation -- pair with NOMOVE /
       NORESIZE), APPEARING re-applies it each time the window is shown after being absent.
       Call immediately before window_begin. */
    void ( *window_set_next_pos  )( f32 x, f32 y, imgui_cond_t cond );
    void ( *window_set_next_size )( f32 w, f32 h, imgui_cond_t cond );

    /* window_set_next_viewport -- assign the NEXT window_begin to a specific viewport.  Sticky: it
       lands on the window record and persists across frames until reassigned.  Omit to inherit the
       ambient viewport -- the one most recently emitted into this frame -- so windows created from
       within a viewport's panels naturally land on the same surface without explicit assignment.
       If the assigned viewport is later closed, the window automatically reverts to the primary. */
    void ( *window_set_next_viewport )( imgui_vp_t vp );

    /* window_set_next_size_constraints -- queue a one-shot [min,max] size box for the NEXT
       child_begin, then cleared.  The Dear ImGui SetNextWindowSizeConstraints analogue, in its
       most useful form: it bounds the child's resolved width / height, so an auto-sized (h <= 0)
       box grows with its content up to max_h and then scrolls, never collapses below min_h, and a
       CHILD_RESIZE_* drag cannot leave the range.  A bound <= 0 is "unconstrained" on that side
       (e.g. 0, 0, 0, max_h to cap height only).  Call immediately before child_begin. */
    void ( *window_set_next_size_constraints )( f32 min_w, f32 min_h, f32 max_w, f32 max_h );

    bool ( *window_begin )( const char* title, imgui_win_flags_t flags );
    void ( *window_end   )( void );

    /* window_set_open / window_is_open -- drive a CLOSEABLE window's visibility by title (the same
       key window_begin hashes).  The window's close (X) button hides it; the host re-opens it by
       calling window_set_open( title, true ) from a button.  window_is_open reports the current
       state (a window with no record yet -- never begun -- reads as open). */
    void ( *window_set_open )( const char* title, bool open );
    bool ( *window_is_open  )( const char* title );

    /* Docking -- tile + tab windows into a dock tree that fills a viewport (the DockSpaceOverViewport
       analogue).  Phase 1 is programmatic: build a layout in code, then windows whose titles were
       dock_window'd render into their node (no per-window title bar -- the node draws a shared tab
       strip) instead of free-floating.  Free-floating windows still overlap on top of the dockspace.

       dockspace_over_viewport() -- ensure viewport vp hosts a dock tree, lay it out over the surface,
                                    draw + interact its splitters, and return the tree ROOT node id.
                                    Call once per frame at the TOP of the build, before the docked
                                    windows' window_begin (which read their resolved node rects).
       dock_split()              -- split a LEAF node in two; returns the NEW empty leaf on the `dir`
                                    side and writes the REMAINING node id to *out_remain (may be NULL).
                                    `ratio` is the new side's fraction of the axis.  The DockBuilder
                                    idiom -- keep splitting the returned remainder to carve a layout.
       dock_window()             -- add a window (matched to window_begin by title) as a tab in a leaf,
                                    moving it out of any node it was already in; it becomes active.
       dock_undock()             -- remove a window from its node, returning it to free-floating.
       window_is_docked()        -- true while the window is tabbed into some node.

           imgui_dock_id_t root  = imgui()->dockspace_over_viewport( 0, IMGUI_DOCKSPACE_NONE );
           imgui_dock_id_t left  = imgui()->dock_split( root, IMGUI_DIR_LEFT, 0.25f, &root );
           imgui()->dock_window( "Scene Tree", left );
           imgui()->dock_window( "Viewport",   root );   // center; tab more windows here with root */

    imgui_dock_id_t ( *dockspace_over_viewport )( imgui_vp_t vp, imgui_dockspace_flags_t flags );
    imgui_dock_id_t ( *dock_split )( imgui_dock_id_t node, imgui_dir_t dir, f32 ratio,
                                     imgui_dock_id_t* out_remain );
    /* dock_split_root() -- split the WHOLE viewport tree, carving a new leaf along a full edge (`dir`).
       Unlike dock_split (a single leaf), this wraps the root in a new split so the pane spans the entire
       side -- the way to place a full-height column beside an existing top/bottom stack.  Returns the
       new leaf id (dock windows into it), or IMGUI_DOCK_NONE.  Also the commit path of an edge drop. */
    imgui_dock_id_t ( *dock_split_root )( imgui_vp_t vp, imgui_dir_t dir, f32 ratio );
    void ( *dock_window )( const char* title, imgui_dock_id_t node );
    void ( *dock_undock )( const char* title );
    bool ( *window_is_docked )( const char* title );

    /* Layout persistence.  dock_save() serializes viewport vp's dock tree into buf as a small ASCII
       blob and returns the byte count a full write needs (like snprintf -- pass a 0 bufsz to size
       first).  dock_load() rebuilds the tree from such a blob; returns false on a bad header.  The
       host owns the file: write the blob on change, read + load it at startup.  CALL dock_load at a
       safe point -- between frames or at the top of the build before any docked window's window_begin
       -- never from inside a docked window (it frees + rebuilds the tree). */
    u32  ( *dock_save )( imgui_vp_t vp, char* buf, u32 bufsz );
    bool ( *dock_load )( imgui_vp_t vp, const char* text );

    /* Popups -- transient overlay windows on top of everything.  A regular popup auto-closes when
       the user clicks outside it; a modal blocks input behind it and dims the background, closing
       only via popup_close_current.  The string id namespaces both the open request and the body,
       so popup_open("x") and popup_begin("x") must use the same id.  Popups stack (a popup opened
       while inside another nests under it); a click keeps the deepest popup under the cursor and
       closes the rest.  Popup / tooltip bodies lay out like a window body: declare a layout header
       (stack / columns / ...) before emitting widgets.

           if ( imgui()->button( "Open" ) )    imgui()->popup_open( "menu" );
           if ( imgui()->popup_begin( "menu", IMGUI_WIN_NONE ) ) {
               imgui()->stack();
               if ( imgui()->selectable( "Cut",  NULL ) ) { ... }
               if ( imgui()->selectable( "Copy", NULL ) ) { ... }
               imgui()->popup_end();
           }

       popup_begin / popup_modal_begin return true only when the popup is open AND visible -- guard
       the body and call popup_end only on a true return (like window_begin's collapsed contract).
       Auto-sized popups (the default) measure their content on the appearing frame off-screen and
       snap into place the next frame, so there is no first-frame size pop. */

    void ( *popup_open          )( const char* id );
    bool ( *popup_begin         )( const char* id, imgui_win_flags_t flags );
    bool ( *popup_modal_begin   )( const char* id, const char* title, imgui_win_flags_t flags );
    void ( *popup_end           )( void );
    void ( *popup_close_current )( void );
    bool ( *popup_is_open        )( const char* id );

    /* Context menus -- open a popup on a right-click.  _item binds to the previous widget (the one
       emitted just before the call); _window binds to empty space in the current window.  Use them
       in place of the popup_open + popup_begin pair:

           imgui()->selectable( "Row", NULL );
           if ( imgui()->popup_context_item_begin( "row_ctx" ) ) { ...; imgui()->popup_end(); } */

    bool ( *popup_context_item_begin   )( const char* id );
    bool ( *popup_context_window_begin )( const char* id );

    /* Tooltips -- a non-interactive overlay shown at the cursor while the previous widget is
       hovered.  set_item_tooltip is the one-liner; tooltip_begin / tooltip_end wrap a multi-widget
       body (guard the body on the true return, always call tooltip_end).

           imgui()->button( "Hover me" );
           imgui()->set_item_tooltip( "Does the thing" );

       help_marker draws a dim "(?)" hint that pops `text` on hover -- the Dear ImGui footnote,
       typically emitted on the same line after a control:

           imgui()->checkbox( "No mouse", &flag );
           imgui()->same_line( 0.0f );
           imgui()->help_marker( "Disable mouse inputs and interactions." ); */

    void ( *set_item_tooltip )( const char* text );
    bool ( *tooltip_begin    )( void );
    void ( *tooltip_end      )( void );
    void ( *help_marker      )( const char* text );

    /* Menus -- a coordination layer over the popup stack.  A menu bar holds menu_begin entries;
       each opens a submenu popup that holds menu_items and further menu_begin entries (nesting on
       the popup stack).  Disabled state reuses the item-flag stack: push_item_flag(IMGUI_ITEM_DISABLED).

       main_menu_bar_begin pins a bar across the top of the display; menu_bar_begin fills the strip a
       window reserved with IMGUI_WIN_MENUBAR (and returns false on a window without the flag).  Both
       return true only when visible -- guard the entries on the return and call the matching end only
       then, exactly like window_begin / popup_begin.

           if ( imgui()->main_menu_bar_begin() ) {
               if ( imgui()->menu_begin( "File" ) ) {
                   if ( imgui()->menu_item( "Open", "Ctrl+O", NULL ) ) { ... }
                   imgui()->menu_item( "Show grid", NULL, &show_grid );   // checkable
                   if ( imgui()->menu_begin( "Recent" ) ) {              // submenu
                       imgui()->menu_item( "a.txt", NULL, NULL );
                       imgui()->menu_end();
                   }
                   imgui()->menu_end();
               }
               imgui()->main_menu_bar_end();
           }

       menu_begin renders horizontally in a bar (its popup drops below) and as a full-width row with
       a submenu arrow inside a menu (its popup opens to the side); the orientation follows the active
       layout mode, so no flag is needed.  menu_item returns true on the clicked frame and dismisses
       the whole menu chain; shortcut is display-only (may be NULL); selected may be NULL (a plain
       command) or a bool* (a checkable item, toggled on click). */

    bool ( *main_menu_bar_begin )( void );
    void ( *main_menu_bar_end   )( void );
    bool ( *menu_bar_begin      )( void );
    void ( *menu_bar_end        )( void );
    bool ( *menu_begin )( const char* label );
    void ( *menu_end   )( void );
    bool ( *menu_item  )( const char* label, const char* shortcut, bool* selected );

    /* Child regions -- a nested scrollable layout box inside the current window (or another
       child).  child_begin carves a box of height h (width w, or the remaining content width
       when w <= 0) from the layout pen, clips and scrolls its contents independently, and
       gives it its own scrollbar; flags take the IMGUI_WIN_*SCROLL policy bits.  h <= 0
       auto-sizes the height to the content (AutoResizeY).  IMGUI_WIN_CHILD_RESIZE_X / _Y add a
       draggable grip on the right / bottom border (flow children only): that axis becomes
       user-owned and persisted, seeded from w/h then driven by the drag, the way a window owns
       its size.  window_set_next_size_constraints (above) bounds the resolved size, so an
       auto-sized box can grow with its content up to a max height and then scroll.  Always pair
       with child_end -- the parent layout resumes directly below the box.  Fill it with any
       widgets (e.g. selectable rows for a list box).  Always returns true. */

    bool ( *child_begin )( const char* id, f32 w, f32 h, imgui_win_flags_t flags );

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
    void ( *child_end   )( void );

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
       nothing scrolls.  For titlebars, split panes (cell -> child_begin), dashboards, image grids.
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
       GetContentRegionAvail analogue -- size a child_begin to the leftover, or lay out by hand. */
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

    /* disabled_begin / disabled_end -- named-scope shorthand for IMGUI_ITEM_DISABLED (BeginDisabled
       / EndDisabled).  disabled_begin( true ) dims + inerts the bracketed widgets; ( false ) pushes
       a no-op scope so a conditional disable still balances.  Nests: an inner ( false ) never
       re-enables widgets an outer ( true ) disabled. */
    void ( *disabled_begin )( bool disabled );
    void ( *disabled_end   )( void );

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

    /* window_set_drag() -- select how windows may be dragged (global default TITLEBAR).
       Call between frames; affects every window. */
    void ( *window_set_drag )( imgui_win_drag_t mode );

    /* window_set_nav() -- aim keyboard navigation at a window by title (the explicit-focus entry).
       Clears the nav cursor so the window's first item takes focus and engages the nav highlight.
       Nav otherwise follows the front-most window automatically; Ctrl+Tab cycles among windows and
       Alt enters the main menu bar.  An open popup / menu always captures nav while it is open. */
    void ( *window_set_nav )( const char* title );

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched window_begin / window_end pair, and only
       when window_begin returned true -- a collapsed window draws no clip, so widgets emitted
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
       drops a popup of rows below it on click.  combo_begin opens the dropdown: it returns true
       only while the dropdown is open, so -- like window_begin's collapse -- guard the rows on the
       return and call combo_end only then.  preview_value is the text shown in the closed box (the
       caller's current selection, usually items[current]).  A row clicked in the body dismisses the
       combo automatically, so emit selectables and set your selection from their return:

           if ( imgui()->combo_begin( "mode", items[cur], IMGUI_COMBO_NONE ) ) {
               for ( i32 i = 0; i < n; ++i )
                   if ( imgui()->selectable( items[i], NULL ) ) cur = i;
               imgui()->combo_end();
           }

       flags is imgui_combo_flags_t: the HEIGHT_* group caps the dropdown to a fixed row count
       (then it scrolls), 0 (IMGUI_COMBO_NONE) is the ~8-row default.  combo() is the one-liner over
       an array of strings (*current_item is the selected index; out of range shows an empty
       preview).  Both return true on the frame the selection changes. */
    bool ( *combo_begin )( const char* label, const char* preview_value, imgui_combo_flags_t flags );
    void ( *combo_end   )( void );
    bool ( *combo       )( const char* label, i32* current_item, const char* const items[], i32 count );

    /* List box -- a framed, independently scrolling box of selectable rows with a trailing label.
       listbox_begin opens the box (w / h in pixels; w <= 0 fills the line after the label, h <= 0
       is ~7 rows tall) and always returns true -- always pair with listbox_end, and fill it with
       selectables exactly like a child_begin:

           if ( imgui()->listbox_begin( "items", 0, 0 ) ) {
               for ( i32 i = 0; i < n; ++i ) {
                   bool sel = ( cur == i );
                   if ( imgui()->selectable( names[i], &sel ) ) cur = i;
               }
               imgui()->listbox_end();
           }

       listbox() is the one-liner over an array of strings; height_in_items <= 0 picks
       min(count, 7).  Returns true on the frame the selection changes. */
    bool ( *listbox_begin )( const char* label, f32 w, f32 h );
    void ( *listbox_end   )( void );
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

    /* Font -- select / load fonts; call between frames (outside frame_begin / render), except
       push_font / pop_font which may bracket a section or widget mid-frame.

       Fonts live in an id-addressed registry.  Slot 0 is the default / fallback (a built-in
       bitmap to start).  load_font() loads a .orb_font into a fresh id; set_font_file() loads one
       into an existing id (id 0 swaps the default).  use_font() makes a loaded id active; another
       context can select its own font this way.  push_font() / pop_font() bracket a temporary
       font and restore the previous one.  Each load_font/set_font_file uses its own bindless
       texture.  Widget layout dimensions follow the active font's metrics.

       set_font()      -- set the default (id 0) to a built-in bitmap font and use it.
       set_bmp_scale() -- integer pixel-scale multiplier for built-in bitmaps (1 = native, 2 = 2x). */

    void ( *set_font      )( imgui_font_t font );
    void ( *set_bmp_scale )( u32 scale );
    bool ( *set_font_file )( u32 id, const char* path );
    void ( *use_font      )( u32 id );
    void ( *push_font     )( u32 id );
    void ( *pop_font      )( void );

    /* Low-level draw list access -- may be called anywhere between frame_begin and render.
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

    /* Icons -- a runtime-built R8 atlas of arbitrary symbols (folder, gear, check, editor glyphs).
       register_icon packs a raw monochrome bitmap (row-major coverage, w*h bytes) and returns a
       handle (0 = atlas full); the pixels live in the same flush as text and tint by `col`.  Pixel
       sourcing is the caller's (procedural now, the asset pipeline later).  find_icon looks one up
       by the name it was registered with; icon_size is its native pixel size (for layout).
       image is a layout widget (reserve w x h, draw centered/fit); draw_icon_in places an icon in
       a rect the caller already holds (cell / button label / canvas cut).  col 0 means white. */

    imgui_icon_id_t ( *register_icon )( const char* name, u32 w, u32 h, const u8* coverage );
    imgui_icon_id_t ( *find_icon     )( const char* name );
    imgui_vec2_t    ( *icon_size     )( imgui_icon_id_t id );
    void            ( *image         )( imgui_icon_id_t id, f32 w, f32 h, u32 col );
    void            ( *draw_icon_in  )( imgui_rect_t r, imgui_icon_id_t id, u32 col );

    /* Symbol + shape draw primitives (the draw_* family, Dear ImGui's AddXxx / Render* analogue),
       drawn through the normal vertex pipeline (lines / triangles / circles), NOT the icon atlas.
       They share the draw_* verb with draw_rect / draw_text / draw_line above -- everything that
       pushes geometry into the draw list is draw_*; render() is reserved for the frame flush.  The
       built-in widgets draw their check marks, arrows, bullets and close crosses through these, and
       the broader shape
       palette (frames, per-corner rounded rects, polygons, arcs / pie, beziers, dashes, checker /
       hatch / gradient fills, soft shadows, outlined / shadowed text, grips, spinners) is exposed so
       editor / custom widgets can paint them.  Implemented in imgui_draw_symbol.c.

       set_check_style / set_bullet_style / set_arrow_style choose the global indicator shape
       (imgui_check_style_t / imgui_bullet_style_t / imgui_arrow_style_t); scope a change locally with
       push_style_var on IMGUI_VAR_CHECK_STYLE / _BULLET_STYLE / _ARROW_STYLE.

       Pipeline note: draw_gradient is an exact one-quad blend via per-vertex color
       (IMGUI_CMD_RECT_GRADIENT); draw_shadow (layered rings) is still an approximation that a
       future multi-corner-color command would make exact, without changing this surface.  Angles
       for arc / pie / progress are radians, screen-space (y
       down).  `thickness` is the stroke width for the stroked forms. */

    void ( *draw_check_mark        )( imgui_rect_t box, u32 col );
    void ( *draw_arrow             )( imgui_rect_t box, imgui_dir_t dir, u32 col );
    void ( *draw_bullet            )( f32 cx, f32 cy, f32 r, u32 col );
    void ( *draw_close             )( imgui_rect_t box, u32 col );
    void ( *draw_arrow_pointing_at )( f32 tx, f32 ty, f32 half, imgui_dir_t dir, u32 col );
    void ( *draw_chevron           )( imgui_rect_t box, imgui_dir_t dir, f32 thickness, u32 col );
    void ( *draw_plus_minus        )( imgui_rect_t box, bool plus, f32 thickness, u32 col );
    void ( *draw_frame             )( imgui_rect_t box, u32 col_bg, u32 col_border, f32 border );
    void ( *draw_round_rect        )( imgui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                                        bool filled, f32 thickness, u32 col );
    void ( *draw_ngon              )( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, bool filled, f32 thickness, u32 col );
    void ( *draw_circle            )( f32 cx, f32 cy, f32 r, bool filled, f32 thickness, u32 col );
    void ( *draw_arc               )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col );
    void ( *draw_pie               )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col );
    void ( *draw_bezier_quad       )( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_bezier_cubic      )( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_dashed_line       )( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 col );
    void ( *draw_checker           )( imgui_rect_t box, f32 cell, u32 col_a, u32 col_b );
    void ( *draw_hatch             )( imgui_rect_t box, f32 spacing, f32 thickness, u32 col );
    void ( *draw_gradient          )( imgui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
    void ( *draw_shadow            )( imgui_rect_t box, f32 spread, u32 col );
    void ( *draw_text_outline      )( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline );
    void ( *draw_text_shadow       )( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy );
    void ( *draw_grip              )( imgui_rect_t box, u32 col );
    void ( *draw_spinner           )( imgui_rect_t box, f32 t, f32 thickness, u32 col );
    void ( *draw_progress_arc      )( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col );
    void ( *set_check_style          )( u32 style );
    void ( *set_bullet_style         )( u32 style );
    void ( *set_arrow_style          )( u32 style );

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

    /* Debug render mode -- how the main UI draw list is rasterized (imgui_render_mode_t): NORMAL,
       WIREFRAME (triangle edges), or BATCH (per-draw-call color tint).  A pipeline + push-constant
       switch, so it is live in every build (not gated to Debug like the overlay layers above). */
    void                ( *debug_set_render_mode )( imgui_render_mode_t mode );
    imgui_render_mode_t ( *debug_get_render_mode )( void );

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

    /* Hardware cursor.  The widgets already drive the shape from their own hover (resize edges show
       the directional sizers, a text field shows the I-beam).  set_mouse_cursor lets UI code request
       a shape imgui cannot infer -- e.g. APP_CURSOR_HAND over a custom clickable -- for this frame;
       the last request wins and is flushed to the OS window under the pointer while imgui owns the
       mouse, then reset to APP_CURSOR_ARROW next frame.  get_mouse_cursor reads the current request. */
    void         ( *set_mouse_cursor )( app_cursor_t c );
    app_cursor_t ( *get_mouse_cursor )( void );

    /* wants_redraw -- true when at least one animated widget has not yet reached its target this frame.
       The host checks this after the UI build to decide whether to skip the editor sleep: while any
       transition is in flight, the loop must keep pumping frames to advance the animation. */
    bool ( *wants_redraw )( void );

    /* Tables -- a multi-column layout with independent cell clipping and optional scrolling,
       sortable headers, and resizable columns.  Conceptually a grid whose rows accumulate and scroll
       (like flow) with column tracks resolved once per table (like grid), plus frozen header support.

       USAGE CONTRACT:
         1. table_begin()            -- open the table; returns true (always, like child_begin).
                                        Consume it paired with table_end() regardless.
         2. table_setup_column()     -- call ncols times between table_begin and the first row.
                                        The calls may be omitted; all columns default to stretch.
         3. table_headers_row()      -- optional; draws and clips a non-scrolling header strip.
                                        Call after all table_setup_column, before the first data row.
                                        [Phase 1 stub -- no-op until Phase 2 lands.]
         4. for each row:
              table_next_row()       -- begin a new data row.  First call sets row 0.
              for each column:
                table_next_column()  -- advance to the next column and return true; clips draw + hit-
                                        test to the cell.  Returns false past the last column.
                <emit widgets>       -- normal widget calls; they land inside the cell.
         5. table_end()              -- close the table; restores the parent layout.

       Column widths use the overloaded-unit rule (same as columns / grid):
           > 1.0  fixed pixels   1.0  fill / stretch   (0,1)  fraction   0.0  natural (= stretch)
       Height: 0 = auto (8 rows tall), > 0 = fixed pixels.

           if ( imgui()->table_begin( "my_table", 3, IMGUI_TABLE_NONE, 0 ) )
           {
               imgui()->table_setup_column( "Name",  IMGUI_TABLE_COL_STRETCH,  0     );
               imgui()->table_setup_column( "Value", IMGUI_TABLE_COL_FIXED,    80.0f );
               imgui()->table_setup_column( "Unit",  IMGUI_TABLE_COL_FIXED,    40.0f );
               for ( i32 i = 0; i < count; ++i )
               {
                   imgui()->table_next_row( 0 );
                   if ( imgui()->table_next_column() ) imgui()->text( name[i]  );
                   if ( imgui()->table_next_column() ) imgui()->text( value[i] );
                   if ( imgui()->table_next_column() ) imgui()->text( unit[i]  );
               }
               imgui()->table_end();
           }

       table_set_column_index( col ) -- jump to a specific column (0-based) rather than advancing.
       table_get_column_count()      -- number of columns the table was opened with.
       table_get_column_index()      -- current column index (-1 before the first next_column).
       table_get_row_index()         -- current row index (-1 before the first next_row).
       table_get_sort_specs( out )   -- read raw sort state (column + direction); returns true on
                                        the frame a header was clicked.  Use when you want to sort
                                        your own data structure by hand.
       table_sort_order( order, n, val_fn, cmp_fn, user )
                                     -- built-in sort: reorder a display-order index array to match
                                        the active sort.  Pass val_fn for automatic alphabetical /
                                        numeric ordering, or cmp_fn for a custom comparator.  Cheap
                                        to call every frame (only reorders on a header click).
       table_set_bg_color( target, abgr ) -- override the current row's or cell's background. */

    bool ( *table_begin            )( const char* id, i32 ncols, imgui_table_flags_t flags, f32 height );
    void ( *table_end              )( void );
    void ( *table_setup_column     )( const char* label, imgui_table_col_flags_t flags, f32 width );
    void ( *table_headers_row      )( void );
    void ( *table_next_row         )( f32 min_h );
    bool ( *table_next_column      )( void );
    bool ( *table_set_column_index )( i32 col );
    i32  ( *table_get_column_count )( void );
    i32  ( *table_get_column_index )( void );
    i32  ( *table_get_row_index    )( void );
    bool ( *table_get_sort_specs   )( imgui_table_sort_specs_t* out );
    bool ( *table_sort_order       )( i32* order, i32 count, imgui_table_sort_value_fn val_fn,
                                      imgui_table_sort_cmp_fn cmp_fn, void* user );
    void ( *table_set_bg_color     )( imgui_table_bg_target_t target, u32 abgr );

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
