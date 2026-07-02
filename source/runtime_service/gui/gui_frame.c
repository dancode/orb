/*==============================================================================================

    runtime_service/gui/gui_frame.c -- Frame lifecycle, viewport, font, and clip helpers.

    Implements the public functions that bracket a frame: init/shutdown, frame_begin/frame_end,
    ctx_begin/ctx_end, render, viewport open/resize/close, font loading/selection,
    and clip rect push/pop.
    Included by gui.c before gui_api.c so the vtable can reference these by name.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Init / Shutdown
==============================================================================================*/

/* Backend capability flags latched by gui_init_config(), read by gui_init when it stands up the
   backend.  Defaults to GUI_CAPS_DEFAULT (set below, GUI_CAPS_DEFAULT is a compound literal and
   not a valid static initializer) so a caller that never calls gui_init_config() sees today's
   full-feature behavior unchanged. */
static gui_backend_caps_t s_init_caps = { .icons = true, .retained_cache = true,
                                           .render_debug = true, .stats_trace = false };

/* OPTIONAL: override which backend capability layers this run compiles in.  Call before init();
   a call after init() has no effect (the backend has already latched its own copy).  Skip this
   entirely to accept GUI_CAPS_DEFAULT. */
void
gui_init_config( gui_backend_caps_t caps )
{
    s_init_caps = caps;
}

bool
gui_init( gui_builtin_font_t font )
{
    /* Seed the style base from the default theme before any font init runs; font_load calls
       gui_style_apply which scales s_style_base -- it must be non-zero first. */
    gui_theme_set( "dark" );

    ctx_pool_init();   /* wire default context's static backing arrays; sets g_ctx */

    if ( !gui_backend_init( s_init_caps ) )      /* shared pipeline / sampler / atlas + optional layers */
        return false;

    /* Optional built-in font (gui.h); non-fatal on failure -- init still succeeds, just without
       text, mirroring the debug-overlay init a few lines below. */
    if ( font != GUI_FONT_NONE && !font_load_builtin( font ) )
        printf( "[gui] WARNING: built-in font load failed; continuing without text\n" );

    /* No viewports created here -- the host calls viewport_open() after init() for each OS window.
       Viewports own their own geometry buffers and are opened explicitly before any frames. */

#ifdef GUI_DEBUG_OVERLAY
    /* Debug overlay GPU buffers.  Non-fatal: a failure just leaves the overlay dark. */
    if ( !gui_debug_init() )
        printf( "[gui] WARNING: debug overlay buffers failed; overlay disabled\n" );
#endif
    return true;
}

/*============================================================================================*/

void
gui_shutdown( void )
{
    #ifdef GUI_DEBUG_OVERLAY
    gui_debug_shutdown();
    #endif

    /* Destroy GPU surfaces for every context before releasing memory blocks.
       viewport_destroy is g_ctx-agnostic (takes a pointer), so no rebind is needed.
       Primary context viewports (including any gui-owned floaters) are destroyed first. */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        gui_context_t* ctx = s_ctx_pool[ i ];
        if ( !ctx ) continue;
        for ( u32 v = 0; v < ctx->max_viewports; ++v )
            viewport_destroy( &ctx->viewports[ v ] );
    }
    gui_backend_exit();       /* shared pipeline / sampler / atlas */

    /* Free all context blocks. */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        if ( !s_ctx_pool[ i ] ) continue;
        free( s_ctx_pool[ i ]->_alloc );
        s_ctx_pool[ i ] = NULL;
    }
    g_ctx = NULL;
}

/*==============================================================================================
    Memory Stats
==============================================================================================*/

gui_mem_stats_t
gui_mem_stats( void )
{
    return gui_render_memory();
}

void
gui_print_mem_stats( void )
{
    gui_render_print_memory();
}

/*==============================================================================================
    Performance overlay

    A built-in, hidden-chrome FPS / cost readout (the host used to hand-roll this).  gui owns no
    clock -- it is a leaf of rhi + app -- so the host hands it a monotonic seconds callback through
    perf_overlay(); gui brackets the frame with it.  The emit clock opens at frame_begin and is
    latched on the first render() of the frame; the render clock sums the render() flush calls.  Both
    raw measurements are folded into smoothed (EMA) readouts at the next frame_begin, so the panel
    trails the work it describes by one frame -- the standard self-measurement lag for an in-frame
    overlay (the build that reads the numbers is also the one being measured).
==============================================================================================*/

static struct
{
    gui_clock_fn  clock;              /* host monotonic seconds source (NULL = timing off) */
    f64             t_emit_start;       /* clock() captured at frame_begin (0 = not armed)    */
    f64             emit_ms;            /* this frame: frame_begin -> first render() (ms)     */
    f64             rend_ms;            /* this frame: accumulated render() wall time (ms)    */
    bool            emit_captured;      /* emit_ms latched on the first render() this frame   */
    f32             fps;                /* smoothed readouts shown by the overlay             */
    f32             s_emit_ms;
    f32             s_rend_ms;
} s_perf;

/* Publish last frame's raw emit/render times into the smoothed readouts and open a fresh emit clock.
   Called from frame_begin (which owns dt -> fps). */
static void
perf_frame_begin( f32 dt )
{
    if ( dt > 0.0f )
    {
        f32 inst = 1.0f / dt;
        s_perf.fps = s_perf.fps <= 0.0f ? inst : s_perf.fps * 0.92f + inst * 0.08f;
    }
    f32 em = (f32)s_perf.emit_ms;
    f32 rm = (f32)s_perf.rend_ms;
    s_perf.s_emit_ms = s_perf.s_emit_ms <= 0.0f ? em : s_perf.s_emit_ms * 0.9f + em * 0.1f;
    s_perf.s_rend_ms = s_perf.s_rend_ms <= 0.0f ? rm : s_perf.s_rend_ms * 0.9f + rm * 0.1f;

    s_perf.emit_ms       = 0.0;
    s_perf.rend_ms       = 0.0;
    s_perf.emit_captured = false;
    s_perf.t_emit_start  = s_perf.clock ? s_perf.clock() : 0.0;
}

