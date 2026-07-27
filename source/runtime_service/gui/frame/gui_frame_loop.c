/*==============================================================================================

    runtime_service/gui/frame/gui_frame_loop.c -- Frame lifecycle and clip helpers.

    Implements the public functions that bracket a frame: init/shutdown, frame_begin/frame_end,
    ctx_begin/ctx_end, render, clip rect push/pop, and the dirty-state queries.  Font
    loading/selection and the font -> layout bridge (gui_style_apply) live in gui_frame_font.c;
    the between-frames commit of deferred font reloads (gui_font_flush_deferred) is a frame_begin
    step and stays here.  Viewport open/resize/close and the gui-owned floater surfaces
    (spawn/update/render_floaters) live in gui_viewport.c, included just after this file; the
    multi-context lifecycle (ctx_create/destroy/bind/set_listening) and the context block
    allocation live in gui_context.c, a sibling in this unit -- the storage they operate on is
    the interact server's (s_ctx_pool, core/gui_ctx.c), but tearing a context down is
    orchestrator work.  The perf / state HUD overlays and the frame-timing helpers the lifecycle
    here calls live in gui_frame_overlay.c, included just before this file.

    Included by the gui_frame.c unit root (the frame orchestrator), which the gui.c module
    face then names in its vtable.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Init / Shutdown
==============================================================================================*/

/* Boot-path seams -- defined in gui_boot.c (same TU, included after this file): teardown of the
   boot-owned window/context from gui_shutdown, and the auto chrome-shell emit at the default
   context's ctx_begin.  Both no-op when the host did not boot(). */

static void boot_shutdown( void );
static void boot_shell_emit( void );

/*==============================================================================================
    Lifecycle contract -- catching host misuse at the seam that names it

    Every rule below fails LATE and silently if it is not checked here: a gui built without a
    device has no pipeline (renders nothing), a build with no font measures every rect at 0 x 0,
    a frame rendered before frame_end replays an unsealed draw list, and a floater freed outside
    viewport_update tears down a surface an in-flight draw list still references.  Each check
    reports the rule it broke and how to satisfy it, once per call site, in EVERY build -- a
    Release host must still learn why its UI is dead -- and traps in Debug so the offending call
    is on the stack.  `cond` is evaluated twice, so it must be side-effect free.
==============================================================================================*/

#define GUI_CONTRACT( cond, ... )                                                   \
    do                                                                              \
    {                                                                               \
        static bool reported_ = false;                                              \
        if ( !reported_ && !( cond ) )                                              \
        {                                                                           \
            reported_ = true;                                                       \
            printf( "[gui] CONTRACT: " __VA_ARGS__ );                               \
            fflush( stdout );        /* flush before the once-assert can trap */    \
            ORB_ASSERT_MSG_ONCE( cond, "gui lifecycle contract violated -- see the "\
                                       "[gui] CONTRACT line above" );               \
        }                                                                           \
    }                                                                               \
    while ( 0 )

/* True between a successful init() and shutdown().  Guards the public entry points that would
   otherwise fault on state init() creates (the context pool, the pipeline, the atlas) rather
   than report the missing init. */
static bool s_gui_ready = false;

/* Where the current frame stands.  The lifecycle is a strict sequence -- build, seal, sync,
   render -- and each step is only safe once the one before it has run; the latch lets each
   entry point name the step the host skipped instead of failing three calls later. */
typedef enum gui_frame_phase_e
{
    GUI_FRAME_IDLE = 0,   /* between frames -- nothing open                                */
    GUI_FRAME_BUILD,      /* frame_begin ran; widgets may emit                             */
    GUI_FRAME_SEALED,     /* frame_end ran; the draw list is final                         */
    GUI_FRAME_SYNCED,     /* viewport_update ran; surfaces reconciled -- safe to render    */

} gui_frame_phase_t;

static gui_frame_phase_t s_frame_phase = GUI_FRAME_IDLE;

