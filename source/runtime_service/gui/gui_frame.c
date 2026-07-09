/*==============================================================================================

    runtime_service/gui/gui_frame.c -- Frame lifecycle, font, and clip helpers.

    Implements the public functions that bracket a frame: init/shutdown, frame_begin/frame_end,
    ctx_begin/ctx_end, render, font loading/selection, clip rect push/pop, and the
    animation-state query.  Viewport open/resize/close and the gui-owned floater surfaces
    (spawn/update/render_floaters) live in gui_viewport.c, included just after this file; memory
    stats and the multi-context lifecycle (ctx_create/destroy/bind/set_listening) live in
    0_foundation/gui_ctx.c, next to the context pool they operate on.  The perf / state HUD
    overlays and the frame-timing helpers the lifecycle here calls live in gui_frame_overlay.c,
    included just before this file.
    Included by gui.c before gui_api.c so the vtable can reference these by name.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Init / Shutdown
==============================================================================================*/

/* Backend capability flags latched by gui_init_config_back(), read by gui_init when it stands up
   the backend.  Defaults to GUI_CAPS_DEFAULT (set below, GUI_CAPS_DEFAULT is a compound literal
   and not a valid static initializer) so a caller that never calls gui_init_config_back() sees
   the full-feature defaults unchanged. */

static gui_backend_caps_t s_init_caps = { .icons = true, .retained_cache = true,
                                           .render_debug = true, .stats_trace = false };

/* Boot-path seams -- defined in gui_boot.c (same TU, included after this file): teardown of the
   boot-owned 4_window/context from gui_shutdown, and the auto chrome-shell emit at the default
   context's ctx_begin.  Both no-op when the host did not boot(). */
static void boot_shutdown( void );
static void boot_shell_emit( void );

/* OPTIONAL: override which backend capability layers this run compiles in.  Call before init();
   a call after init() has no effect (the backend has already latched its own copy).  Skip this
   entirely to accept GUI_CAPS_DEFAULT. */

void
gui_init_config_back( gui_backend_caps_t caps )
{
    s_init_caps = caps;
}

/* OPTIONAL: override which UI-unit feature boundaries this run compiles in (gui_forward_caps_t,
   gui.h) -- tables, keyboard_nav.  s_fwd_caps lives at the top of gui.c (not here) so every tier
   file below it in the unity build can read it directly; this just overwrites that copy.  Call
   before init() (or before the first frame, at latest -- unlike the backend caps, nothing here is
   latched into a one-time GPU setup, so a call any time before the first affected code path runs
   is safe).  Skip this entirely to accept GUI_FORWARD_CAPS_DEFAULT. */

void
gui_init_config_front( gui_forward_caps_t caps )
{
    s_fwd_caps = caps;
}