/* Close the emit phase at frame_end -- "build cost = frame_begin -> frame_end".  Latches emit_ms
   from the clock armed in perf_frame_begin; idempotent (the first capture this frame wins, so the
   render fallback below is a no-op once this has run). */
static void
perf_frame_end( void )
{
    if ( !s_perf.clock )
        return;
    if ( !s_perf.emit_captured && s_perf.t_emit_start > 0.0 )
    {
        s_perf.emit_ms       = ( s_perf.clock() - s_perf.t_emit_start ) * 1000.0;
        s_perf.emit_captured = true;
    }
}

/* Close the emit phase and return the clock reading to bracket the render flush.  frame_end normally
   latches emit_ms first; this remains as a fallback so timing still reads if frame_end was skipped.
   Returns 0 when timing is off (clock not yet supplied or t_emit_start unarmed). */
static f64
perf_render_begin( void )
{
    if ( !s_perf.clock )
        return 0.0;
    f64 now = s_perf.clock();
    if ( !s_perf.emit_captured && s_perf.t_emit_start > 0.0 )
    {
        s_perf.emit_ms       = ( now - s_perf.t_emit_start ) * 1000.0;
        s_perf.emit_captured = true;
    }
    return now;
}

/* Fold one render() flush span into this frame's render accumulator (t0 from perf_render_begin). */
static void
perf_render_end( f64 t0 )
{
    if ( s_perf.clock && t0 > 0.0 )
        s_perf.rend_ms += ( s_perf.clock() - t0 ) * 1000.0;
}

gui_id_t g_gui_perf_overlay_id = 0;

void
gui_perf_overlay( gui_clock_fn clock, int mode )
{
    /* Adopt the host clock so frame_begin / render can time emit + render (effective next frame). */
    s_perf.clock = clock;

    if ( mode <= 0 )
        return;

    f32 fps = s_perf.fps;

    /* Hide the window: transparent body + outline, fixed top-left, hugging its content. */
    gui_push_style_color( GUI_COL_WINDOW_BG, GUI_COLOR( 0, 0, 0, 0 ) );
    gui_push_style_color( GUI_COL_BORDER,    GUI_COLOR( 0, 0, 0, 0 ) );

    f32 top_y = 8.0f;
    gui_window_t* mb = window_find( id_hash( "##MainMenuBar" ) );
    if ( mb && mb->last_frame == g_ctx->retained.frame )
        top_y += mb->h;

    gui_window_set_next_pos( 8.0f, top_y, GUI_COND_ALWAYS );
    
    g_gui_perf_overlay_id = id_hash( "perf_overlay" );
    gui_window_begin( "perf_overlay", GUI_WIN_OVERLAY );
    {
        gui_stack();
        /* FPS, graded by health: >=60 green, >=30 amber, else red. */
        u32 fps_col = fps >= 60.0f ? GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
                    : fps >= 30.0f ? GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
                    :                GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF );
        char line[ 64 ];
        snprintf( line, sizeof( line ), "FPS %5.1f  (%4.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f );
        gui_text_colored( fps_col, line );

        if ( mode >= 2 )
        {
            gui_spacing( 2.0f );
            gui_textf( "emit   %5.2f ms", s_perf.s_emit_ms );
            gui_textf( "render %5.2f ms", s_perf.s_rend_ms );
        }

        if ( mode >= 3 )
        {
            gui_render_stats_t rs = gui_render_stats();
            gui_spacing( 2.0f );
            gui_textf( "verts   %6u", rs.vert_count );
            gui_textf( "tris    %6u", rs.tri_count  );
            gui_textf( "batches %6u", rs.draw_calls );
            gui_textf( "cmds    %6u", rs.cmd_count  );

            if ( mode >= 4 )
            {
                /* Retained-mode stats: how much geometry was reused vs re-tessellated. */
                gui_spacing( 2.0f );
                gui_textf( "wins ret  %u/%u", rs.win_retained,  rs.win_total   );
                gui_textf( "verts ret %u/%u", rs.vert_retained, rs.vert_count  );
                gui_textf( "tris ret  %u/%u", rs.tri_retained,  rs.tri_count   );

                /* Upload stats: GPU memory bandwidth. */
                gui_spacing( 2.0f );
                gui_textf( "up batch  %u", rs.upload_batches );
                gui_textf( "up bytes  %u", rs.upload_bytes   );
            }
        }
    }
    gui_window_end();

    gui_pop_style_color( 2 );
}

/*==============================================================================================
    Frame API
==============================================================================================*/

/* Context save stack -- ctx_begin pushes the context bound on entry, ctx_end pops and rebinds it,
   so begin/end nests as a balanced scope exactly like window_begin/window_end.  Reset to empty each
   frame in frame_begin, so an unbalanced previous frame cannot leak a binding into this one. */
#define GUI_CTX_STACK_DEPTH 8
static gui_context_t* s_ctx_save_stack[ GUI_CTX_STACK_DEPTH ];
static u32              s_ctx_save_sp;

/* True when the current frame has any input change, in-flight animation, or render delta from last
   frame.  Computed in frame_begin after input_update; exposed via gui_frame_dirty().  When false
   the host may skip ctx_begin / widget emit / ctx_end entirely and call render() directly -- the
   previous frame's draw list and tessellation are preserved and reused unchanged. */
static bool s_frame_dirty = true;   /* start true: forces a full first-frame build */

/* Global frame phase: input poll + draw-list reset.  Always reads display dimensions from the
   PRIMARY context (slot 0): the OS window and its viewports belong to the default context
   regardless of which context is active for input this frame.

   This is the global half of the frame; it binds NO context.  Open at least one context with
   ctx_begin(GUI_CTX_DEFAULT) before emitting any window, close it with ctx_end, and seal the
   build with frame_end.  See the FRAME CONTRACT note in gui_api.h. */