/*============================================================================================*/
/* True when at least one rhi render context is live.  init() builds its pipeline, sampler and
   atlas up front, so the device must already exist -- and scanning contexts (rather than asking
   for a device flag) also proves the swapchain-bearing context the host will hand to
   viewport_open is open.  context_size touches no device state, so it is safe to call before
   rhi()->init() has ever run. */

static bool
rhi_context_any_live( void )
{
    for ( i32 i = 0; i < (i32)GUI_MAX_VIEWPORTS; ++i )
        if ( rhi()->context_size( i, NULL, NULL ) )
            return true;
    return false;
}

/*============================================================================================*/

bool
gui_init( gui_builtin_font_t font )
{
    /* Both preconditions produce a gui that runs but paints nothing, so fail here rather than
       leave the host to discover it as a blank window: a second init strands the first one's
       GPU resources and rebinds the context pool under any viewport already open, and an init
       with no live device has nothing to create the pipeline / sampler / atlas from. */

    if ( s_gui_ready )
    {
        printf( "[gui] CONTRACT: init() called twice -- shutdown() before re-initializing.\n" );
        return false;
    }

    if ( !rhi_context_any_live() )
    {
        printf( "[gui] CONTRACT: init() before a live rhi device -- call rhi()->init() and "
                "rhi()->context_open( win ) for the window gui will render into, THEN init(). "
                "Without a device there is no pipeline and every frame renders nothing.\n" );
        return false;
    }

    /* Seed the style base from the default theme before any font init runs; font_load calls
       gui_style_apply which scales s_style_base -- it must be non-zero first. */

    gui_theme_set( "dark" );

    /* wire default context's static backing arrays; sets g_ctx */

    ctx_pool_init();

    /* init: shared pipeline / sampler / atlas + optional layers */

    if ( !backend_init() )
        return false;

    /* The draw unit's resources (fonts + icons) register into the shared atlas the
       server just created -- the orchestrator boots them in dependency order. */
    if ( !gui_draw_boot() )
    {
        backend_exit();
        return false;
    }

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
    if ( dbg_init() == false ) {
         printf( "[gui] WARNING: debug overlay buffers failed; overlay disabled\n" );
    }
#endif

    /* The pipeline dashboard needs no lifecycle here: it is an ordinary GUI_WIN_DEBUG_BAND
       window drawn through the normal pipeline (gui_dashboard.c); the backend keeps only the
       snapshot capture, which owns no GPU resources (render/gui_dash_capture.c). */

    s_gui_ready = true;
    return true;
}

/*============================================================================================*/