bool
gui_init( gui_builtin_font_t font )
{
    /* Seed the style base from the default theme before any font init runs; font_load calls
       gui_style_apply which scales s_style_base -- it must be non-zero first. */

    gui_theme_set( "dark" );

    /* wire default context's static backing arrays; sets g_ctx */

    ctx_pool_init();

    /* init: shared pipeline / sampler / atlas + optional layers */

    if ( !gui_backend_init( s_init_caps ) )      
        return false;

    /* Optional built-in font (gui.h); non-fatal on failure -- init still succeeds, just without
       text, mirroring the debug-overlay init a few lines below.  font_load_builtin activates the
       font in the backend but -- unlike the public gui_font_load/gui_font_use wrappers -- does not
       rescale layout itself, so gui_style_apply() is called explicitly here; its own font_valid()
       guard makes this correct either way -- a real font rescales s_style, GUI_FONT_NONE (or a
       failed load) leaves it at the zero-font values gui_theme_set seeded above until the caller's
       own font_load() activates one. */

    if ( font != GUI_FONT_NONE && font_load_builtin( font ) == false ) {
         printf( "[gui] WARNING: built-in font load failed; continuing without text\n" );
    }

    gui_style_apply();

    /* No viewports created here -- the host calls viewport_open() after init() for each OS window.
       Viewports own their own geometry buffers and are opened explicitly before any frames. */

#ifdef GUI_DEBUG_OVERLAY
    /* Debug overlay GPU buffers.  Non-fatal: a failure just leaves the overlay dark. */
    if ( gui_debug_init() == false ) {
         printf( "[gui] WARNING: debug overlay buffers failed; overlay disabled\n" );
    }
#endif

    /* The pipeline dashboard needs no lifecycle here: it is an ordinary GUI_WIN_DEBUG_BAND
       window drawn through the normal pipeline (gui_dashboard.c); the backend keeps only the
       snapshot capture, which owns no GPU resources (backend/gui_dash_capture.c). */

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

    /* Boot-owned surface last: the viewport GPU buffers above are gone, now release the
       swapchain context and OS window boot() created.  No-op on the explicit path. */
    boot_shutdown();
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

/* Debug override: when true, frame_dirty is pinned true every frame, defeating the clean-frame
   skip so the UI rebuilds and re-renders unconditionally.  Toggled via gui()->set_force_redraw. */
static bool s_force_redraw = false;

/* Once-per-frame latch for the internal debug-overlay emit at the default context's ctx_end. */
static bool s_overlays_emitted = false;

/* Once-per-frame latch for the boot-path chrome-shell emit at the default context's ctx_begin. */
static bool s_shell_emitted = false;

/* Global frame phase: input poll + draw-list reset.  Always reads display dimensions from the
   PRIMARY context (slot 0): the OS window and its viewports belong to the default context
   regardless of which context is active for input this frame.

   This is the global half of the frame; it binds NO context.  Returns true when this frame must
   emit widgets (frame_dirty): open the context scopes and build only then, and always close with
   frame_end -- on a clean (false) frame it replays the volatile widgets internally and render()
   reuses the preserved geometry.  See the FRAME CONTRACT note in gui_api.h. */

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

bool
gui_frame_begin( f32 dt )
{
    s_ctx_save_sp      = 0;       /* fresh context scope stack; a leaked binding cannot survive a frame */
    s_any_redraw       = false;   /* re-accumulated at each ctx_end from that context's animations */
    s_overlays_emitted = false;   /* debug overlays emit once, at the default context's ctx_end */
    s_shell_emitted    = false;   /* boot chrome shell emits once, at the default context's ctx_begin */

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
    gui_build_stats_publish();

    /* Refresh the IO snapshot, computing s_io_dirty as a side-effect. */
    input_update( disp_w, disp_h, dt );

    /* Debug hotkeys (overlay tiers, render mode, retained/idle skip) -- polled from the fresh IO
       snapshot while debug_enable is on.  A press is itself an input change, so any mode it flips
       lands in a frame that io_dirty already marks for a full emit. */
    if ( gui_debug_is_enabled() )
        debug_hotkeys();

    /* Frontend dirty: true when the frame must emit widgets.
         - io_dirty          : any input change this frame (mouse move/button/key/wheel/text)
         - wants_redraw      : an animation was in flight last frame and must advance this frame
                               (wants_redraw is cleared at ctx_begin, so at frame_begin it still
                               holds the value set during last frame's emit -- "was mid-animation")
         - render_any_changed: last frame's diff found a change (new/removed/moved window), so
                               the frame has not yet reached a stable cached state
       When false: the host may skip ctx_begin / widget emit / ctx_end.  The draw list and
       tessellation from the previous frame are preserved and replayed verbatim. */
    s_frame_dirty = s_force_redraw
                 || io_dirty()
                 || g_ctx->retained.wants_redraw
                 || gui_build_any_changed();

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
        gui_build_frame_reset();   /* s_frame_built = false; rebuilt on first render() */

        /* Reset global interaction state exactly once per app frame.  hover_win promotion and
           active_id release happen here -- NOT in ctx_new_frame -- so subsequent ctx_begin
           calls for additional contexts do not clobber hover nominations from earlier ones. */
        interaction_frame_reset();
        drag_new_frame();          /* drag-and-drop lifecycle rides the same once-per-frame reset */
    }
    /* Clean frame: draw_reset / gui_build_frame_reset / interaction_frame_reset are all
       skipped.  s_draw.cmds is preserved from the previous frame; s_frame_built remains true
       so cache_build_frame returns immediately and reuses the existing s_tess + s_dispatch.
       Interaction state (hover_win, active_id, focused_id) persists unchanged -- the cursor
       has not moved, so last frame's hover is still valid. */

#ifdef GUI_DEBUG_OVERLAY
    gui_debug_reset();         /* clear the overlay's per-frame geometry (always) */
#endif

    return s_frame_dirty;
}

/* Per-context frame phase: bind `ctx_handle` and run a full frame init for it.  Pushes the context
   bound on entry so the matching ctx_end restores it.  Every context gets the full init (nav, popup
   check, per-frame scratch reset) regardless of its `listening` flag; the flag only gates widget
   interaction (hover nomination and widget hit-tests).  Emit this context's windows immediately
   after the call -- it leaves g_ctx bound to ctx_handle -- and close with ctx_end. */