/* Commit any font (re)loads deferred from last frame's build at this between-frames latch.  The
   GPU atlas swap (create/upload/register + deferred destroy of the old atlas) is done here, before
   any context renders, so it never interleaves with an in-flight frame.  Returns true when the
   active font changed -- its metrics drive layout, so the caller forces a rebuild this frame. */
static bool
gui_font_flush_deferred( void )
{
    if ( !font_flush_pending() )
        return false;
    gui_style_apply();          /* active font's metrics changed -> rescale the layout base */
    return true;
}

void
gui_frame_begin( f32 dt )
{
    s_ctx_save_sp = 0;   /* fresh context scope stack; a leaked binding cannot survive a frame */

    gui_context_t* primary = s_ctx_pool[ 0 ];   /* default ctx always owns the OS window */
    i32 disp_w = primary->viewport_count > 0 ? primary->viewports[ 0 ].disp_w : 0;
    i32 disp_h = primary->viewport_count > 0 ? primary->viewports[ 0 ].disp_h : 0;

    /* Open the perf overlay's emit clock here -- "start at frame_begin" -- and publish last frame's
       measured cost into the smoothed readouts the overlay shows. */
    perf_frame_begin( dt );

    /* caption_inset is NOT cleared here.  Native shell windows republish it during the build, so
       the field always reflects the last frame the shell was active. */

    /* Push last frame's cursor request to the OS BEFORE interaction_frame_reset promotes the new
       hover_win and input_update overwrites mouse_viewport -- cursor_flush reads both as the
       previous frame left them, keeping the requested shape and its target window coherent. */
    cursor_flush();

    /* Promote last frame's render-stat accumulator to the published value BEFORE draw_reset, so a
       build that reads render_stats() this frame sees the previous frame's completed totals. */
    gui_render_stats_publish();

    /* Refresh the IO snapshot, computing s_io_dirty as a side-effect. */
    input_update( disp_w, disp_h, dt );

    /* Frontend dirty: true when the frame must emit widgets.
         - io_dirty          : any input change this frame (mouse move/button/key/wheel/text)
         - wants_redraw      : an animation was in flight last frame and must advance this frame
                               (wants_redraw is cleared at ctx_begin, so at frame_begin it still
                               holds the value set during last frame's emit -- "was mid-animation")
         - render_any_changed: last frame's diff found a change (new/removed/moved window), so
                               the frame has not yet reached a stable cached state
       When false: the host may skip ctx_begin / widget emit / ctx_end.  The draw list and
       tessellation from the previous frame are preserved and replayed verbatim. */
    s_frame_dirty = io_dirty()
                 || s_retained.wants_redraw
                 || gui_render_any_changed();

    /* Debug overlay capture runs every emit, so any active layer forces a full build. */
    #ifdef GUI_DEBUG_OVERLAY
    if ( gui_debug_get_layers() )
        s_frame_dirty = true;
    #endif

    /* Commit deferred font (re)loads at this safe between-frames point -- always, since the host
       can request a load between frames independent of the widget emit.  A committed swap changes
       glyph geometry, so it forces a full rebuild this frame. */
    if ( gui_font_flush_deferred() )
        s_frame_dirty = true;

    /* Push any icons registered since last frame to the GPU -- every frame the icons layer is on,
       since host code can register icons between frames independent of the widget emit. */
    if ( s_init_caps.icons )
        icon_atlas_flush_upload();

    if ( s_frame_dirty )
    {
        /* Full rebuild: clear the draw list and tessellation so the emit phase writes fresh
           commands, and reset global interaction state for this frame's hit tests. */
        draw_reset( disp_w, disp_h );
        gui_render_frame_reset();   /* s_frame_built = false; rebuilt on first render() */

        /* Reset global interaction state exactly once per app frame.  hover_win promotion and
           active_id release happen here -- NOT in ctx_new_frame -- so subsequent ctx_begin
           calls for additional contexts do not clobber hover nominations from earlier ones. */
        interaction_frame_reset();
    }
    /* Clean frame: draw_reset / gui_render_frame_reset / interaction_frame_reset are all
       skipped.  s_draw.cmds is preserved from the previous frame; s_frame_built remains true
       so cache_build_frame returns immediately and reuses the existing s_tess + s_dispatch.
       Interaction state (hover_win, active_id, focused_id) persists unchanged -- the cursor
       has not moved, so last frame's hover is still valid. */

#ifdef GUI_DEBUG_OVERLAY
    gui_debug_reset();         /* clear the overlay's per-frame geometry (always) */
#endif
}

/* Per-context frame phase: bind `ctx_handle` and run a full frame init for it.  Pushes the context
   bound on entry so the matching ctx_end restores it.  Every context gets the full init (nav, popup
   check, per-frame scratch reset) regardless of its `listening` flag; the flag only gates widget
   interaction (hover nomination and widget hit-tests).  Emit this context's windows immediately
   after the call -- it leaves g_ctx bound to ctx_handle -- and close with ctx_end. */
void
gui_ctx_begin( gui_ctx_t ctx_handle )
{
    if ( ctx_handle < 0 || ctx_handle >= (i32)s_ctx_pool_count || !s_ctx_pool[ ctx_handle ] )
        ctx_handle = GUI_CTX_DEFAULT;

    /* Push the context bound on entry; ctx_end restores it.  Count truthfully past the cap so a
       too-deep nesting still balances against ctx_end (the saved slot just aliases the top). */
    if ( s_ctx_save_sp < GUI_CTX_STACK_DEPTH )
        s_ctx_save_stack[ s_ctx_save_sp ] = g_ctx;
    ++s_ctx_save_sp;

    gui_context_t* c = s_ctx_pool[ ctx_handle ];
    ctx_bind( c );

    s_retained.wants_redraw = false;    /* cleared before the build; set again by any animating widget */
    ctx_new_frame();                    /* per-context scratch reset + frame clock bump (no global interaction touch) */
    popup_close_check();                /* stale-close + click-outside, BEFORE any user popup_open */
    popup_apply_modal();                /* fence interaction behind an open modal (steals hover_win) */
    window_raise_on_press();            /* a press raises the hover window (takes effect this frame) */
    nav_new_frame();                    /* commit last frame's nav move + read this frame's nav keys */
}