void
gui_shutdown( void )
{
    /* Idempotent: a host that tears down after a FAILED init (or twice) would otherwise
       double-free the atlas and the context blocks. */
    if ( !s_gui_ready )
        return;
    s_gui_ready   = false;
    s_frame_phase = GUI_FRAME_IDLE;

    #ifdef GUI_DEBUG_OVERLAY
    dbg_shutdown();
    #endif

    /* Destroy GPU surfaces once -- the one global s_vp_pool (including any gui-owned floaters),
       not per context: a viewport is a real OS window / RHI context, never context-owned. */
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
        viewport_destroy( v );
    gui_draw_shutdown();      /* draw unit resources (fonts + icons) leave the atlas first */
    backend_exit();       /* shared pipeline / sampler / atlas */

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
static u32            s_ctx_save_sp;

/* True when the current frame has any input change, in-flight animation, or render delta from last
   frame.  Computed in frame_begin after io_frame_begin; exposed via gui_frame_dirty().  When false
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

/*============================================================================================*/
/* Upload any (re)loaded fonts' resident pixels into the shared atlas at this between-frames latch.
   The GPU atlas swap (register / upload) is done here, before any context renders, so it never
   interleaves with an in-flight frame.  Returns true when the active font changed -- its metrics
   drive layout, so the caller forces a rebuild this frame. */

static bool
gui_font_flush_deferred( void )
{
    if ( !font_atlas_sync() )
        return false;
    gui_style_apply();          /* active font's metrics changed -> rescale the layout base */
    return true;
}

/*============================================================================================*/
/* Global frame phase: input poll + draw-list reset.  Always reads display dimensions from the
   PRIMARY context (slot 0): the OS window and its viewports belong to the default context
   regardless of which context is active for input this frame.

   This is the global half of the frame; it binds NO context.  Returns true when this frame must
   emit widgets (frame_dirty): open the context scopes and build only then, and always close with
   frame_end -- on a clean (false) frame it replays the volatile widgets internally and render()
   reuses the preserved geometry.  See the FRAME CONTRACT note in gui_api.h. */

bool
gui_frame_begin( f32 dt )
{
    /* No init means no context pool -- every step below dereferences g_ctx. */
    GUI_CONTRACT( s_gui_ready, "frame_begin() before a successful init() -- check the bool "
                               "init() returned; gui has no context pool to build into.\n" );
    if ( !s_gui_ready )
        return false;

    /* Still mid-build means last frame never sealed.  frame_end is unconditional (THE BEGIN /
       END RULE): skipping it strands the volatile replay, the perf clock and the focus latch. */
    GUI_CONTRACT( s_frame_phase != GUI_FRAME_BUILD,
                  "frame_begin() with the previous frame still open -- call frame_end() every "
                  "frame, whatever frame_begin returned.\n" );
    s_frame_phase = GUI_FRAME_BUILD;

    s_ctx_save_sp      = 0;       /* fresh context scope stack; a leaked binding cannot survive a frame */
    s_any_redraw       = false;   /* re-accumulated at each ctx_end from that context's animations */
    s_overlays_emitted = false;   /* debug overlays emit once, at the default context's ctx_end */
    s_shell_emitted    = false;   /* boot chrome shell emits once, at the default context's ctx_begin */

    /* display dimensions: viewports are the one global s_vp_pool, so this needs no context at all.
       The host may resize the OS window at any time, so read the current size from the primary
       (main swapchain) surface, slot 0. */

    i32 disp_w = s_vp_count > 0 ? s_vp_pool[ 0 ].disp_w : 0;
    i32 disp_h = s_vp_count > 0 ? s_vp_pool[ 0 ].disp_h : 0;

    /* Open the perf overlay's emit clock here -- "start at frame_begin" -- and publish last frame's
       measured cost into the smoothed readouts the overlay shows. */

    perf_frame_begin( dt );

    /* caption_inset is NOT cleared here.  Native shell windows republish it during the build, so
       the field always reflects the last frame the shell was active. */

    /* Push last frame's cursor request to the OS BEFORE interact_new_frame promotes the new
       hover_win and io_frame_begin overwrites mouse_viewport -- cursor_flush reads both as the
       previous frame left them, keeping the requested shape and its target window coherent. */

    cursor_flush();

    /* Promote last frame's render-stat accumulator to the published value BEFORE draw_reset, so a
       build that reads render_stats() this frame sees the previous frame's completed totals. */

    build_stats_publish();

    /* Refresh the IO snapshot, computing s_io_dirty as a side-effect. */
    io_frame_begin( disp_w, disp_h, dt );

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
                 || build_any_changed()
                 || STEP_FRAME_PENDING();   /* a latched stepper request (seek/capture/release)
                                               must reach its serving emit; per-context
                                               wants_redraw can be wiped by a later ctx_begin */

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

    /* Push any resource-atlas changes (a font (re)load above, or icons registered since last frame)
       to the GPU at this safe between-frames point.  Unconditional: fonts pack into the shared atlas
       too, so the flush must run even when the icons layer is off.  An upload that actually landed
       forces a rebuild -- register_icon / load_icon live in the draw layer, below core, so they
       cannot raise the flag themselves; the arriving pixels are the signal, and without it art
       registered while the UI is idle stays invisible until the next unrelated input. */
    if ( res_atlas_flush_upload() )
        s_frame_dirty = true;

    if ( s_frame_dirty )
    {
        /* Full rebuild: clear the draw list and tessellation so the emit phase writes fresh
           commands, and reset global interaction state for this frame's hit tests. */
        draw_reset( disp_w, disp_h );
        build_frame_reset();   /* s_frame_built = false; rebuilt on first render() */

        /* Reset global interaction state exactly once per app frame.  hover_win promotion and
           active_id release happen here -- NOT in ctx_new_frame -- so subsequent ctx_begin
           calls for additional contexts do not clobber hover nominations from earlier ones. */
        interact_new_frame();
        drag_new_frame();          /* drag-and-drop lifecycle rides the same once-per-frame reset */
    }
    /* Clean frame: draw_reset / build_frame_reset / interact_new_frame are all
       skipped.  s_draw.cmds is preserved from the previous frame; s_frame_built remains true
       so cache_build_frame returns immediately and reuses the existing s_tess + s_dispatch.
       Interaction state (hover_win, active_id, focused_id) persists unchanged -- the cursor
       has not moved, so last frame's hover is still valid. */

#ifdef GUI_DEBUG_OVERLAY
    dbg_reset();         /* clear the overlay's per-frame geometry (always) */
#endif

    return s_frame_dirty;
}

/*============================================================================================*/
/* Seal the build: every window/context emitted this frame is now final.  The symmetric partner to
   frame_begin -- it latches the emit cost (frame_begin -> frame_end) for the perf overlay and, in
   Debug builds, asserts every ctx_begin was matched by a ctx_end.  Call once after the UI build and
   before any render(); render consumes the sealed draw list.  On a clean frame (frame_begin
   returned false, no widget emit ran) it replays the registered volatile widgets against their
   cached geometry, so a host never wires update_volatile itself. */

void
gui_frame_end( void )
{
    /* Debug hotkeys (overlay tiers, render mode, retained/idle skip) -- polled here, after
       nav_new_frame and all widget emission for the frame, so any key nav/widgets actually used
       (arrow-nav activation, type-ahead, mnemonics) has already been consumed (zeroed) out of the
       IO snapshot; debug_hotkeys only sees keys nothing else claimed this frame.  A press is
       itself an input change, so any mode it flips still lands in a frame io_dirty already marked
       dirty back in frame_begin -- moving this call does not affect that. */
    if ( gui_debug_is_enabled() )
        debug_hotkeys();

    /* Clean frame: no emit ran, the preserved draw list will be replayed verbatim -- patch the
       volatile widgets (gui()->volatile_cb registrations) in place so they keep animating. */
    if ( !s_frame_dirty )
        volatile_update();

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

    /* Clear the one-frame event-borne input (text/wheel/paste) now that the widgets have read it.
       Runs every frame, including clean ones, so it stays cleared before the next ring drain. */
    io_frame_end();

    /* Sealed: the draw list is final.  viewport_update may now free surfaces, and render may
       replay.  Reported (not corrected) when frame_begin never ran -- the phase still advances
       so one stray call cannot cascade into a second complaint from render. */
    GUI_CONTRACT( s_frame_phase == GUI_FRAME_BUILD,
                  "frame_end() without a matching frame_begin() this frame.\n" );
    s_frame_phase = GUI_FRAME_SEALED;
}

/*============================================================================================*/
/* Per-context frame phase: bind `ctx_handle` and run a full frame init for it.  Pushes the context
   bound on entry so the matching ctx_end restores it.  Every context gets the full init (nav, popup
   check, per-frame scratch reset) regardless of its `listening` flag; the flag only gates widget
   interaction (hover nomination and widget hit-tests).  Emit this context's windows immediately
   after the call -- it leaves g_ctx bound to ctx_handle -- and close with ctx_end. */

void
gui_ctx_begin( gui_ctx_id_t ctx_handle )
{
    /* Every widget this context emits lays out off the active font's metrics (s_style, scaled by
       gui_style_apply/metrics_compute) -- with none activated s_style is still zero-initialized and
       everything collapses to zero size (see gui_init's font_valid() gate).  Catch the missing
       font here, at the frame boundary, instead of as an invisible UI downstream: this is the
       first point in the lifecycle where "no font" is knowably wrong (init() with
       GUI_FONT_NONE is legal -- the host may load its own before the first frame). */

    GUI_CONTRACT( font_valid(),
                  "ctx_begin() with no active font -- s_style is still the zero-font base, so "
                  "every widget measures 0 x 0 and the UI paints nothing.  Pass a built-in font "
                  "to init(), or font_load() one before the first frame.\n" );

    /* The build must sit inside the frame: emitted before frame_begin the widgets land in a draw
       list that is about to be reset, emitted after frame_end in one already sealed and rendered. */

    GUI_CONTRACT( s_frame_phase == GUI_FRAME_BUILD,
                  "ctx_begin() outside the build -- the emit belongs between frame_begin() and "
                  "frame_end().\n" );

    if ( ctx_handle < 0 || ctx_handle >= (i32)s_ctx_pool_count || !s_ctx_pool[ ctx_handle ] )
         ctx_handle = GUI_CTX_DEFAULT;

    /* Push the context bound on entry; ctx_end restores it.  Count truthfully past the cap so a
       too-deep nesting still balances against ctx_end (the saved slot just aliases the top). */
    {
        if ( s_ctx_save_sp < GUI_CTX_STACK_DEPTH )
             s_ctx_save_stack[ s_ctx_save_sp ] = g_ctx;

        ++s_ctx_save_sp;
    }

    gui_context_t* c = s_ctx_pool[ ctx_handle ];
    ctx_bind( c );

    g_ctx->retained.wants_redraw = false;    /* cleared before the build; set again by any animating widget */
    ctx_new_frame();                    /* per-context scratch reset + frame clock bump (no global interaction touch) */
    s_layout_sp = 0;                    /* fresh layout stack (the flow unit's) -- no region is open
                                           until a window_begin/child_begin; paired here */
    style_new_frame();                  /* fresh style stacks, re-seeded from the theme -- the orchestrator
                                           pairs the two resets; the interact server knows no style */
    popup_close_check();                /* stale-close + click-outside, BEFORE any user popup_open */
    window_modal_apply();               /* fence interaction behind a GUI_WIN_MODAL window (dev console) */
    popup_apply_modal();                /* fence interaction behind an open modal popup (wins over the above) */
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

/*============================================================================================*/
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

    /* End-of-build dock bookkeeping: recompute each pane's hidden state (all its tabbed windows
       stopped emitting) now that every window's begin has run for this context; a transition sets
       wants_redraw so the collapsed / revived tiling lands next frame (gui_dock_core.c). */
    dock_hidden_refresh();

    /* Captured after the overlay emit so an overlay's own animation (if any) counts too. */
    s_any_redraw |= g_ctx->retained.wants_redraw;

    --s_ctx_save_sp;
    u32 i = s_ctx_save_sp < GUI_CTX_STACK_DEPTH ? s_ctx_save_sp : GUI_CTX_STACK_DEPTH - 1;
    ctx_bind( s_ctx_save_stack[ i ] );   /* NULL (no prior context) rebinds the default */
}

/*============================================================================================*/
/* Flush one viewport's geometry partition to GPU.  The host opens a frame on that viewport's rhi
   context, calls render() with the context cmd, then ends the frame -- once per live viewport.
   The viewport's stored disp_w/h drive the GPU viewport and scissor clamping.
   The debug overlay is also painted when vp == 0 (the primary).  GUI_VP_INVALID is a no-op. */

/* The two ordering rules only a render can catch, both of which produce a working-looking frame:

     - a surface still flagged pending_close means viewport_update() has not run since the build.
       It is the ONLY safe point to free a surface (no in-flight draw list references one there),
       so the floater is instead torn down at some arbitrary later point -- or never.

     - a viewport sized differently from the swapchain it flushes into means the resize reached
       one of gui / rhi but not the other: rhi()->event() must run before (or alongside)
       gui()->event(), so a WIN_RESIZE rebuilds the swapchain and re-sizes the viewport in the
       same drain.  gui laid out for the old surface; the frame renders stretched or clipped. */

static void
render_contract_check( gui_vp_t vp, const gui_viewport_t* v )
{
    if ( s_frame_phase != GUI_FRAME_SYNCED )
    {
        bool pending = false;
        for ( u32 i = 1; i < s_vp_count; ++i )
            pending = pending || ( s_vp_pool[ i ].owned && s_vp_pool[ i ].pending_close );

        GUI_CONTRACT( !pending,
                      "render() with a floater surface still waiting to be freed -- call "
                      "viewport_update() after frame_end() and before render(); it is the only "
                      "safe point to free a surface.\n" );
    }

    /* Owned floaters carry their own rhi context; a host-provided surface uses the slot
       convention (viewport index == win_id == rhi context id). */
    i32 ctx_id = v->owned ? v->rhi_ctx : v->win_id;
    i32 sw = 0, sh = 0;
    if ( ctx_id >= 0 && rhi()->context_size( ctx_id, &sw, &sh )
         && sw > 0 && sh > 0 && v->disp_w > 0 && v->disp_h > 0 )
    {
        GUI_CONTRACT( sw == v->disp_w && sh == v->disp_h,
                      "viewport %u is laid out for %d x %d but its swapchain is %d x %d -- route "
                      "rhi()->event() before (or alongside) gui()->event() so one resize reaches "
                      "both.\n", (u32)vp, v->disp_w, v->disp_h, sw, sh );
    }
}

/*============================================================================================*/

void
gui_render( gui_vp_t vp, rhi_cmd_t cmd )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return;
    gui_viewport_t* v = &s_vp_pool[ vp ];

    /* Rendering an open build replays a draw list the emit is still writing into. */
    GUI_CONTRACT( s_frame_phase != GUI_FRAME_BUILD,
                  "render() before frame_end() -- the draw list is not sealed yet.\n" );
    render_contract_check( vp, v );

    /* Latch the emit time (first render of the frame) and bracket the flush -- "conclude cost at
       render": emit ends here, render time accumulates across every render() call this frame. */
    f64 t0 = perf_render_begin();
    gui_render_flush( v->vb, v->ib, v->target, vp, cmd, v->disp_w, v->disp_h );
#ifdef GUI_DEBUG_OVERLAY
    dbg_flush( vp, cmd, v->disp_w, v->disp_h );   /* each viewport flushes its own rects */
#endif
    perf_render_end( t0 );
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
    Dirty state query
==============================================================================================*/

/* True when at least one gui_anim_f32 channel is still transitioning this frame.  The host
   loop checks this after the build to decide whether to skip the editor-sleep wait: as long as
   any value is mid-animation the host must keep pumping frames, otherwise the transition freezes. */
bool
gui_wants_redraw( void )
{
    return g_ctx->retained.wants_redraw;
}

/* One-shot redraw request: raise the bound context's wants_redraw so the NEXT frame_begin reads
   dirty and runs a full emit.  The public face of the flag every internal pop-time mutation
   already sets -- for host/user-widget state changed DURING a build that only the next build can
   show (a screen switch on a click, a model edit behind gui()->item).  Self-clearing, unlike the
   set_force_redraw pin. */
void
gui_request_redraw( void )
{
    redraw_request();
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