void
gui_ctx_begin( gui_ctx_id_t ctx_handle )
{
    /* Every widget this context emits lays out off the active font's metrics (s_style, scaled by
       gui_style_apply/layout_compute) -- with none activated s_style is still zero-initialized and
       everything collapses to zero size (see gui_init's font_valid() gate).  Catch the missing
       font here, at the frame boundary, instead of as an invisible UI downstream. */
    ORB_ASSERT( font_valid() );

    if ( ctx_handle < 0 || ctx_handle >= (i32)s_ctx_pool_count || !s_ctx_pool[ ctx_handle ] )
        ctx_handle = GUI_CTX_DEFAULT;

    /* Push the context bound on entry; ctx_end restores it.  Count truthfully past the cap so a
       too-deep nesting still balances against ctx_end (the saved slot just aliases the top). */
    if ( s_ctx_save_sp < GUI_CTX_STACK_DEPTH )
        s_ctx_save_stack[ s_ctx_save_sp ] = g_ctx;
    ++s_ctx_save_sp;

    gui_context_t* c = s_ctx_pool[ ctx_handle ];
    ctx_bind( c );

    g_ctx->retained.wants_redraw = false;    /* cleared before the build; set again by any animating widget */
    ctx_new_frame();                    /* per-context scratch reset + frame clock bump (no global interaction touch) */
    popup_close_check();                /* stale-close + click-outside, BEFORE any user popup_open */
    popup_apply_modal();                /* fence interaction behind an open modal (steals hover_win) */
    window_raise_on_press();            /* a press raises the hover window (takes effect this frame) */
    nav_new_frame();                    /* commit last frame's nav move + read this frame's nav keys */

    /* Boot-path chrome: when boot() owns a borderless main window, its shell is emitted here --
       first in the default context's build, so the caption band it publishes is live for every
       window after it.  Once per frame (mirrors the s_overlays_emitted latch at ctx_end); a
       no-op for explicit-path hosts, who emit viewport_shell themselves. */
    if ( g_ctx == s_ctx_pool[ 0 ] && !s_shell_emitted )
    {
        s_shell_emitted = true;
        boot_shell_emit();
    }
}

/* Close the context opened by the matching ctx_begin, rebinding the context that was current before
   it.  The symmetric partner to ctx_begin -- it removes the need to hand-restore the default with
   ctx_bind after emitting a secondary context's windows.

   Two internal duties run here, while the closing context is still bound (the exact point a host
   used to hand-place them, last in the context's build):
     - fold this context's animation state into the frame-wide s_any_redraw for frame_pace
     - emit the debug overlays (perf/state/dashboard) into the DEFAULT context when debug is on */
void
gui_ctx_end( void )
{
    if ( s_ctx_save_sp == 0 )
        return;   /* unbalanced ctx_end -- ignore rather than underflow */

    /* Once per frame: a host that re-opens the default context in the same frame must not get a
       second (duplicate-window) overlay emit. */
    if ( gui_debug_is_enabled() && g_ctx == s_ctx_pool[ 0 ] && !s_overlays_emitted )
    {
        s_overlays_emitted = true;
        debug_overlays_emit();
    }

    /* Captured after the overlay emit so an overlay's own animation (if any) counts too. */
    s_any_redraw |= g_ctx->retained.wants_redraw;

    --s_ctx_save_sp;
    u32 i = s_ctx_save_sp < GUI_CTX_STACK_DEPTH ? s_ctx_save_sp : GUI_CTX_STACK_DEPTH - 1;
    ctx_bind( s_ctx_save_stack[ i ] );   /* NULL (no prior context) rebinds the default */
}

/* Seal the build: every 4_window/context emitted this frame is now final.  The symmetric partner to
   frame_begin -- it latches the emit cost (frame_begin -> frame_end) for the perf overlay and, in
   Debug builds, asserts every ctx_begin was matched by a ctx_end.  Call once after the UI build and
   before any render(); render consumes the sealed draw list.  On a clean frame (frame_begin
   returned false, no widget emit ran) it replays the registered volatile widgets against their
   cached geometry, so a host never wires update_volatile itself. */
void
gui_frame_end( void )
{
    /* Clean frame: no emit ran, the preserved draw list will be replayed verbatim -- patch the
       volatile widgets (gui()->volatile_cb registrations) in place so they keep animating. */
    if ( !s_frame_dirty )
        gui_update_volatile();

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
    if ( vp >= g_ctx->max_viewports )
        return;
    gui_viewport_t* v = &g_ctx->viewports[ vp ];

    /* Latch the emit time (first render of the frame) and bracket the flush -- "conclude cost at
       render": emit ends here, render time accumulates across every render() call this frame. */
    f64 t0 = perf_render_begin();
    gui_render_flush( v, vp, cmd, v->disp_w, v->disp_h );
#ifdef GUI_DEBUG_OVERLAY
    gui_debug_flush( vp, cmd, v->disp_w, v->disp_h );   /* each viewport flushes its own rects */
#endif
    perf_render_end( t0 );
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

/* Rebuild layout metrics from whatever font is now active.  A safe no-op before any font has
   activated (font_valid() false) -- s_style just stays at its last computed value (zero-init
   pre-first-font).  Every caller (theme reset, init, font load/use, deferred-reload flush) can
   call this unconditionally and trust it to do the right thing either way. */
void
gui_style_apply( void )
{
    if ( !font_valid() )
        return;
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
    return g_ctx->retained.wants_redraw;
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

/* Debug override: pin frame_dirty true every frame, defeating the retained-cache clean-frame skip
   so the UI rebuilds and re-renders unconditionally.  Use to isolate a "did not update until input
   moved" symptom from a genuine emit bug -- if the symptom vanishes with this on, a missing
   wants_redraw signal (not the emit) is the culprit. */
void
gui_set_force_redraw( bool on )
{
    s_force_redraw = on;
}

bool
gui_force_redraw( void )
{
    return s_force_redraw;
}

// clang-format on
/*============================================================================================*/