/* Close the context opened by the matching ctx_begin, rebinding the context that was current before
   it.  The symmetric partner to ctx_begin -- it removes the need to hand-restore the default with
   ctx_bind after emitting a secondary context's windows. */
void
gui_ctx_end( void )
{
    if ( s_ctx_save_sp == 0 )
        return;   /* unbalanced ctx_end -- ignore rather than underflow */

    --s_ctx_save_sp;
    u32 i = s_ctx_save_sp < GUI_CTX_STACK_DEPTH ? s_ctx_save_sp : GUI_CTX_STACK_DEPTH - 1;
    ctx_bind( s_ctx_save_stack[ i ] );   /* NULL (no prior context) rebinds the default */
}

/* Seal the build: every window/context emitted this frame is now final.  The symmetric partner to
   frame_begin -- it latches the emit cost (frame_begin -> frame_end) for the perf overlay and, in
   Debug builds, asserts every ctx_begin was matched by a ctx_end.  Call once after the UI build and
   before any render(); render consumes the sealed draw list. */
void
gui_frame_end( void )
{
    /* Build cost concludes here: latch emit_ms for the perf overlay (render is timed separately). */
    perf_frame_end();

    /* A leftover context scope means a ctx_begin without its ctx_end -- catch it at the seam rather
       than letting the stale binding bleed into render or the next frame. */
    ORB_ASSERT( s_ctx_save_sp == 0 );

    /* Focus departure: if focused_id changed during this frame (a click moved focus, or Enter /
       Escape cleared it), latch the departing widget and its edit flag for one frame so
       is_item_deactivated_after_edit can read them on the NEXT frame's emission of that widget.
       If focus did not change, clear the ended slot so it does not linger past the valid frame. */
    if ( s_interaction.focused_id != s_interaction.focused_id_at_frame_start )
    {
        s_interaction.focus_ended_id     = s_interaction.focused_id_at_frame_start;
        s_interaction.focus_ended_edited = s_interaction.focused_id_edited;
        s_interaction.focused_id_edited  = false;
    }
    else
    {
        s_interaction.focus_ended_id     = GUI_ID_NONE;
        s_interaction.focus_ended_edited = false;
    }
}

/* Flush one viewport's geometry partition to GPU.  The host opens a frame on that viewport's rhi
   context, calls render() with the context cmd, then ends the frame -- once per live viewport.
   The viewport's stored disp_w/h drive the GPU viewport and scissor clamping.
   The debug overlay is also painted when vp == 0 (the primary).  GUI_VP_INVALID is a no-op. */
void
gui_render( gui_vp_t vp, rhi_cmd_t cmd )
{
    if ( vp < 0 || vp >= (i32)g_ctx->max_viewports )
        return;
    gui_viewport_t* v = &g_ctx->viewports[ vp ];

    /* Latch the emit time (first render of the frame) and bracket the flush -- "conclude cost at
       render": emit ends here, render time accumulates across every render() call this frame. */
    f64 t0 = perf_render_begin();
    gui_render_flush( v, (u32)vp, cmd, v->disp_w, v->disp_h );
#ifdef GUI_DEBUG_OVERLAY
    gui_debug_flush( vp, cmd, v->disp_w, v->disp_h );   /* each viewport flushes its own rects */
#endif
    perf_render_end( t0 );
}

/*==============================================================================================
    Viewport API

    Open a viewport: claim the slot at win_id (slot index == win_id), create its GPU geometry
    buffers, and record the OS window and initial drawable size.

    Slot alignment: win_id 0 = primary swapchain, win_id 1..N = secondary surfaces.  
    Since each viewport requires a live OS window, the window pool guarantees the matching slot is free.
    RHI_SWAPCHAIN_COLOR resolves per-context at flush time -- which cmd you pass render() selects
    the swapchain.

    Returns the handle to pass to render / viewport_resize / viewport_close / window_set_next_viewport,
    or GUI_VP_INVALID on bad win_id or GPU buffer failure.
    Must be called after init() and before frame_begin().

==============================================================================================*/

gui_vp_t
gui_viewport_open( i32 win_id )
{
    /* Slot index == win_id; an open window guarantees the slot is free. */
    if ( win_id < 0 || win_id >= (i32)g_ctx->max_viewports )
        return GUI_VP_INVALID;

    gui_viewport_t* vp = &g_ctx->viewports[ win_id ];
    ORB_ASSERT( !rhi_handle_valid( vp->vb ) );   /* slot must be free */

    if ( !viewport_create( vp, ( rhi_texture_t ){ .id = RHI_SWAPCHAIN_COLOR }, win_id ) )
        return GUI_VP_INVALID;

    /* Query current window size from app() -- avoids the host passing redundant w/h. */
    i32 w = 0, h = 0;
    app()->window_get_size( win_id, &w, &h );
    vp->disp_w = w;
    vp->disp_h = h;

    // Update the high-water viewport count so the host can enumerate live viewports.
    if ( (u32)win_id + 1u > g_ctx->viewport_count )
        g_ctx->viewport_count = (u32)win_id + 1u;

    return (gui_vp_t)win_id;
}

/* Update a viewport's drawable size.  Call on OS resize BEFORE frame_begin.
   Works identically for the primary (0) and secondary viewports.  GUI_VP_INVALID is a no-op. */
void
gui_viewport_resize( gui_vp_t vp, i32 w, i32 h )
{
    if ( vp < 0 || vp >= (i32)g_ctx->max_viewports )
        return;

    g_ctx->viewports[ vp ].disp_w = w;
    g_ctx->viewports[ vp ].disp_h = h;
}

/* Close a viewport and release its GPU geometry buffers.  Works for the primary (0) and secondary
   viewports alike.  Windows assigned to the closed viewport revert to the primary.  The host owns
   the OS window and rhi context; gui owns only the geometry buffers. */
void
gui_viewport_close( gui_vp_t vp )
{
    if ( vp < 0 || vp >= (i32)g_ctx->max_viewports )
        return;
    viewport_destroy( &g_ctx->viewports[ vp ] );
    /* Migrate any windows on this surface back to the primary. */
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].viewport == (u32)vp )
            s_windows[ i ].viewport = 0;
    /* Trim the high-water viewport count when the closed slot was at the top. */
    while ( g_ctx->viewport_count > 0
            && !rhi_handle_valid( g_ctx->viewports[ g_ctx->viewport_count - 1 ].vb ) )
        --g_ctx->viewport_count;
}

/*==============================================================================================
    Owned-floater lifecycle (gui-owned surfaces)

    Unlike viewport_open (the host hands gui a window + context to flush into), these own the OS
    window + rhi context end to end: gui creates them on spawn and destroys them on close.  The
    tear-off gesture (Phase 3) drives spawn/close; a host/sandbox may also call gui_viewport_spawn
    directly to place a panel in its own OS window.

    viewport_spawn is defined here (not render.c) because it picks a slot from g_ctx->viewports and
    bumps viewport_count -- and g_ctx lives in gui_ctx.c, included after render.c.  render.c's
    viewport_create / viewport_destroy stay g_ctx-agnostic (they take a vp pointer) for that reason.
==============================================================================================*/

/* Create a NEW gui-owned floater surface: OS window + its rhi context (swapchain) + per-surface
   geometry buffers.  The window's win_id doubles as the viewport slot index (APP_WIN_MAX ==
   GUI_MAX_VIEWPORTS, so the id is always a valid slot, and the window pool guarantees it is
   free) -- preserving the slot == win_id invariant the input router relies on.  Returns the
   viewport index, or GUI_VP_INVALID on any failure (each step unwinds the previous). */
static gui_vp_t
viewport_spawn( const char* title, i32 x, i32 y, i32 w, i32 h, bool no_activate )
{
    /* OS window first -- its win_id is the viewport slot index.  no_activate (set for a mid-drag
       tear-off) opens the floater with APP_WIN_NOFOCUS so it does NOT steal foreground from the
       origin window -- on Windows, activating another top-level window releases that window's
       mouse capture, which would sever the in-flight drag the moment the floater appeared. */
    /* Owned floaters are native-borderless: a detached panel owns its OS window and acts as that
       window's frame (window_begin treats any window on an owned viewport as GUI_WIN_NATIVE), so
       the OS drives its move / resize / snap.  no_activate (mid-drag tear-off) adds APP_WIN_NOFOCUS
       so spawning does not steal foreground and sever the origin window's mouse capture. */
    u32 open_flags = APP_WIN_BORDERLESS | ( no_activate ? APP_WIN_NOFOCUS : 0u );
    i32 win_id = app()->window_open( title, x, y, w, h, open_flags );
    if ( win_id == APP_WIN_INVALID )
        return GUI_VP_INVALID;
    if ( win_id < 0 || win_id >= (i32)g_ctx->max_viewports )
    {
        app()->window_close( win_id );    /* no viewport slot for this id */
        return GUI_VP_INVALID;
    }

    gui_viewport_t* vp = &g_ctx->viewports[ win_id ];
    ORB_ASSERT( !rhi_handle_valid( vp->vb ) );   /* slot must be free (slot == win_id) */

    /* This window's own render context (swapchain) -- context_open queries handle+size from app(). */
    i32 ctx = rhi()->context_open( win_id );
    if ( ctx == RHI_CTX_INVALID )
    {
        app()->window_close( win_id );
        return GUI_VP_INVALID;
    }

    /* Per-surface geometry buffers; RHI_SWAPCHAIN_COLOR resolves to this ctx's image at flush. */
    if ( !viewport_create( vp, ( rhi_texture_t ){ .id = RHI_SWAPCHAIN_COLOR }, win_id ) )
    {
        rhi()->context_destroy( ctx );
        app()->window_close( win_id );
        return GUI_VP_INVALID;
    }

    vp->rhi_ctx = ctx;
    vp->owned   = true;    /* gui created the window + context -> gui destroys them */
    vp->disp_w  = w;
    vp->disp_h  = h;

    if ( (u32)win_id + 1u > g_ctx->viewport_count )
        g_ctx->viewport_count = (u32)win_id + 1u;
    return (gui_vp_t)win_id;
}

/* Public spawn: open an gui-owned floater hosting its own OS window at (x,y) sized w x h.
   Returns the viewport handle to assign windows to (window_set_next_viewport), or
   GUI_VP_INVALID.  Must be called between frames (it creates an OS window + rhi context). */

gui_vp_t
gui_viewport_spawn( const char* title, i32 x, i32 y, i32 w, i32 h )
{
    return viewport_spawn( title, x, y, w, h, false );
}

/* Service an OS resize/close event for any gui-known viewport (delegated from gui_event, which
   cannot see the viewport pool from input.c).

   For WIN_RESIZE: updates the matching viewport's drawable size.  rhi()->event() handles the
   swapchain rebuild -- gui no longer calls rhi()->context_resize() here.
   For WIN_CLOSE:  marks an owned floater for teardown at the next viewport_update.

   Returns true (consumed) only when win_id is an gui-owned floater, so the host's close-to-quit
   path and rhi()->event() still fire for the primary (host-owned) window. */

static bool
gui_owned_window_event( const app_event_t* ev )
{
    /* Walk all live viewports (index 0 = primary, 1+ = secondary/owned). */
    for ( u32 i = 0; i < g_ctx->viewport_count; ++i )
    {
        gui_viewport_t* vp = &g_ctx->viewports[ i ];
        if ( vp->win_id != ev->win_id )
            continue;
        if ( !rhi_handle_valid( vp->vb ) )
            continue;   /* slot not live */

        if ( ev->type == APP_EV_WIN_RESIZE )
        {
            vp->disp_w       = ev->data.win_resize.w;
            vp->disp_h       = ev->data.win_resize.h;
            s_viewport_dirty = true;   /* layout must recompute for the new surface size */
            /* Owned floaters: gui owns the window+context, so consume the event.
               Primary viewport: return false -- rhi()->event() also needs to rebuild the swapchain
               and the host may want to track the size for other purposes. */
            return vp->owned;
        }
        else if ( ev->type == APP_EV_WIN_CLOSE && vp->owned )
        {
            vp->pending_close = true;   /* torn down at the next viewport_update */
            return true;               /* consumed: gui owns this window's close lifecycle */
        }
        break;   /* found the viewport; primary close falls through to host */
    }
    return false;
}

/* Reconcile gui-owned floater surfaces with their OS windows.  Call once per frame after the UI
   build and BEFORE rendering: it is the safe point to tear surfaces down, since no in-flight draw
   list references one being freed.  Today it destroys surfaces the user closed (pending_close);
   Phase 3 will also service tear-off / merge-back requests enqueued during the build. */

void
gui_viewport_update( void )
{
    /* (1) Tear-off / merge-back: a window whose title was dragged off its host surface (enqueued by
       window_begin_ex) changes which surface hosts it. */
    if ( s_vp_request.active )
    {
        s_vp_request.active     = false;
        gui_window_t* win     = window_find( s_vp_request.win_id );
        bool            has_home = s_vp_request.has_home;
        s_vp_request.has_home   = false;   /* one-shot: never leak into a later drag tear-off */
        if ( win && s_vp_request.from_vp == 0 )
        {
            /* Tear the window off the main surface into its own floater.  Placement depends on how
               the tear-off was requested:

               by_drag -- a live title-bar drag (button still down).  Place the floater so the grab
               point stays exactly beneath the cursor: client origin = screen cursor - the grab offset
               recorded when the drag began.  Spawned no-activate so the origin window keeps its OS
               mouse capture (activating would release it and sever the drag); the per-frame
               floater-follow in window_begin then keeps the panel tracking the cursor.

               else -- a detach-button click, no drag in flight.  Keep the panel at its EXACT screen
               position (main client origin + the panel's position within it), so it pops out in place
               rather than jumping to the cursor.  Activate normally; there is no capture to preserve. */

            i32 sx, sy;
            if ( s_vp_request.by_drag )
            {
                i32 cx = 0, cy = 0;
                app()->mouse_position_screen( &cx, &cy );
                sx = cx - (i32)s_drag_off_x;
                sy = cy - (i32)s_drag_off_y;
            }
            else if ( has_home )
            {
                /* Re-opening a closed floater: land at the saved RESTORE (normal) position. */
                sx = win->home_x;
                sy = win->home_y;
            }
            else
            {
                i32 mx = 0, my = 0;
                app()->window_get_pos( g_ctx->viewports[ 0 ].win_id, &mx, &my );
                sx = mx + (i32)win->x;
                sy = my + (i32)win->y;
            }

            /* Re-open spawns at the saved restore size so the OS restore rect is the previous normal
               size; a plain tear-off spawns at the window's current size. */
            i32 sw = has_home ? (i32)win->restore_w : (i32)win->w;
            i32 sh = has_home ? (i32)win->restore_h : (i32)win->h;

            gui_vp_t vp = viewport_spawn( s_vp_request.title ? s_vp_request.title : "panel",
                                            sx, sy, sw, sh, s_vp_request.by_drag );
            if ( vp != GUI_VP_INVALID )
            {
                /* window_open positions the FRAME; set_pos lands the CLIENT corner on (sx,sy). */
                app()->window_set_pos( g_ctx->viewports[ vp ].win_id, sx, sy );
                win->viewport = (u32)vp;
                win->x        = 0.0f;
                win->y        = 0.0f;

                /* Re-maximize a floater that was closed maximized: spawned at the restore rect first
                   (above), so the OS restore target becomes the previous normal size. */
                if ( has_home && win->reopen_maximized )
                    app()->window_maximize( g_ctx->viewports[ vp ].win_id );
            }
        }
        else if ( win )
        {
            /* Merge back into the main surface.  Placement mirrors tear-off:

               by_drag -- capture is held by the main window for the whole drag, so s_io.mouse_x/y are
               already main-client coords; the panel lands at cursor - grab offset, continuous with the
               in-flight drag, which the attached drag-apply in window_begin then carries on.

               else -- a button click; keep the panel at the screen location the floater occupied (its
               client origin minus the main client origin), so it docks in place rather than jumping. */
            u32 fvp = s_vp_request.from_vp;

            win->viewport = 0;

            /* Clamp the window to fit inside the host surface on any pop-in path.  Size first so
               that position clamping below always has a non-negative travel range.  A panel that was
               fullscreened while floating must not land with resize handles off-screen.
               Skipped for GUI_WIN_NO_BOUNDARY_CLAMP -- placement is externally managed. */
            if ( !( win->flags & GUI_WIN_NO_BOUNDARY_CLAMP ) )
            {
                const gui_viewport_t* hv = &g_ctx->viewports[ 0 ];
                f32 dw    = vp_w( hv );
                f32 dh    = vp_h( hv );
                f32 top   = hv->caption_inset;
                f32 max_h = dh - top; if ( max_h < 0.0f ) max_h = 0.0f;
                if ( win->w > dw )    win->w = dw;
                if ( win->h > max_h ) win->h = max_h;
            }

            if ( s_vp_request.by_drag )
            {
                win->x = s_io.mouse_x - s_drag_off_x;
                win->y = s_io.mouse_y - s_drag_off_y;
            }
            else
            {
                i32 fx = 0, fy = 0, mx = 0, my = 0;
                if ( fvp < g_ctx->max_viewports )
                    app()->window_get_pos( g_ctx->viewports[ fvp ].win_id, &fx, &fy );
                app()->window_get_pos( g_ctx->viewports[ 0 ].win_id, &mx, &my );
                win->x = (f32)( fx - mx );
                win->y = (f32)( fy - my );

                /* Snap fully inside the host's client bounds: a floater merged from well clear of the
                   main window would otherwise land at a screen offset outside the visible area (the
                   button path never runs the per-frame window_clamp the drag path relies on).
                   Skipped for GUI_WIN_NO_BOUNDARY_CLAMP -- caller is responsible for placement. */
                if ( !( win->flags & GUI_WIN_NO_BOUNDARY_CLAMP ) )
                {
                    const gui_viewport_t* hv = &g_ctx->viewports[ 0 ];
                    f32 dw  = vp_w( hv );
                    f32 dh  = vp_h( hv );
                    f32 top = hv->caption_inset;
                    f32 max_x = dw - win->w;
                    f32 max_y = dh - win->h; if ( max_y < top ) max_y = top;
                    win->x = win->x < 0.0f ? 0.0f : ( win->x > max_x ? max_x : win->x );
                    win->y = win->y < top  ? top  : ( win->y > max_y ? max_y : win->y );
                }
            }

            bool empty = true;
            for ( u32 w = 0; w < s_window_count; ++w )
                if ( s_windows[ w ].viewport == fvp ) { empty = false; break; }
            if ( empty && fvp > 0 && fvp < g_ctx->max_viewports && g_ctx->viewports[ fvp ].owned )
                viewport_destroy( &g_ctx->viewports[ fvp ] );
        }
    }

    /* (2) Tear down owned floaters for either reason:

         pending_close -- the user closed the OS window (APP_EV_WIN_CLOSE).

         abandoned     -- the window(s) the floater hosts stopped being emitted.  A floater is just a
                          surface for the panel that was torn into it; if the caller hides that panel
                          or quits drawing it, its window_begin stops running and last_frame freezes,
                          leaving a hovering OS window with no UI and no controls.  Detect it by the
                          same staleness rule popups use (popup_close_check): the freshest assigned
                          window has missed a full frame (max last_frame + 1 < frame), or no window is
                          bound at all.  One frame of grace tolerates a transient single-frame hide.

       This runs after step (1), so a window just torn off / merged this frame already carries
       last_frame == s_retained.frame on its new surface and never reads as abandoned. */
    for ( u32 i = 1; i < g_ctx->viewport_count; ++i )
    {
        gui_viewport_t* vp = &g_ctx->viewports[ i ];
        if ( !vp->owned )
            continue;

        bool abandoned = false;
        if ( !vp->pending_close )
        {
            /* Freshest emit among windows bound to this surface; no bound window stays abandoned. */
            u32  max_lf = 0u;
            bool any    = false;
            for ( u32 w = 0; w < s_window_count; ++w )
                if ( s_windows[ w ].viewport == i )
                {
                    any = true;
                    if ( s_windows[ w ].last_frame > max_lf )
                        max_lf = s_windows[ w ].last_frame;
                }
            abandoned = !any || ( max_lf + 1u < s_retained.frame );
        }

        if ( !( vp->pending_close || abandoned ) )
            continue;

        /* Windows assigned to this surface revert to the primary, then free the surface
           (viewport_destroy drains the GPU, frees buffers, destroys the ctx, closes the window).
           Reverting lets a panel re-emitted later reappear in the main window. */
        for ( u32 w = 0; w < s_window_count; ++w )
            if ( s_windows[ w ].viewport == i )
                s_windows[ w ].viewport = 0;
        viewport_destroy( vp );
    }

    /* Compact the high-water viewport count after any teardowns. */
    while ( g_ctx->viewport_count > 0
            && !rhi_handle_valid( g_ctx->viewports[ g_ctx->viewport_count - 1 ].vb ) )
        --g_ctx->viewport_count;
}

/* Present every gui-owned floater surface from the shared draw list: open a frame on the
   floater's own rhi context, clear, replay that viewport's partition, end.  The main surface
   (index 0, host-owned) is presented by the host via render(); this loop handles only the surfaces
   gui spawned, so a single-window host stays a single-window present loop and tear-off "just
   works".  A minimized floater is skipped (its frame_begin would hand back an invalid cmd). */
void
gui_viewport_render_floaters( void )
{
    for ( u32 viewport_id = 1; viewport_id < g_ctx->viewport_count; ++viewport_id )
    {
        gui_viewport_t* vp = &g_ctx->viewports[ viewport_id ];
        if ( !vp->owned || vp->rhi_ctx == RHI_CTX_INVALID )
            continue;
        if ( app()->window_is_minimized( vp->win_id ) )
            continue;

        rhi_cmd_t cmd = rhi()->frame_begin( vp->rhi_ctx );
        if ( !rhi_cmd_valid( cmd ) )
            continue;

        /* Clear to the window background so the panel composites over a fresh surface (a floater
           is just a UI surface; without the clear, dragging within it smears -- hall-of-mirrors). */
        rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
            .texture  = { .id = RHI_SWAPCHAIN_COLOR },   /* resolves to this ctx's swapchain image */
            .load_op  = RHI_LOAD_OP_CLEAR,
            .store_op = RHI_STORE_OP_STORE,
            .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
        }, 1, NULL );
        rhi()->cmd_end_rendering( cmd );

        gui_render( (gui_vp_t)viewport_id, cmd );
        rhi()->frame_end( vp->rhi_ctx );
    }
}

/*==============================================================================================
    Font API
==============================================================================================*/

/* The font registry lives in the render backend unit; this UI-unit API drives it through the
   font_load / font_use accessors (gui_backend.h) and rebuilds layout from the active font's
   metrics (font_em / font_char_h / font_line_h) -- the font -> layout bridge. */

/* Saved active-font ids for push_font / pop_font; small fixed depth -- font pushes are coarse
   (a section or one widget), not deeply nested. */
#define GUI_FONT_STACK_MAX 8
static u32 s_font_stack[ GUI_FONT_STACK_MAX ];
static u32 s_font_stack_depth = 0;

/* Rebuild layout metrics from whatever font is now active. */
void
gui_style_apply( void )
{
    layout_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h() );
}

u32
gui_font_load( const char* path )
{
    u32 id = font_load( path );     // loads into a new id and activates it
    if ( id == 0 )
        return 0;
    gui_style_apply();
    draw_set_font( font_active_id() );   // load also activates -> retag the atlas batch context
    return id;
}

bool
gui_font_load_into( u32 id, const char* path )
{
    /* font_load_into defers a swap of an already-loaded slot to the next frame_begin (see the
       reload queue in gui_font_internal.c); layout follows there, via gui_font_flush_deferred, once the
       new metrics are live.  Nothing to rescale here -- the slot still shows its current font. */
    return font_load_into( id, path );
}

void
gui_font_use( u32 id )
{
    font_use( id );
    gui_style_apply();
    /* The active font is also the per-segment atlas batch context: cut a new draw segment so the
       tessellator re-activates this font for the span and its glyphs / fills / dashes sample the
       right atlas.  font_use ignores a bad id, so tag with whatever is actually active now. */
    draw_set_font( font_active_id() );
}

void
gui_push_font( u32 id )
{
    if ( s_font_stack_depth < GUI_FONT_STACK_MAX )
        s_font_stack[ s_font_stack_depth++ ] = font_active_id();
    gui_font_use( id );
}

void
gui_pop_font( void )
{
    if ( s_font_stack_depth == 0 )
        return;
    gui_font_use( s_font_stack[ --s_font_stack_depth ] );
}

/*==============================================================================================
    Clip API
==============================================================================================*/

void
gui_push_clip( f32 x, f32 y, f32 w, f32 h )
{
    draw_push_clip_rect( x, y, w, h );
}

void
gui_pop_clip( void )
{
    draw_pop_clip_rect();
}

/*==============================================================================================
    Animation state query
==============================================================================================*/

/* True when at least one gui_anim_f32 channel is still transitioning this frame.  The host
   loop checks this after the build to decide whether to skip the editor-sleep wait: as long as
   any value is mid-animation the host must keep pumping frames, otherwise the transition freezes. */
bool
gui_wants_redraw( void )
{
    return s_retained.wants_redraw;
}

/* True when the current frame must perform a full widget emit.  Computed in frame_begin as the OR
   of three signals: io_dirty (any input change), wants_redraw (in-flight animation from last frame),
   and render_any_changed (last frame's diff found a structural change).  When false the host may
   skip ctx_begin / widget emit / ctx_end entirely -- the previous frame's draw list, tessellated
   geometry, and GPU draw commands are preserved and replayed verbatim by render(). */
bool
gui_frame_dirty( void )
{
    return s_frame_dirty;
}

/*==============================================================================================
    Multi-context API
==============================================================================================*/

/* Set whether a context listens for hover/click/nav input.  Call between frames.
   Multiple contexts may listen simultaneously; a deaf context renders but returns inert
   widget state.  The default context starts listening; secondary contexts start deaf. */
void
gui_ctx_set_listening( gui_ctx_t ctx, bool listen )
{
    if ( ctx >= 0 && ctx < GUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        s_ctx_pool[ ctx ]->listening = listen;
}

/* Allocate a fresh secondary context sized to `cfg` (NULL = editor defaults).
   Each gets a unique id_salt so same-named widgets do not alias across contexts.
   Returns GUI_CTX_INVALID when the pool is full.  Call between frames. */
gui_ctx_t
gui_ctx_create( const gui_ctx_config_t* cfg )
{
    /* Resolve config: NULL or zero fields fall back to editor defaults.
       max_dock_nodes == 0 is valid (disables docking); do not default it. */
    gui_ctx_config_t c = cfg ? *cfg : GUI_CTX_CONFIG_EDITOR;
    if ( !c.max_windows   ) c.max_windows   = GUI_DEFAULT_MAX_WINDOWS;
    if ( !c.state_slots   ) c.state_slots   = GUI_DEFAULT_STATE_SLOTS;
    if ( !c.popup_depth   ) c.popup_depth   = GUI_DEFAULT_POPUP_DEPTH;
    if ( !c.max_viewports ) c.max_viewports = GUI_DEFAULT_MAX_VIEWPORTS;

    /* state_slots must be a power of two for the hash mask to work. */
    u32 slots = c.state_slots;
    if ( slots < 16 ) slots = 16;
    u32 p = 1;
    while ( p < slots ) p <<= 1;
    slots = p;

    /* Find a free pool slot (1..GUI_CTX_POOL_MAX-1). */
    i32 slot = -1;
    for ( i32 i = 1; i < GUI_CTX_POOL_MAX; ++i )
        if ( !s_ctx_pool[ i ] ) { slot = i; break; }
    if ( slot < 0 ) return GUI_CTX_INVALID;

    gui_context_t* ctx = ctx_alloc_slot( &c, slots, slot );
    if ( !ctx ) return GUI_CTX_INVALID;
    ctx->listening = false;   /* secondary contexts start deaf; caller opts in */

    s_ctx_pool[ slot ] = ctx;
    if ( (u32)slot >= s_ctx_pool_count ) s_ctx_pool_count = (u32)slot + 1u;
    return (gui_ctx_t)slot;
}

/* Free a secondary context; rebinds the default if this was current.  Never destroys slot 0. */
void
gui_ctx_destroy( gui_ctx_t ctx )
{
    if ( ctx <= 0 || ctx >= GUI_CTX_POOL_MAX || !s_ctx_pool[ ctx ] )
        return;
    gui_context_t* c = s_ctx_pool[ ctx ];
    if ( g_ctx == c ) ctx_bind( NULL );
    /* Destroy any GPU surfaces the context opened before releasing its memory block. */
    for ( u32 i = 0; i < c->max_viewports; ++i )
        viewport_destroy( &c->viewports[ i ] );
    if ( c->_alloc ) free( c->_alloc );
    s_ctx_pool[ ctx ] = NULL;
}

/* Make ctx the current context.  GUI_CTX_DEFAULT (0) or an invalid handle rebinds the default. */
void
gui_ctx_bind( gui_ctx_t ctx )
{
    if ( ctx >= 0 && ctx < GUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        ctx_bind( s_ctx_pool[ ctx ] );
    else
        ctx_bind( NULL );
}

// clang-format on
/*============================================================================================*/
