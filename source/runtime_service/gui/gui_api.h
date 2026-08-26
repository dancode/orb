#ifndef GUI_API_H
#define GUI_API_H
/*==============================================================================================

    runtime_service/gui/gui_api.h -- gui module API struct and gateway macro.
    Always statically linked into the host.

    This is the phone book: every function the GUI exposes, gathered into one big struct
    (gui_api_t) so the rest of the engine can call gui()->whatever(...) without knowing which
    internal unit actually implements it. gui.h (included above this file) defines the nouns
    those functions take and return; this file is the verbs. It adds no logic of its own -- it
    is purely the declaration of that struct, plus the macro that turns it into a working
    module gateway.

    The sections below are listed in the order you would actually use them once a frame is
    open, from the lowest-level stratum to the highest, and every one of them is reached either
    through the gui() vtable or as a direct gui_* call:

        GUI_FRAME    lifecycle: boot, frame phases, pacing, viewports, contexts, events
        GUI_DRAW     render server: the draw_* primitive set, fonts, icons, paths, clips
        GUI_CORE     interaction server: item(), ids, io queries, anim, drag and drop
        GUI_SURFACE  root surfaces: raw pane / region blocks + the feat_* chrome mechanisms
        GUI_RECT     stateless carve math
        GUI_FLOW     the layout pen
        GUI_STYLE    style service: stacks, slot reads, density scale
        GUI_STOCK    comp_* widget logic + the stock_* reference renders over it
        GUI_CHROME   OPTIONAL policy layer: windows, dock, popups, flow widgets, themes
        GUI_DEBUG    severable diagnostics

    Everything below GUI_CHROME works fine without it -- chrome (windows, docking, popups) is
    just one consumer of the lower strata, not something the rest of the GUI depends on. A game
    or tool can build its whole UI directly out of frame + draw + core + surface + rect/flow and
    define its own look, skipping chrome entirely; sb_gui_base is the working proof of that,
    built up one tier at a time.

    THE BEGIN / END RULE -- one rule, no exceptions, for every begin/end pair in this header
    (frame, ctx, window, child, region, pane, popup, modal, tooltip, combo, listbox, menu, menu
    bar, toolbar dropdown, tab bar, tab item, table, flow, layout, split, drag source/target):

        THE BOOL GATES THE BODY, NEVER THE END. Whatever a begin() returns, its matching end()
        is always safe to call -- it cleans up exactly what that begin() opened, and does
        nothing when it opened nothing. That means you never have to remember which pairs need
        special handling; the pattern is always the same:

            if ( gui()->window_begin( "Tools", GUI_WIN_NONE ) )
            {
                ...body widgets...           // skipped when false -- they cost nothing
            }
            gui()->window_end();             // safe whatever begin returned

    A false return just means "do not draw the body" -- the window is collapsed, closed, an
    unselected tab, a popup that is not open. It says nothing about what state the begin() set
    up behind the scenes. That is exactly why the end() must never be placed inside the `if`:
    a begin() that opens some internal bookkeeping and then reports "no body this time" (an
    auto-sizing popup still mid-measurement, a window that is closing this very frame) would
    leave that bookkeeping stranded forever if its end() never ran. Writing the end() outside
    the guard, as shown above, is the one form that can never get this wrong. Older call sites
    in this codebase sometimes place the end() inside the guard anyway and are still correct --
    but only because their end() happens to be a no-op on exactly the paths that would matter.

==============================================================================================*/

#include "runtime_service/gui/gui.h"

#include "engine/app/app.h" /* app_event_t for event()   */
#include "engine/mod/mod_import.h"

/* forward declare so the API can take a cmd argument without including rhi_api.h */
struct rhi_cmd_s; typedef struct rhi_cmd_s* rhi_cmd_t;

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct gui_api_s
{
    /*============================================================================================================
        GUI_FRAME -- lifecycle  (frame/)
        The door and the conductor: init/boot, frame phases, pacing, viewports, contexts, event routing,
        memory / render stats -- Owns per-frame ordering; no widgets.
        Every section below emits inside the frame scope this one opens.
    =============================================================================================================*/

    /* Diagnostics sink -- catch every message gui logs (pool overflows, load failures, contract
       violations, the stats dumps) with a callback of your own instead of letting gui print
       straight to stdout.

       gui deps are { rhi, app }, deliberately not core, so core's LOG_* macros are out of reach
       from inside gui; this hook is how the two meet without gui taking the dependency.  A
       typical host binds it to its own logger in one line:

           static void host_gui_log( gui_log_level_t lvl, const char* msg, void* user )
           { (void)user; LOG_INFO( "[gui] %s", msg ); }        // or switch on lvl
           gui()->log_set_fn( host_gui_log, NULL );

       Install BEFORE init() to catch the init-path diagnostics.  Passing NULL restores the
       default sink (printf + fflush), which is also what an unwired gui uses -- so this is
       optional, and skipping it leaves the pre-hook behavior exactly as it was.  The message is
       formatted, NUL-terminated, carries no "[gui] " prefix and no trailing newline, and is
       valid only for the duration of the call.  See log/gui_log.h for the full contract. */

    void                ( *log_set_fn )         ( gui_log_fn fn, void* user );

    /* Runtime font baker -- the door on-demand font sizes come through.  gui ships no bake for
       an arbitrary pixel size, so the font resolver asks this callback to produce one whenever
       no shipped bake serves a request (the DPI retarget, the type ramp's SMALL/LARGE roles,
       font_get).  dev_font_get is the canonical implementation; the gui target stays free of
       the developer tier the same way it stays free of core above.  Install any time -- before
       or after init(); NULL uninstalls.  Unwired, requests degrade to the nearest shipped size
       (warn-once) and the type ramp stays off for unshipped sizes. */

    void                ( *font_baker_set )     ( gui_font_bake_fn fn, void* user );

    /* GPU resource lifecycle -- the boot and teardown calls every host makes exactly once.

        init()
            : call after rhi()->init(); creates pipeline, font atlas, GPU buffers.
              `family` optionally boots a managed font (gui_font_family_t, gui.h) into
              slot 0 at `size_px` (0 = 16): a shipped bake if one matches, else the
              installed baker, else the nearest shipped size.  Pass GUI_FONT_NONE to
              load nothing and call font_load() yourself.  A failed resolve is
              non-fatal (a warning; init still succeeds without text).

        shutdown()
            : call before rhi()->shutdown(); destroys all GPU resources.

        asset_path()
            : resolve `relative` (e.g. "assets/icon/foo.png") against sys_root_dir() --
              the build root, one level above the executable -- the same convention
              load_icon and the font resolver resolve through. Writes the resolved
              path into `out` (out_size bytes); for a caller that wants the absolute
              path itself (e.g. a plain fopen) rather than a load_icon call.

        font_load()
            : load a pre-baked .orb_font atlas into a new font id and make it active;
              call after init(). Returns the new id (>= 1), or 0 on failure.

        font_get()
            : resolve a font by REQUEST -- source name + pixel size -- without
              activating it.  `family` is a file in assets/font_source, an OS-installed
              face name, or a shipped file stem; the resolver finds a shipped bake,
              asks the installed baker, or degrades to the nearest in-family size
              (warn-once), never below the default font.  IMMEDIATE-MODE retention:
              call it every frame the font is in use (steady-state = a memo probe) and
              apply the id with font_use / push_font -- the request IS the hold, like
              any other per-frame widget state.  A font not requested for an emitted
              frame goes stale and may be reclaimed under registry pressure; requesting
              it again reloads from the bake cache.

        font_get_builtin()
            : font_get for a curated family (gui_font_family_t) -- no name plumbing
              at the call site. */

    bool                ( *init      )          ( gui_font_family_t family, u32 size_px );
    void                ( *shutdown  )          ( void );
    void                ( *asset_path )         ( const char* relative, char* out, int out_size );

    u32                 ( *font_load )          ( const char* path );
    u32                 ( *font_get )           ( const char* family, u32 size_px );
    u32                 ( *font_get_builtin )   ( gui_font_family_t fam, u32 size_px );

    /* DPI response (gui_dpi_mode_t, gui.h) -- keeps text and widgets a sensible physical size on
       a scaled monitor. The engine works in physical pixels, so without this a UI on a 200%
       display renders crisp but tiny. When enabled, gui resolves the managed family at
       base_size * scale every frame; em-driven layout does the rest.  Exact with a runtime
       baker installed, else granularity = the family's shipped sizes.

        dpi_set()
            : select the response mode.  AUTO follows EACH surface's own monitor scale
              (app window_dpi_scale of its hosting window, tracked live across monitor moves /
              OS scale edits -- mixed-DPI monitors get per-surface sizes); MANUAL applies
              `scale` (0.5..4, clamped) everywhere; OFF pins the authored size.
              `scale` <= 0 keeps the previous manual factor.  Cheap and idempotent --
              a host may push it every frame (e.g. from a cvar).
        dpi_mode()
            : the current response mode.
        dpi_scale()
            : the scale in effect on the PRIMARY surface -- its landed size / the init()
              base size.  1.0 while unmanaged (GUI_FONT_NONE init).  During a host font
              takeover (font_use / a custom load) it keeps reporting the last landed scale --
              retargeting is suspended, not reset. */

    void                ( *dpi_set   )          ( gui_dpi_mode_t mode, f32 scale );
    gui_dpi_mode_t      ( *dpi_mode  )          ( void );
    f32                 ( *dpi_scale )          ( void );

    /* Full memory footprint currently held by gui, in bytes: GPU buffers + atlases, the fixed CPU
       backend buffers, and the per-context heap blocks -- see gui_mem_stats_t (gui.h) for the
       bucket breakdown.  print_mem_stats() dumps the same breakdown to stdout as a table. */

    gui_mem_stats_t     ( *mem_stats       )    ( void );
    void                ( *print_mem_stats )    ( void );

    /* Per-frame render statistics (geometry + batch counts) for the LAST completed frame.
       Published at frame_begin, so a read during the build reflects the previous frame -- the
       standard one-frame lag.  Feeds an FPS / performance overlay without re-deriving counts. */

    gui_render_stats_t  ( *render_stats )( void );


    /* A host on the BOOT PATH (see that band, after the frame lifecycle below) calls boot()
       instead of this block -- it runs exactly these calls, in this order, from one descriptor.
       The runtime host does its own setup here and never boots. */


    /* NOTE: the built-in perf overlay, state overlay, and pipeline dashboard are emitted by gui
       itself, not by host code.  debug_enable( true ) arms an internal hotkey driver ('.' -- main
       row or numpad -- arms the group, then F9 / F10 ...) and gui emits them last in the default
       context's build -- see debug_enable (GUI_DEBUG section).  The perf overlay's clock arrives
       once through frame_set_hooks. */

    /* Frame hooks -- hand gui a few OS services it has no way to reach on its own (gui links
       only app + rhi, not sys), one time, right after init():

         clock       -- monotonic seconds source (sys_tick_seconds); brackets the frame for the
                        perf overlay's emit / render cost readouts.  NULL leaves timing at zero.
         sleep_ms    -- thread sleep (sys_sleep_milliseconds); boot_pace's spin/animation sleep.
         wait_events -- block until OS input or timeout (sys_wait_for_os_events_ms); enables the
                        idle-skip path of boot_pace.  NULL disables idle skip entirely. */

    void ( *frame_set_hooks )( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events );

    /* Frame lifecycle -- the four calls that run every single frame, in order, no matter which
       host is driving gui. BOTH host paths call these verbs; they are what a runtime host
       (run_host_main) uses to drive gui as a service, and what the boot loop below wraps. A
       frame is four explicit phases -- this is a multi-context system and the API does not hide
       it; even a single-context host names its one context:

         if ( frame_begin(dt) )        -- global: snapshot app input, compute frame_dirty, reset the
         {                                draw list on dirty frames.  Binds NO context; call once at
                                          the top of the frame.  Returns frame_dirty: emit the UI
                                          build only when true -- on a false (clean) frame skip the
                                          context scopes entirely; render() replays the preserved
                                          geometry verbatim and frame_end patches the volatile
                                          widgets (gui()->volatile_cb) internally.
           ctx_begin(GUI_CTX_DEFAULT) -- bind a context and run its per-frame init; emit its
              ... emit windows ...        windows immediately after.
           ctx_end()                    -- close it, rebinding the previously-bound context.  Closing
         }                                the DEFAULT context also auto-emits the debug overlays
                                          when debug_enable is on.
         frame_end()                   -- seal the build (latches emit cost; asserts ctx balance).
                                          Call on clean frames too -- it runs the volatile replay.

       frame_begin/frame_end and ctx_begin/ctx_end are balanced scopes, exactly like
       window_begin/window_end: every begin has an end, and each end restores the scope its begin
       opened.  render() runs AFTER frame_end and consumes the sealed draw list.

       render()    -- flush one viewport's geometry partition to GPU; opens a LOAD render pass on
                      that viewport's swapchain, emits all draw calls, and closes the pass.  Also
                      paints the debug overlay when vp is the primary (index 0).
                      Call once per live viewport, each with the matching context cmd.

       End-of-loop pacing is NOT here: it is host policy.  The boot loop's version is boot_pace
       below; a runtime host schedules against its own frame deadline and reads gui's settle state
       through wants_redraw / frame_dirty / volatile_live -- the same three signals boot_pace
       gates on, which is the only part worth sharing. */

    bool ( *frame_begin )( f32 dt );
    void ( *frame_end   )( void );
    void ( *render      )( i32 vp, rhi_cmd_t cmd );

    /*========================================================================================
        BOOT PATH -- the other way to run gui  (boot_ == this path only)

        In plain terms, this is the quick-start option: instead of a host wiring up its own
        window, rhi context, and per-frame plumbing by hand, boot() does all of that in one
        call, and boot_poll / boot_present_begin / boot_present_end / boot_pace replace the
        manual frame-lifecycle calls with one simple loop. Reach for this in sandboxes, demos,
        and tools whose main window is nothing but a gui surface; a full engine host drives the
        frame verbs above directly instead. The two are not mutually exclusive machinery -- the
        boot loop is built entirely out of the same public calls a hand-rolled host would make.

        There are exactly TWO host methodologies, and this band is one of them:

          RUNTIME HOST (source/runtime, run_host_main) -- the idiomatic engine path.  The host
            owns the OS window, the rhi context, the event drain, the render composite, and the
            pacing; gui is one optional service among several (render, draw, job, hot-reload).
            It calls the frame verbs ABOVE and none of the verbs below.
          BOOT PATH (this band) -- the quick gui loop, for sandboxes, demos, and tools whose
            main window IS a gui surface.  gui owns the main surface AND the shape of the loop,
            so a test bed is a boot() plus a while().

        boot_ marks membership in that second methodology -- NOT a state dependency.  boot_poll
        happens to touch only app + rhi routing and would technically run anywhere, but nothing
        outside this loop calls it, and a real host needs what it does not offer: a look at the
        event ring for its own leftovers, and a close policy other than close-means-quit for
        every window gui did not claim.  It is boot's pump.  Judge membership by which loop a
        verb belongs to, not by what it happens to read.

        boot_pace is the case that shows why membership, not dependency, is the test.  It touches
        no s_boot state -- only the frame hooks and the settle queries -- so it would technically
        run anywhere.  It is still boot_, because it is this loop's PACING POLICY: a fixed
        spin/settle/block ladder.  A runtime host does not want that ladder; it schedules against
        a frame deadline and reads the same settle queries itself (run_host.c's editor_sleep).

        What the boot path does NOT change: the build (frame_begin / ctx / frame_end) and the
        render flush are the same shared verbs on both paths.  boot() itself is pure composition
        -- exactly the public calls a host makes by hand (rhi()->init -> window_open ->
        context_open -> init(font) -> viewport_open -> frame_set_hooks), unwound on failure --
        so nothing here is a mode you get locked into, and viewport_open still attaches gui to a
        host-owned window.

        The one real fork is the present pair: boot_present_begin / boot_present_end render
        through the boot-owned window + rhi context + primary viewport, state the runtime path
        has no way to hand them.  Off the boot path they are a reported no-op, never a partial
        frame.  That host writes the render block itself -- five calls, all public:

          gui()->viewport_update();                  -- reconcile floaters (after the build,
                                                        before any render: the safe teardown
                                                        point).  boot_present_begin's first act.
          rhi_cmd_t cmd = rhi()->frame_begin( ctx );  -- the HOST's context, not gui's
          if ( rhi_cmd_valid( cmd ) ) {
              ...clear / scene passes...             -- whatever goes under the UI
              gui()->render( vp0, cmd );             -- the UI, in a LOAD pass over it
              rhi()->frame_end( ctx );
          }
          gui()->viewport_render_floaters();         -- owned floaters, each on its own context

        Same five steps the boot pair runs internally; run_host.c (path B) and sb_vulkan are the
        worked examples.  Note the perf overlay's present bracket is armed inside the pair, so
        the runtime path shows no present row -- the readout differs, not the frame.

        The boot loop, end to end:

          i32 vp0 = gui()->boot( &desc );            -- once, after mod_init_all, before any
                                                        other window opens
          while ( gui()->boot_poll( &dt ) )
          {
              ...frame_begin / build / frame_end...  -- the shared verbs, identical on both paths
              rhi_cmd_t cmd;
              if ( gui()->boot_present_begin( &cmd ) )
                  ...host render passes...           -- true hands out the live cmd for the
                                                        host's own passes (offscreen scenes,
                                                        custom draws under the UI)
              gui()->boot_present_end();             -- gui draw + present + all owned floaters
              gui()->boot_pace( 4, 16 );             -- this loop's pacing policy, see below
          }
          gui()->shutdown();                         -- also tears down the boot-owned surface;
                                                        rhi()->shutdown() stays with the host

        boot()  -- stand the whole stack up from one descriptor (gui_boot_desc_t, gui.h): the
                   window is borderless by default, with the chrome shell auto-emitted each frame
                   (os_chrome opts back into the stock OS frame); rhi()->init() is idempotent, so
                   a host that already initialized rhi loses nothing.  Returns the primary
                   viewport, or GUI_VP_INVALID with everything unwound.
        boot_poll( &dt ) -- the loop's pump and its exit test: pump_events, then route every event
                   through rhi (swapchain resize) and gui (input, floater lifecycle).  False on
                   quit or on a WIN_CLOSE that reached gui unconsumed.  dt comes from the
                   frame_set_hooks clock (nominal 60 Hz without one), clamped to 100 ms so a
                   debugger stall does not step the UI by seconds.  The host still reads input
                   through app()'s snapshot API (key_pressed etc.); this owns only the event ring.
        boot_present_begin() -- viewport_update + minimized guard + rhi frame open + swapchain
                   clear (the boot clear color).  A balanced pair like every other begin/end:
                   begin's bool gates the HOST's passes only.  False means skip them (minimized,
                   swapchain rebuild, or no boot).
        boot_present_end()   -- draw the gui over whatever the host recorded, present the main
                   surface, then present every owned floater.  Call it unconditionally: it is
                   minimized-safe, and a no-op without a matching begin -- there is no hidden
                   self-begin.
        boot_pace( spin_sleep_ms, anim_sleep_ms )
                   -- this loop's end-of-frame sleep; call once at the very bottom.
                      Default path: sleep spin_sleep_ms between frames (4 ~= 250 Hz).
                      With idle skip on (set_idle_skip): block on OS input so a static UI burns no
                      frames, sleeping anim_sleep_ms (16 ~= 60 Hz) only while a widget animation
                      settles.  0 opts that sleep out (no call), even while the feature is on --
                      free-run for that path.  A no-op until frame_set_hooks provides the sleep /
                      wait callbacks.  A host that wants its own cadence simply does not call it.
    ==========================================================================================*/

    i32  ( *boot               )( const gui_boot_desc_t* desc );
    bool ( *boot_poll          )( f32* out_dt );
    bool ( *boot_present_begin )( rhi_cmd_t* out_cmd );
    void ( *boot_present_end   )( void );
    void ( *boot_pace          )( i32 spin_sleep_ms, i32 anim_sleep_ms );

    /* Idle-skip control -- the programmatic twin of the I hotkey.  When on, boot_pace blocks on
       OS input while the UI is idle instead of spinning.  Off by default. */
    void ( *set_idle_skip )( bool on );
    bool ( *idle_skip     )( void );

    /* Viewport management -- a viewport is where gui actually draws to: one render surface backed
       by one OS window. Most apps only ever have one (the main window); a second is a floating
       tool window, an about box, anything with its own OS window. One frame's build gathers
       every window's geometry into a single draw list; render() then dispatches each window's
       share of that list to the viewport it is assigned to (window_set_next_viewport, or
       inherited from whichever viewport was most recently emitted into this frame).

       viewport_open()   -- open a surface for OS window win_id.  The initial drawable size is
                            queried from that window's rhi context internally -- no redundant w/h
                            parameters.  Returns a valid handle, or GUI_VP_INVALID (with the rule
                            printed) when init() has not run, win_id is out of range, the slot is
                            already open, or the window has no live rhi context -- open it with
                            rhi()->context_open( win ) first, since gui flushes into ITS swapchain.
                            The first call creates the primary (index 0); call before any frames.
                            win_id routes mouse events from that OS window to this surface.
       viewport_close()  -- close a viewport and release its GPU geometry buffers.  Works for both
                            the primary and secondary viewports.  Windows on the closed viewport
                            automatically fall back to the primary.  The host owns the OS window and
                            rhi context; gui owns only the geometry.
       viewport_resize() -- update a viewport's drawable size.  Prefer rhi()->event() +
                            gui()->event() for automatic routing; call this directly only when
                            explicit control is needed.
       viewport_shell()  -- emit the window chrome for a borderless viewport.  Every host window is
                            one of two things: a UI window (window_begin -- a body full of widgets)
                            or a chrome shell for the OS window itself -- this call.  It emits a
                            frame-only GUI_WIN_NATIVE window: its titlebar IS the OS caption (title
                            text + min/max/close buttons, drag to move, double-click to maximize),
                            its border the OS sizing frame, and its body is empty and click-through
                            -- every other window lives on top of it.  Call it FIRST inside
                            ctx_begin, every frame, before any other window on that viewport.
                            Returns the caption band height so the host can stack its own strips
                            (menu bar, toolbar) below it -- the built-in main_menu_bar, free-window
                            clamping, and the dock tree already inset themselves automatically.
                            On a viewport whose OS window has its own chrome (opened without
                            APP_WIN_BORDERLESS) it is a no-op returning 0: call it unconditionally
                            and flip only the window_open flag to switch chrome modes.  flags may
                            add GUI_WIN_NOTITLEBAR / NO_MINIMIZE / NO_MAXIMIZE / NORESIZE.
       viewport_caption_h() -- the caption band height (px) a chrome shell published on this
                            viewport; 0 for an OS-chrome window.  The query twin of
                            viewport_shell's return, for hosts on the boot path (where the shell
                            is emitted internally) that stack pinned strips below the caption.
       viewport_size()      -- the viewport's current drawable size (disp_w/disp_h) -- the query
                            twin of viewport_resize.  Either out pointer may be NULL; an invalid
                            viewport reports 0 x 0.
       viewport_content_y() -- the y where host content starts on this viewport: 0 for an
                            OS-chrome window, the caption band on a gui-shelled native window,
                            plus the main menu bar when one was emitted (this frame or last --
                            emit the bar before querying).  The same bound the maximize pin and
                            free-window clamp use, published so hosts place windows below the
                            viewport chrome without summing the parts themselves. */

    i32  ( *viewport_open      )( i32 win_id );
    void ( *viewport_close     )( i32 vp );
    void ( *viewport_resize    )( i32 vp, i32 w, i32 h );
    f32  ( *viewport_shell     )( i32 vp, const char* title, gui_win_flags_t flags );
    f32  ( *viewport_caption_h )( i32 vp );
    void ( *viewport_size      )( i32 vp, i32* out_w, i32* out_h );
    f32  ( *viewport_content_y )( i32 vp );

    /* gui-owned floater surfaces -- a floater is a small OS window gui opens and closes by
       itself (the window a user gets by dragging a panel loose from the main UI). Where
       viewport_open hands gui a host-created window + context to flush into, these own the OS
       window + rhi context end to end: gui creates them on spawn and tears them down on close.
       This is the lifecycle the tear-off gesture drives; a host may also call viewport_spawn
       directly to place a panel in its own OS window.

       viewport_spawn()          -- open a floater hosting its own OS window at (x,y) sized w x h;
                                    returns its viewport handle (assign windows via
                                    window_set_next_viewport) or GUI_VP_INVALID.  Between frames.
       viewport_update()         -- reconcile owned floaters with their OS windows: apply tear-off /
                                    merge-back and tear down closed or abandoned surfaces.  Call once
                                    per frame AFTER the UI build and BEFORE rendering (the safe point
                                    to free a surface).
       viewport_render_floaters() -- present every owned floater from the shared draw list, each on
                                    its own rhi context (frame_begin/clear/flush/frame_end).  The
                                    host still presents the main surface (index 0) via render(). */

    i32  ( *viewport_spawn           )( const char* title, i32 x, i32 y, i32 w, i32 h );
    void ( *viewport_update          )( void );
    void ( *viewport_render_floaters )( void );

    /* Multi-context -- lets one gui instance run more than one independent UI at once, each with
       its own windows, nav, popups, keyed widget state, and id namespace, so identically-named
       widgets in two contexts never collide. Most hosts only ever use the primary context
       (GUI_CTX_DEFAULT / 0), which is always live after init(); a secondary context is for
       something like a separate in-game overlay that must not share state with the editor UI.

       ctx_create()       -- allocate a fresh secondary context, sized to `cfg` (NULL / zero fields =
                             the internal maxima the library was compiled with).
                             Each gets a unique id_salt so same-named widgets in different contexts
                             never alias.  Returns GUI_CTX_INVALID on pool exhaustion.  Between frames.
       ctx_destroy()      -- free a secondary context; rebinds the default if it was current.  Never
                             destroys GUI_CTX_DEFAULT.  Call between frames.
       ctx_bind()         -- make ctx the current context with no per-frame init: a mid-build "switch
                             retained state" escape hatch.  ctx_begin/ctx_end are the normal scope;
                             reach for ctx_bind only to peek at another context's state mid-frame.
                             GUI_CTX_DEFAULT (0) or any invalid handle rebinds the default.
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
         if ( frame_begin(dt) )         -- once: input poll; true = emit this frame (frame_dirty).
         {
           ctx_begin(GUI_CTX_DEFAULT) -- bind + init the default context; emit its windows.
           ctx_end()                    -- close it (auto-emits debug overlays when debug is on).
           ctx_begin(ctx2)              -- a second context, if any; emit its windows.
           ctx_end()
         }
         frame_end()                    -- seal the build; volatile replay on clean frames.
       A single-context host runs exactly one ctx_begin(GUI_CTX_DEFAULT)/ctx_end pair. */

    i32  ( *ctx_create        )( const gui_ctx_config_t* cfg );
    void ( *ctx_destroy       )( i32 ctx );
    void ( *ctx_bind          )( i32 ctx );
    void ( *ctx_set_listening )( i32 ctx, bool listen );
    void ( *ctx_begin         )( i32 ctx );
    void ( *ctx_end           )( void );

    /* Host input -- the one entry point for OS events. The host drains its own app event ring
       and forwards each event here so gui can update input state and hit-test widgets. The
       return value (app_event_result_t, app.h) tells the host whether it should keep routing
       the event to other systems: the host keeps routing until some sink returns
       APP_EVENT_CONSUMED. gui only claims events it exclusively owns:

         - APP_EV_CHAR / MOUSE_WHEEL / CLIPBOARD:  input state gui alone keeps  -> CONSUMED.
         - APP_EV_MOUSE_MOVE / _DOWN / _UP:        records which viewport the cursor is over,
           but the UI-vs-scene decision is the capture fence's at read time      -> SHARED.
         - APP_EV_WIN_RESIZE:  updates the matching viewport's drawable size.  A gui-OWNED
           floater is gui's window end to end -> CONSUMED; the primary window is the host's
           surface that gui merely tracks     -> SHARED, so rhi()->event() and the host still
           see it.  A resize for an unknown window is PASS.
         - APP_EV_WIN_CLOSE:   an owned floater is marked for teardown -> CONSUMED; any other
           window's close is not gui's business at all                 -> PASS, so the host's
           close-to-quit path runs.
         - everything else: PASS. */

    app_event_result_t ( *event )( const app_event_t* ev );

    /*============================================================================================================
        GUI_DRAW -- render server  (render/ + draw/)
        Everything in this band puts actual pixels on screen: fonts, icons, textures, the draw_*
        primitive set, paths, and clips. Every call here takes an explicit color and size -- there
        is no ambient style at this level, and none of it knows or cares how the rect it was
        given was computed. It draws exactly what it is told, clipped to the ambient clip rect
        and stacked at the ambient z order.
    =============================================================================================================*/

    /*====================================  fonts -- id-addressed registry  =====================================*/

    /* Font -- select / load fonts; call between frames (outside frame_begin / render), except
       push_font / pop_font which may bracket a section or widget mid-frame.

       Fonts live in a small registry of numbered slots, and everything below is either loading
       one into a slot or picking which slot is active. Slot 0 is the default; it is empty until the first
       font_load / font_load_into( 0, path ) call -- call one right after gui()->init(), before any
       frame renders.  font_load() loads a .orb_font into a fresh id; font_load_into() loads one
       into an existing id (id 0 swaps the default).  font_use() makes a loaded id active; another
       context can select its own font this way.  push_font() / pop_font() bracket a temporary
       font and restore the previous one.  Each font_load/font_load_into uses its own bindless
       texture.  Widget layout dimensions follow the active font's metrics. */

    bool ( *font_load_into     )( u32 id, const char* path );
    void ( *font_use           )( u32 id );
    void ( *push_font          )( u32 id );
    void ( *pop_font           )( void );
    u32  ( *font_active_id     )( void );   // id of the currently active font (save/restore, or just to inspect)

    /* The bindless index + pixel size backing a loaded font id's live GPU atlas, for
       previewing it through image_texture / draw_texture_in (0 / {0,0} if empty). */
    u32        ( *font_atlas_idx  )( u32 font_id );
    gui_vec2_t ( *font_atlas_size )( u32 font_id );

    /*===========================  custom draw -- canvas primitives, symbols, paths  ============================*/

    /* Low-level draw list access -- the bare-metal drawing calls: no widget, no layout, just
       geometry. May be called anywhere between frame_begin and render.
       draw_rect and draw_text push geometry directly into the draw list.
       draw_rects pushes N solid rects as ONE command -- the batched form for dense custom
       drawing (timeline bars, graph columns) that would otherwise exhaust the frame's command
       budget one draw_rect at a time.
       push_clip / pop_clip set the current scissor rectangle. */

    void ( *draw_rect  )( f32 x, f32 y, f32 w, f32 h, u32 col );
    void ( *draw_rects )( const gui_rect_col_t* rects, u32 count );
    void ( *draw_text  )( f32 x, f32 y, u32 col, const char* str );

    /* volatile_cb -- an escape hatch for content that must keep animating even on frames where
       the rest of the UI is frozen because nothing changed (a live clock readout, a spinning
       spinner). It runs `fn` inline, as ordinary code, wrapped so its command range can be
       replayed standalone on frames where the rest of the UI build is skipped (frame_begin
       returned false; frame_end runs the replay internally -- see frame_dirty below).
       `fn` calls ordinary emit functions (text, rect_filled,
       etc) and should bracket them with volatile_begin()/volatile_end() from inside its own body.
       `label` is hashed the same way item_id() hashes a label -- combined with the current id
       scope, so it need only be stable and unique within its own call site, same as any other
       widget label.  Interactive widgets are safe to call from `fn` but are inert during replay --
       see gui.h (gui_volatile_fn) for the contract. */
    void ( *volatile_cb    )( const char* label, gui_volatile_fn fn );
    void ( *volatile_begin )( void );   // called from inside fn: stamp the callback's start position
    void ( *volatile_end   )( void );   // called from inside fn: reserved, no-op today
    
    /* text_size -- laid-out pixel size of s (widest line x line span; '\n' breaks).  CalcTextSize. */
    gui_vec2_t ( *text_size )( const char* str );

    /* draw_text_in -- draw s aligned within rect r (gui_align_t; multi-line, each line aligned).
       The placement primitive: "right-align this caption in the canvas" with no hand-computed edge.
       draw_text_clipped is the single-line variant that ellipsizes to r's width. */
    void ( *draw_text_in      )( gui_rect_t r, gui_align_t align, u32 col, const char* str );
    void ( *draw_text_clipped )( gui_rect_t r, gui_align_t align, u32 col, const char* str );

    /* draw_text_xf -- text that scales and rotates, for gauges, compasses, and spinning labels:
       the same run as draw_text, scaled uniformly and rotated about (x, y).
       `rot` is radians in screen space (0 points +x, positive turns clockwise -- gui_radians()).
       (x, y) is both the anchor and the pivot; to turn a label about its own middle, offset the
       anchor by the rotated half-extent of text_size().  Single line: '\n' is not a break here.

       WHAT IT LOOKS LIKE IS THE FONT'S DOING, not this call's.  The geometry is exact at any
       scale and angle either way, but a COVERAGE font is point-sampled, so magnifying one
       magnifies its texels; a DISTANCE-FIELD font (font_tool -sdf) resolves its edge in the
       fragment from a screen-space derivative and therefore stays sharp at any size and any
       angle, with no parameter here to tune and no second draw call -- an SDF run and the
       upright text beside it merge into one batch when they share a font.

       Cost, stated plainly: the transform is baked into vertices, so a run that MOVES
       re-tessellates on the frames it moves (unlike draw_pulse, which animates in the fragment
       and never re-emits).  That is a few quads, not a frame -- but a hundred spinning labels is
       a hundred runs of glyph work per frame, and a caller wanting that should say so knowingly. */
    void ( *draw_text_xf )( f32 x, f32 y, u32 col, const char* str, f32 scale, f32 rot );

    /* Icons -- small symbols (a folder, a gear, a checkmark, an editor glyph) packed at runtime
       into one shared R8 atlas texture so they all batch into the same draw call as text.
       register_icon packs a raw monochrome bitmap (row-major coverage, w*h bytes) and returns a
       handle (0 = atlas full); the pixels live in the same flush as text and tint by `col`.

       load_icon is the from-disk source: it decodes an image file (PNG and the other stb_image
       formats) to R8 coverage -- alpha channel when present, else luminance -- and registers it the
       same way, so a loaded icon is identical to a procedural one downstream.  `path` is resolved
       through asset_path -- a plain path relative to the assets root ("assets/icon/foo.png"), no
       need to call asset_path yourself first.  
       
       load_icons is the batch form an application's own icon table wants: a flat array of 
       name,path string PAIRS -- lookup name first, then the root-relative image path, e.g. 
       { "gear", "assets/icon/gear.png", "play", "assets/my/play.png" }.  `count` is the TOTAL
       string count (ARRAY_COUNT of the table), so it must be even; an odd count is a name missing 
       its path and asserts.  Names already registered are skipped, so the call is idempotent and
       safe to repeat after a hot reload.

       It returns how many of the named icons are available afterward -- compare against
       count / 2 to log a shortfall.  find_icon looks one up by the name it was registered with 
       (built-in icons register at gui init); icon_size is its native pixel size (for layout). 
       image is a layout widget (reserve w x h, draw centered/fit); draw_icon_in places an icon 
       in a rect the caller already holds (cell / button label / canvas cut).  col 0 means white. */

    gui_icon_id_t ( *register_icon )( const char* name, u32 w, u32 h, const u8* coverage );
    gui_icon_id_t ( *load_icon     )( const char* name, const char* path );
    u32           ( *load_icons    )( const char* const* pairs, u32 count );
    gui_icon_id_t ( *find_icon     )( const char* name );
    gui_vec2_t    ( *icon_size     )( gui_icon_id_t id );
    void          ( *image         )( gui_icon_id_t id, f32 w, f32 h, u32 col );
    void          ( *draw_icon_in  )( gui_rect_t r, gui_icon_id_t id, u32 col );

    /* draw_icon_in turned about the fitted box's centre (radians, screen space) -- compass
       needles, minimap markers, spinner glyphs.  An SDF icon turns clean at any angle (its edge
       resolves in the fragment); a coverage icon shows its texels, the same split the two font
       bakes have.  One quad, same batch. */
    void          ( *draw_icon_xf  )( gui_rect_t r, gui_icon_id_t id, u32 col, f32 rot );

    /* The distance-field twin of the icon calls above: the same kind of icon, but stored in a
       way that stays crisp at any size and rotation and can take an outline or glow.
       The _sdf twins register the same coverage as a DISTANCE FIELD instead: the bytes are
       transformed and land in the distance-field atlas, and the fragment then recovers the edge
       from the field's screen-space derivative rather than from a texel.  What that buys is an
       icon that is exact at ANY size, survives rotation, and can take a GUI_OP_TEXT_EDGE outline
       or glow -- none of which a coverage icon can do, because a coverage atlas must be sampled
       NEAREST or bitmap text stops being crisp.

       It is a per-icon choice and both kinds are wanted.  Keep COVERAGE for pixel-precise art: a
       16 px symbol tuned to the grid, anything with 1 px detail or deliberately hard corners, all
       of which a field rounds off.  Reach for SDF when the icon is drawn at sizes other than the
       one it was authored at, or wants an outline.  Mixing them is free -- the sampling model
       travels in the vertex, so a coverage icon, an SDF icon, a glyph run and a fill still share
       one draw call.  Everything downstream of the id is identical: find_icon, icon_size, image
       and draw_icon_in do not know or care which kind they hold.

       The source art wants to differ though, and this is the part to get right.  A field is
       transformed at the SOURCE resolution and stored at `out_max` (longest edge, 0 = 64), so the
       source should be several times that -- transforming a 16 px bitmap yields a smooth field
       around a 16 px staircase, which is no better than the bitmap.  The art also needs a
       transparent MARGIN: a shape running to the edge of its own bitmap has no outside there for
       the field to fall off into, so that edge draws hard and takes no outline (it logs a note if
       you do it). */
    gui_icon_id_t ( *register_icon_sdf )( const char* name, u32 w, u32 h, const u8* coverage,
                                          u32 out_max );
    gui_icon_id_t ( *load_icon_sdf     )( const char* name, const char* path, u32 out_max );

    /* BAKED SHAPES -- the same distance field as an SDF icon, read one stage earlier, which is the
       whole difference and a large one.  An SDF icon's texel becomes ALPHA in the colour stage,
       after the effect cascade has already run, so an outline is the only thing that can be made
       from it.  A shape's texel becomes the fragment's DISTANCE before a single op has run, so it
       is a FIELD in exactly the sense a rounded box is -- and every op reaches it.

       What that unlocks is the silhouette no expression and no record can state: the record's own
       subtraction tops out at one rounded-box cut and no stack of quads can un-paint ink, so a
       keyhole, a gear, a notched badge had nowhere to come from.  Bake one and it wears the border,
       the glow, the inset, the swell, the pulse and the cut a panel wears.

       draw_set_shape is how the existing verbs are aimed at it -- it is ambient like the corner
       radius, so draw_shadow / draw_glow / draw_inset / draw_ring / draw_pulse / draw_swell need no
       shape-flavoured twin and never will:

           gui()->draw_set_shape( keyhole );
           gui()->draw_glow  ( r, 0.0f, 18.0f, AMBER );
           gui()->draw_ring  ( r, 0.0f,  2.0f, INK   );
           gui()->draw_set_shape( GUI_SHAPE_NONE );

       draw_shape_in is the flat placement primitive for callers who want the art and no effect --
       the shape analogue of draw_icon_in, and it saves and restores the ambient itself.

       THE ONE NUMBER TO RESPECT is shape_reach.  A stored field only holds the margin the bake
       padded for (gui_shape_bake_t.spread, 8 texels by default), scaled by how big the shape is
       drawn; a border or glow asking to travel further does not fade out, it stops dead where the
       texels saturate.  Ask for the ceiling rather than discovering it visually.

       Two ops are unavailable and always will be: DASH and GRAD_ALONG walk a boundary coordinate,
       and an R8 distance field carries no arc length.  They are dropped with one warning rather
       than cutting on a coordinate that is always zero.

       The source art follows the SDF icon rules -- transform at several times the stored size, or
       the field is smooth around a staircase -- with the margin the one thing it need NOT bring:
       GUI_SDF_PAD_GROW makes the margin itself.  GUI_SDF_PAD_KEEP is the import path for art that
       already carries spacing (a 128x128 authored with room), and GUI_SDF_PAD_INSET shrinks the art
       inside a fixed tenant when atlas area is the scarce thing. */

    gui_shape_id_t ( *register_shape )( const char* name, u32 w, u32 h, const u8* coverage,
                                        const gui_shape_bake_t* bake );   // NULL = every default
    gui_shape_id_t ( *find_shape     )( const char* name );
    gui_vec2_t     ( *shape_size     )( gui_shape_id_t id );   // the INK's stored size, texels
    f32            ( *shape_reach    )( gui_shape_id_t id, gui_rect_t r );  // effect ceiling, px
    void           ( *draw_set_shape )( gui_shape_id_t id );   // GUI_SHAPE_NONE restores the box
    void           ( *draw_shape_in  )( gui_rect_t r, gui_shape_id_t id, u32 col );

    /* RGBA textures -- display an arbitrary bindless texture (a scene render target, a loaded
       image) as a full-color quad; the texel is the color, tint_abgr multiplies (0 = untinted).
       image_texture flows in the layout like image(); draw_texture_in fills a rect the caller
       already holds.  The caller owns the texture + its bindless slot (rhi register_texture). */

    void ( *image_texture   )( u32 bindless_idx, f32 w, f32 h, u32 tint_abgr );
    void ( *draw_texture_in )( gui_rect_t r, u32 bindless_idx, u32 tint_abgr );
    /* The rotated form (radians, about the rect centre).  Ignores the ambient rounding --
       a rounded rotated picture stacks draw_rect_xf behind a plain one instead. */
    void ( *draw_texture_xf )( gui_rect_t r, u32 bindless_idx, u32 tint_abgr, f32 rot );

    /* Sprites -- authored art (a PNG you drew, not a generated icon), packed into a sprite atlas
       of their own so a whole skin is still ONE draw call.  The registration verbs mirror the icon ones exactly (register / load /
       find / size), because the two kinds differ in what a texel MEANS -- an icon is coverage the
       colour paints, a sprite is a picture the colour tints -- not in how you obtain one.
       register_sprite takes raw RGBA8 (row-major, w*h*4, straight alpha); load_sprite decodes an
       image file through asset_path like load_icon.

       set_slice is the verb with no icon twin, and the reason sprites are their own kind: it
       declares four insets, in SOURCE pixels, that cut the art into nine pieces.  A sliced sprite
       filling any rect keeps its four corners at authored size while its edges and centre stretch
       (or tile, with GUI_BRUSH_TILE) -- so one 32x32 PNG is a window frame at every window size.
       Set it once after registering; every fill of that sprite inherits it.

       image_sprite flows in the layout like image(); draw_sprite_in FILLS a rect the caller
       already holds (it does not aspect-fit the way draw_icon_in does -- a sprite is usually a
       surface, and its job is to cover what it was given).  tint 0 means untinted. */

    gui_sprite_id_t ( *register_sprite )( const char* name, u32 w, u32 h, const u8* rgba );
    gui_sprite_id_t ( *load_sprite     )( const char* name, const char* path );
    gui_sprite_id_t ( *find_sprite     )( const char* name );
    bool            ( *sprite_set_slice)( gui_sprite_id_t id, gui_pad_t slice );
    gui_pad_t       ( *sprite_slice    )( gui_sprite_id_t id );
    gui_vec2_t      ( *sprite_size     )( gui_sprite_id_t id );
    void            ( *image_sprite    )( gui_sprite_id_t id, f32 w, f32 h, u32 tint_abgr );
    void            ( *draw_sprite_in  )( gui_rect_t r, gui_sprite_id_t id, u32 tint_abgr );

    /* draw_brush -- the general-purpose fill call: like draw_rect, but the fill can be a
       gradient, a stretched sprite, or a nine-slice frame instead of a flat color.
       draw_rect fills a rect with a colour; this fills
       one with a gui_brush_t (gui.h), which is the same thing plus three more answers to "with
       what": a gradient, a stretched sprite, or a nine-slice.  THE door a custom widget should
       paint its face through if it wants to be skinnable by whoever uses it, since a brush can be
       stored in the caller's own theme and swapped without touching the widget:

           gui()->draw_brush( face, &( gui_brush_t ){ .kind   = GUI_BRUSH_NINE,
                                                      .sprite = my_button_art,
                                                      .scale  = ui_scale } ); */

    void ( *draw_brush )( gui_rect_t r, const gui_brush_t* brush );

    /*=============================  ambient draw state -- save, set, draw, restore  =============================*/

    /* Ambient corner rounding for the rect-shaped verbs (draw_rect / draw_brush /
       draw_texture_in / draw_shadow).  draw_frame / draw_round_frame do NOT honor this -- they
       take rounding as a parameter (or force it to 0) precisely so a plain 2D draw call never
       has style bleed into it.  Ambient rather than a parameter because it applies to
       verbs that have no business growing one, and because a container wants to round everything
       drawn inside it without every call site knowing.  A rounded rect resolves as an SDF surface
       (gui.h, the effect band), which is what lets a TEXTURED quad round at all.

       It is a plain value, not a stack -- save and restore around the shapes it should affect:

           f32 save = gui()->draw_rounding();
           gui()->draw_set_rounding( 8.0f );
           gui()->draw_texture_in( r, tex, 0xFFFFFFFFu );
           gui()->draw_set_rounding( save ); */

    void ( *draw_set_rounding )( f32 r );
    f32  ( *draw_rounding     )( void );

    /* The corner PROFILE that rides with that radius: 0..1, where 0 is the circular arc every
       rounded shape has always had and 1 ramps the curvature across the whole corner -- the
       continuous corner ("corner smoothing" in a design tool).  It changes the shape of the
       curve, never its size, so a radius still means exactly what it meant.

       The theme owns it: GUI_VAR_CORNER_SMOOTH is installed into this ambient once per frame,
       so a look is one number and no widget has to know.  These two are for the same local
       override the radius pair serves -- save, set, draw, restore. */

    void ( *draw_set_corner_smooth )( f32 t );
    f32  ( *draw_corner_smooth     )( void );
    /* The curve every animating shape pushed after this is shaped by (gui_curve_t), and that
       curve's own parameter.  Ambient like the radius: save, override, restore. */
    void ( *draw_set_anim_curve    )( u32 curve, f32 param );
    void ( *draw_get_anim_curve    )( u32* curve, f32* param );
    /* The cycle offset added to every animating shape pushed after this -- how anim_once reaches
       the draws that state no phase of their own (the spinner, the marching ants). */
    void ( *draw_set_anim_phase    )( f32 cycles );
    f32  ( *draw_anim_phase        )( void );

    /* Where a stroked box's band sits against its boundary -- the stroke alignment every design
       tool offers: 0 inside (the default every outline has always had), 0.5 centred, 1 outside.
       Applies to the stroked rect / circle / ngon / dashed-border family; resolved at push time
       by inflating the shape, so it costs nothing downstream.  Ambient like the radius --
       save, set, draw, restore. */
    void ( *draw_set_border_align )( f32 a );
    f32  ( *draw_border_align     )( void );

    /* Ambient TEXT EDGE -- draws an outline or drop-shadow color around text, in the same draw
       call as the glyph itself: a second colour painted OUTSIDE the glyph boundary.  Slate spends a whole extra vertex field (SecondaryColor) on this; here
       it is a packed word on the text command, because once a glyph is a distance field the outline
       is not a second copy of the run -- it is the SAME quad, the same batch and the same texture
       sample, with the fill composited over a band widened by `width` pixels.  Nothing about the
       geometry changes, so an outline is affordable on body text, not just on titles.

       SDF fonts ONLY (a .orb_font baked with a spread): a coverage glyph has no signed distance to
       widen and ignores this.  Useful width is bounded by the baked spread, past which the field is
       flat and the outline stops growing rather than tearing.  A width of 0 clears it.

       Ambient like the radius above, and saved/restored the same way -- reading the pair back and
       setting it again is exact, so there is no separate raw setter:

           f32 sw; u32 sc; gui()->draw_get_text_edge( &sw, &sc );
           gui()->draw_set_text_edge( 2.0f, 0xFF000000u );   // 2 px black outline
           gui()->draw_text( x, y, 0xFFFFFFFFu, "Title" );
           gui()->draw_set_text_edge( sw, sc ); */

    void ( *draw_set_text_edge )( f32 width, u32 col );
    void ( *draw_get_text_edge )( f32* width, u32* col );

    /*==========================  the shape catalog -- the one-quad SDF draw verbs  ==========================*/

    /* THE SHAPE CATALOG -- the drawing toolbox every built-in widget paints its own chrome
       with, exposed so custom widgets and editors can paint the same way: symbols, shapes,
       lines and paths, patterns, light and shadow, repeated sets, and clock-driven motion,
       each in its own section below (Dear ImGui's AddXxx / Render* analogue, implemented in
       gui_symbol.c).  Everything that pushes geometry into the draw list is draw_*; render()
       is reserved for the frame flush.

       Nearly every verb resolves as ONE SDF quad: the fragment evaluates the shape's boundary
       analytically (gui_prim.h, the effect band), so edges antialias at any size, radius and
       softness, curved shapes have no segment count to tune, and a call merges into the batch
       already open.  The exceptions that still walk tessellated geometry are the polyline /
       path family, hatch / stripes, and a STROKED per-corner outline.

       Shared vocabulary, defined once:
         - every colour (`col`, `col_a`/`col_b`) is one packed ABGR word -- GUI_COLOR( r,g,b,a ).
         - angles are RADIANS in screen space: 0 points +x, positive turns clockwise (y down).
         - `rounding` is a corner radius in px.  A verb with no radius parameter honors the
           ambient rounding (draw_set_rounding); one that takes its own ignores the ambient.
           draw_frame is the one exception: it has no radius parameter AND ignores the ambient,
           forcing 0 -- see draw_frame / draw_round_frame below.
         - `thickness` picks fill vs stroke on the shapes that offer both: 0 FILLS the shape,
           a positive value strokes its outline that many px wide.  Closed outlines honor the
           border-align ambient (draw_set_border_align).
         - `feather` -- and a shadow's `spread` -- is the SOFT EDGE: how many px the boundary
           fades across (a design tool's feather / blur).  0 keeps the crisp 1 px antialiased
           edge.
         - `grow` is how far a boundary TRAVELS outward in px (negative shrinks) -- the swell
           and ripple verbs' reach.
         - the motion words -- `rate`, `phase`, the wave's curve -- are defined once at the
           clock-driven motion section below.
         - WHICH mark a widget draws (tick vs disc, triangle vs chevron) is style, not a draw
           call: a theme authors the GUI_VAR_*_SHAPE pick and push_style_var scopes it. */

    /*===================================  symbols -- the widget glyph marks  ====================================*/

    /* The marks widget chrome is drawn from -- menu ticks, tree arrows, close crosses, resize
       grips.  Each fits itself to the box or point it is given and takes one colour. */

    void ( *draw_check_mark        )( gui_rect_t box, u32 col );
    void ( *draw_arrow             )( gui_rect_t box, gui_dir_t dir, u32 col );
    void ( *draw_arrow_pointing_at )( f32 tx, f32 ty, f32 half, gui_dir_t dir, u32 col );
    void ( *draw_chevron           )( gui_rect_t box, gui_dir_t dir, f32 thickness, u32 col );
    void ( *draw_plus_minus        )( gui_rect_t box, bool plus, f32 thickness, u32 col );
    void ( *draw_close             )( gui_rect_t box, u32 col );
    void ( *draw_bullet            )( f32 cx, f32 cy, f32 r, u32 col );
    void ( *draw_grip              )( gui_rect_t box, u32 col );

    /*===============================  shapes -- boxes, polygons, discs, sectors  ================================*/

    /* The box family first, then the radial shapes, then the circular sectors.  Filled or
       stroked per the convention above; every one is a single SDF quad. */

    /* draw_frame / draw_round_frame -- draw_rect / draw_round_rect's dual-color sibling: a filled
       body plus a border band, ONE quad.  draw_frame is always square (rounding forced to 0, like
       draw_rect); draw_round_frame takes rounding as a parameter (like draw_round_rect), but only
       ONE radius for all four corners -- draw_round_rect's independent per-corner radii have no
       frame equivalent.  Neither reads the ambient rounding -- a caller never needs to
       save/set/restore draw_set_rounding around either call. */

    void ( *draw_frame             )( gui_rect_t box, u32 col_bg, u32 col_border, f32 border );
    void ( *draw_round_frame       )( gui_rect_t box, f32 rounding, u32 col_bg, u32 col_border, f32 border );
    void ( *draw_round_rect        )( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl, f32 thickness, u32 col );

    // TODO: draw_round_rect_outline -- a stroked rect with independent corner radii, one quad, the fragment
    // TODO: draw_round_frame_ex -- a round frame with independent corner radii, one quad.

    /* The SDF box under a rotation about its centre (radians, screen space) -- rotated cards,
       tilted badges, the plate behind rotated text.  feather 0 = crisp 1 px AA; wider = a rotated
       soft shadow.  Same single quad as the upright box. */
    void ( *draw_rect_xf           )( gui_rect_t box, f32 rounding, f32 feather, f32 rot, u32 col );

    /* A rounded rect minus a second rounded rect -- true subtraction, which no number of extra
       quads can paint: blending only adds ink.  The notched avatar behind a status dot, the
       ticket silhouette.  `cut` shares `box`'s absolute space and may straddle its edge; `soft`
       is the carved edge's AA band in px (clamped up to the standard 1 px).  ONE quad; ramps,
       patterns and the border frame compose over it like any other fill. */
    void ( *draw_rect_cut          )( gui_rect_t box, f32 rounding, gui_rect_t cut,
                                      f32 cut_rounding, f32 soft, u32 col );

    void ( *draw_circle            )( f32 cx, f32 cy, f32 r, f32 thickness, u32 col );
    void ( *draw_ngon              )( f32 cx, f32 cy, f32 r, u32 sides, f32 rot, f32 thickness, u32 col );

    /* The n-pointed star: draw_ngon with each edge midpoint pulled in to ratio * r.  ratio <= 0
       takes the classic five-point proportion; the field caps it at the polygon's apothem. */
    void ( *draw_star              )( f32 cx, f32 cy, f32 r, u32 points, f32 ratio, f32 rot, f32 thickness, u32 col );
    void ( *draw_arc               )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness, u32 col );
    void ( *draw_pie               )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, u32 col );

    /* The arc cut by an angular dash pattern -- dotted rings, tick dials.  dash/gap are arc-length
       pixels at radius r (the draw_dashed_line vocabulary); the period is snapped so whole cycles
       fit the sweep, so a closed dashed ring meets itself without a seam.  Animate by rotating
       a0/a1 together: the pattern rides the sector's frame. */
    void ( *draw_arc_dashed        )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness,
                                      f32 dash, f32 gap, u32 col );

    /* The arc whose colour sweeps col_a (at a0) -> col_b (at a1) by ANGLE -- the hot/cold value
       arc.  A per-vertex colour cannot express this (it varies by angle, not position); the
       fragment lerps it from the aperture it already computes. */
    void ( *draw_arc_gradient      )( f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 thickness,
                                      u32 col_a, u32 col_b );

    /* The determinate ring: an arc of `frac` (0..1) of a full turn from 12 o'clock. */
    void ( *draw_progress_arc      )( f32 cx, f32 cy, f32 r, f32 frac, f32 thickness, u32 col );

    /*=========================================  lines, curves + paths  ==========================================*/

    /* Straight and curved strokes (gui_stroke_align_t; see gui.h for the pixel model).
       draw_line     -- one segment, CENTER_BIASED: H/V lines render pixel-crisp, others antialiased.
       draw_capsule  -- the PILL: the same segment named as a shape, so it keeps its round caps and
                        its exact boundary at every angle instead of straightening into a snapped
                        rect when it happens to be horizontal.  The _outline form hollows it to a
                        `border` px wall -- still one quad, since the fragment bends the same field.
       draw_polyline -- a connected point array with miter-limited corners (always antialiased);
                        `closed` joins the last point back to the first (rect / polygon outlines).
       path_*        -- the retained form: clear, append points with path_line_to, then path_stroke
                        (which strokes and clears the buffer).  Up to GUI_PATH_MAX points.

           gui()->draw_line( 10, 10, 200, 80, 2.0f, col );      // a 2px antialiased diagonal
           gui()->draw_capsule( 20, 40, 90, 40, 14.0f, col );   // a 14px tall pill
           gui()->path_line_to( x0, y0 ); gui()->path_line_to( x1, y1 ); ...
           gui()->path_stroke( 1.5f, GUI_STROKE_CENTER, false, col ); */

    void ( *draw_line     )( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_dashed_line       )( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 col );
    void ( *draw_capsule  )( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_capsule_outline )( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness,
                                    f32 border, u32 col );
    void ( *draw_polyline )( const gui_vec2_t* pts, u32 count, f32 thickness,
                             gui_stroke_align_t align, bool closed, u32 col );
    void ( *path_clear    )( void );
    void ( *path_line_to  )( f32 x, f32 y );
    void ( *path_stroke   )( f32 thickness, gui_stroke_align_t align, bool closed, u32 col );
    void ( *draw_bezier_quad       )( f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, f32 thickness, u32 col );
    void ( *draw_bezier_cubic      )( f32 x0, f32 y0, f32 c0x, f32 c0y, f32 c1x, f32 c1y, f32 x1, f32 y1, f32 thickness, u32 col );
    /* A polyline whose corners are auto-filleted to `radius`, clamped per-corner to half its
       shorter adjacent run (same rule draw_clamp_rounding applies to a rect) -- the corner
       itself is the exact bezier control point for its fillet, so there is no control point to
       pick and no way for it to come out lopsided.  `closed` joins the last point to the first,
       every vertex a corner. */
    void ( *draw_rounded_path      )( const gui_vec2_t* pts, u32 count, f32 radius,
                                      f32 thickness, bool closed, u32 col );
    /* Point-to-point spline through every point in `pts` -- no radius, no control point to
       pick.  Each point's curve tangent is derived from its own neighbours (Catmull-Rom), so
       moving a point re-settles the curve on both sides of it automatically (the Blueprint
       node-graph wire model).  Collinear points degenerate to a straight run with no bulge. */
    void ( *draw_smooth_path       )( const gui_vec2_t* pts, u32 count, f32 thickness,
                                      bool closed, u32 col );
    /* Node-graph wire between two pins.  Endpoint tangents are horizontal by construction --
       out of the source pin heading right, into the destination pin heading right -- because a
       pin's exit direction belongs to the port, not to wherever the other end sits (which is
       why draw_smooth_path, deriving tangents from neighbours, draws a two-point path flat).
       Tangent length is 0.5 * the larger axis distance, floored at `min_tan` so touching pins
       still read as a curve and capped at `max_tan` so a graph-spanning wire does not balloon;
       a backward wire (x1 < x0) widens past the cap to keep its doubleback from overlapping
       itself.  Both clamps are in the same already-DPI-scaled space as the pin coordinates. */
    void ( *draw_wire              )( f32 x0, f32 y0, f32 x1, f32 y1, f32 min_tan, f32 max_tan,
                                      f32 thickness, u32 col );

    /*===============================  patterns + gradients -- what fills a shape  ===============================*/

    /* What a rect is filled WITH rather than what shape it is.  All four tile in the fragment
       (hatch and stripes are the lattice cut on one axis), so any area and any cell count is
       ONE quad, and every one lands inside the ambient rounding's boundary. */

    void ( *draw_checker           )( gui_rect_t box, f32 cell, u32 col_a, u32 col_b );
    /* Line lattice over `box`: a `thickness` px line every `cell` px, over NOTHING -- layer it on
       your own fill.  Anchored to (origin_x, origin_y) in screen px, so a panning canvas passes
       its content origin and the lattice rides the pan.  Major/minor graph paper = two calls. */
    void ( *draw_grid              )( gui_rect_t box, f32 cell, f32 thickness,
                                      f32 origin_x, f32 origin_y, u32 col );
    void ( *draw_hatch             )( gui_rect_t box, f32 spacing, f32 thickness, u32 col );
    void ( *draw_stripes           )( gui_rect_t box, f32 spacing, f32 thickness, f32 angle,
                                      u32 col );
    void ( *draw_gradient          )( gui_rect_t box, u32 col_a, u32 col_b, bool horizontal );
    /* Gradient fill of a ROUNDED box, resolved in the FRAGMENT off the primitive record.  `kind`
       shapes the ramp and `angle` orients it: the axis for GUI_GRAD_LINEAR, the direction the
       sheen peaks toward for GUI_GRAD_CONIC, ignored by GUI_GRAD_RADIAL.  Radial and conic exist
       here and not on draw_gradient because neither can be described by colours at a rectangle's
       corners.  `mid` bends the ramp: where along it the 50/50 blend lands (a design tool's
       gradient midpoint handle), 0..1; 0.5 -- and 0, the unset default -- is the linear ramp. */
    void ( *draw_round_rect_gradient )( gui_rect_t box, f32 rounding, u32 col_a, u32 col_b,
                                        gui_grad_t kind, f32 angle, f32 mid );

    /*============================  light + shadow -- the soft single-quad surfaces  =============================*/

    /* Five soft surfaces sharing one economy: each is a single quad whose falloff the fragment
       resolves, merged into the open batch at any size and softness.  Picking one:

         draw_shadow             filled soft cast -- MEANT to show through a translucent subject
         draw_drop_shadow        the cast with the caster's silhouette cut out: the standard
                                 panel shadow, directional via (off_x, off_y)
         draw_inset_shadow       the falloff turned inward -- the pressed well / inner shadow
         draw_glow               exponential falloff -- reads as light, not blur; draw it
                                 BEFORE the body it halos
         draw_round_rect_shadow  per-corner radii + feather -- the soft cast under a tab or
                                 asymmetric card, which one-radius draw_shadow cannot shape

       All honor the ambient rounding except draw_round_rect_shadow, which states its own.
       `spread` / `feather` / `depth` is how far the falloff reaches, in px.

       draw_edge_shadow sits next to these but is not one of them: a plain gradient quad, not an
       SDF field, for a one-sided lift where the full ambient wrap is more than the chrome needs. */

    void ( *draw_shadow            )( gui_rect_t box, f32 spread, u32 col );
    /* Nothing paints inside `box`: the falloff is measured from the shadow's outline while the
       hole is cut against the CASTER's, (off_x, off_y) px away -- which is what gives the cast
       a direction.  (0, 0) is the even cast on all four sides. */
    void ( *draw_drop_shadow       )( gui_rect_t box, f32 spread, f32 off_x, f32 off_y, u32 col );
    void ( *draw_inset_shadow      )( gui_rect_t box, f32 depth, u32 col );
    /* The cheap one-sided cousin of the above: a plain gradient quad fading from `col` at `edge`
       to transparent `size` px outside `box`, no SDF field at all.  For chrome that only wants a
       lift on one side -- a titlebar's bottom seam, a scrollbar track's inner edge -- rather than
       the full ambient wrap. */
    void ( *draw_edge_shadow       )( gui_rect_t box, gui_edge_t edge, f32 size, u32 col );
    /* Light falls off by a constant fraction per pixel, so the exponential halo reads as
       emission where the linear one reads as blur.  The core is filled, so it shows through a
       translucent subject.  Focus rings, validation states, drag targets, recording indicators;
       composed with draw_pulse's clock it breathes for free. */
    void ( *draw_glow              )( gui_rect_t box, f32 spread, u32 col );
    void ( *draw_round_rect_shadow )( gui_rect_t box, f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
                                      f32 feather, u32 col );

    /*=================================  sets + meters -- one quad, many copies  =================================*/

    /* Repeated copies of one cell, folded in the FRAGMENT: however many copies, each set is ONE
       quad and ONE prim record, so a 3x3 dot field and a 40-tick ruler cost the same.  Cells
       honor the ambient rounding (dots, squares or pills). */

    /* nx by ny copies `pitch` apart centre-to-centre, centred in `at`; `size` is the cell's
       side, and the pitch is floored just above it so copies never touch. */
    void ( *draw_dot_grid          )( gui_rect_t at, u32 nx, u32 ny, f32 pitch_x, f32 pitch_y,
                                      f32 size, u32 col );
    /* The one-axis form with the pitch solved from the count and the span, so the first and
       last marks land on the ends -- ruler gradations, slider detents, timeline marks.  `len`
       is how far each tick reaches across the bar (0 = all the way). */
    void ( *draw_ticks             )( gui_rect_t bar, u32 n, f32 thickness, f32 len,
                                      bool vertical, u32 col );
    /* The angular form: `n` marks on a circle fitted to `box`, each turned to point outward --
       the gauge face, one quad.  A non-zero `rate` (revolutions/sec) spins it on the shader
       clock (the motion section's contract); 0 is the static face. */
    void ( *draw_dial_ticks        )( gui_rect_t box, u32 n, f32 thickness, f32 len,
                                      f32 rate, u32 col );
    /* A segmented level meter: `n` cells across `bar`, the first `value` (0..1) fraction lit in
       `col`, the rest in `col_off` (alpha 0 = nothing behind).  Two quads however many cells,
       and the lit count is DATA on the record rather than geometry, so a moving level re-emits
       one float.  Fills left to right. */
    void ( *draw_meter             )( gui_rect_t bar, u32 n, f32 value, u32 col, u32 col_off );

    /*=======================  clock-driven motion -- animates with zero re-tessellation  ========================*/

    /* Everything here animates in the FRAGMENT on the shared shader clock: the emitted command
       is byte-identical every frame, so the window keeps its retained geometry and the motion
       re-tessellates NOTHING -- where easing a value yourself (anim_ease) dirties the window
       every frame it moves.  One contract covers every verb below:

         - the clock advancing does not schedule a frame.  Call request_redraw() each frame the
           effect should visibly run -- the same contract a volatile widget has.
         - `rate` is cycles per second; what one cycle IS belongs to the verb (a breath, a lap,
           a revolution).  rate 0 stands the clock still at `phase`.
         - `phase` offsets the wave in cycles, so a row of same-rate elements staggers instead
           of beating in lockstep; 0 = in step.  One-shots: anim_once turns an event time into
           the (rate, phase) to emit with and reports when to stop -- draw the settled state
           from there.
         - the wave's shape between its endpoints is the ambient curve (draw_set_anim_curve).

       Picking a spinner: draw_spinner is the sweeping 270-degree arc; draw_dot_spinner the dot
       ring (rigid, or tail-chasing); draw_border_tracer laps the widget's own outline. */

    /* A rounded fill whose ALPHA breathes: `rate` in Hz, `depth` the 0..1 fraction of alpha
       taken at the trough.  The "this is live / recording" indicator.  Honors the ambient
       rounding. */
    void ( *draw_pulse             )( gui_rect_t box, f32 rate, f32 depth, f32 phase, u32 col );
    /* A rounded fill whose SIZE breathes: the boundary travels `grow` px past the box (negative
       shrinks) as the clock runs one cycle.  The pop, the hover grow, the press shrink, the
       breathing focus ring.  At rate 0 it is a static per-element size offset off one shared
       style.  Honors the ambient rounding. */
    void ( *draw_swell             )( gui_rect_t box, f32 rate, f32 grow, f32 phase, u32 col );
    /* The sonar ping: a hollow ring of `thickness` px hugging radius `r` that swells `grow`
       px outward while it fades, expanding and dying on one clock.  `rate` repeats it; for a
       single ping use anim_once and stop drawing when it reports done.  The attention ping, the
       click ripple, the radar pulse. */
    void ( *draw_ripple            )( f32 cx, f32 cy, f32 r, f32 thickness, f32 grow,
                                      f32 rate, f32 phase, u32 col );
    /* A hollow ring hugging `box`: a band of `t` px lying inside the boundary, cut from the
       shape's own FIELD rather than stroked around its perimeter.  That is the difference from
       draw_round_rect's outline and the reason to reach for this one -- a field-borne band
       composes with everything else measured against a field, so it swells, glows and breathes,
       and under an ambient baked shape it traces that silhouette instead of a rectangle.  One
       quad.  Honors the ambient rounding. */
    void ( *draw_ring              )( gui_rect_t box, f32 t, u32 col );
    /* The raw fx_box entry point: every knob draw_shadow / draw_glow / draw_pulse / draw_swell /
       draw_ring narrows to one fixed combination, all exposed together here for a combination
       that does not have its own verb yet.  `variant` picks the field (GUI_FX_BOX_FILL / SKIRT /
       INSET / GLOW / RING); a non-zero `rate` layers GUI_OP_PULSE on top of whichever variant is
       set and a non-zero `swell` layers GUI_OP_SWELL, so a swelling glow or a pulsing ring is one
       call rather than a new verb.  `border` is read only under GUI_FX_BOX_RING; `cut_dx`/
       `cut_dy` only under GUI_FX_BOX_SKIRT (the caster offset).  `rot` turns the whole surface
       about its centre.  Honors the ambient rounding.  A combination worth keeping earns its own
       named verb over this one -- it stays the experimentation path, not the recommended one. */
    void ( *draw_fx_box_ex         )( gui_rect_t box, f32 feather, u32 variant, f32 rate,
                                      f32 depth, f32 phase, f32 rot, f32 cut_dx, f32 cut_dy,
                                      f32 swell, f32 border, u32 col );
    /* The dashed rounded border -- and at a non-zero `speed` (px/sec) the MARCHING ANTS, the
       selection border that scrolls on the clock.  dash/gap are arc-length px (the
       draw_dashed_line vocabulary); the period snaps so whole cycles fit the perimeter and a
       closed border meets itself.  Honors the border-align ambient. */
    void ( *draw_round_rect_dashed )( gui_rect_t box, f32 rounding, f32 thickness,
                                      f32 dash, f32 gap, f32 speed, u32 col );
    /* The BORDER TRACER: one arc of `frac` of the outline (0..1) travelling around it at `rate`
       laps/sec -- indeterminate progress that traces the shape it belongs to rather than
       sitting beside it as a bar.  Mechanically the dashed border with a single cycle spanning
       the whole perimeter.
       draw_border_progress is the determinate twin -- `t` places the arc 0..1 around the border
       from the top-left instead of the clock doing it.  That value IS in the command, so it
       re-tessellates its window as it moves, exactly as a progress bar's fill width does. */
    void ( *draw_border_tracer   )( gui_rect_t box, f32 rounding, f32 thickness,
                                    f32 frac, f32 rate, u32 col );
    void ( *draw_border_progress )( gui_rect_t box, f32 rounding, f32 thickness,
                                    f32 frac, f32 t, u32 col );
    /* A 270-degree arc turning at `rate` revolutions/sec -- the stock indeterminate spinner. */
    void ( *draw_spinner           )( gui_rect_t box, f32 rate, f32 thickness, u32 col );
    /* `n` dots on a circle fitted to `box`.  `col_tail` picks the motion: 0 turns the ring as a
       rigid body -- the mechanical spinner (draw_set_anim_curve with STAIR at `n` steps
       advances one dot per tick) -- while non-zero holds the dots still and marches a colour
       ramp toward col_tail around them, trailing the bright head: the classic tail spinner.
       col & 0x00FFFFFF fades the tail out entirely.  rate 0 is a static dot ring. */
    void ( *draw_dot_spinner       )( gui_rect_t box, u32 n, f32 dot, f32 rate, u32 col,
                                      u32 col_tail );

    /*==============================================  text effects  ==============================================*/

    /* One-call forms of the ambient text edge (draw_set_text_edge): an outline, or a colour
       offset (dx, dy) px behind the run.  SDF fonts only, like the ambient. */

    void ( *draw_text_outline      )( f32 x, f32 y, const char* str, u32 col_text, u32 col_outline );
    void ( *draw_text_shadow       )( f32 x, f32 y, const char* str, u32 col_text, u32 col_shadow, f32 dx, f32 dy );

    /*==================================================  clip  ==================================================*/

    /* The scissor rectangle subsequent draws are cut to; pop restores the previous one. */

    void ( *push_clip )( f32 x, f32 y, f32 w, f32 h );

    /* Same, corners rounded in the fragment (clip_coverage) -- radius clamped to the half-extent. */
    void ( *push_clip_rounded )( f32 x, f32 y, f32 w, f32 h, f32 radius );

    /* Full control: UI_CLIP_* flags (gui.h) pick which corners the radius rounds and which edges
       the feather fades over. */
    void ( *push_clip_ex )( f32 x, f32 y, f32 w, f32 h, f32 radius, f32 feather, u32 flags );

    void ( *pop_clip  )( void );

    /*============================================================================================================
        GUI_CORE -- interaction server  (core/ + interact/)
        This band answers "what is the user doing to this thing right now?" -- hover, click,
        drag, focus -- and tracks it per widget id across frames. It never draws anything; it is
        the shared services every widget, whether stock or custom, is built out of: identity
        (id scopes), the item() state machine over a caller's rect, keyed state, animation, drag
        and drop, io snapshot queries, redraw levers.
        item( id, rect ) -> state is the coordination axis: layout of any kind PRODUCES a
        rect, a widget of any kind CONSUMES one, and this server cannot tell them apart.
    =============================================================================================================*/

    /*=================================  item() -- behavior over a caller rect  =================================*/

    /* item -- the door to building your own widget. Give it a rect and an id, and it hands back
       whether that rect is hovered, pressed, or clicked this frame -- the same behavior every
       stock widget is built on, with none of the drawing decided for you.
       item -- the user-UI behavior seam: run the shared widget interaction state machine over a
       rect the CALLER derived (a canvas() cut, an empty() slot, split/carve panels, custom math)
       and report the resolved state (hover / active / pressed / clicked).  A custom widget is
       rect + item() + draw_*: it hovers, press-captures, clicks, and registers for keyboard nav
       exactly like a stock widget, with the presentation entirely yours.  Owns no layout
       reservation, so it composes with the rect helpers.  invisible_button is item() reduced to
       its click bit; for only a hover tint, use is_mouse_hovering_rect. */
    gui_item_state_t ( *item )( const char* id_str, gui_rect_t r );
    bool ( *invisible_button )( const char* id_str, gui_rect_t r );

    /*===============================  animation service -- keyed value stepping  ===============================*/

    /* Animation service -- the shared toolkit for making a value glide toward a new target
       instead of snapping to it (a hover highlight fading in, a panel sliding open). The
       general value-stepping surface any interface drives transitions with.
       Two models, both keyed on a caller-owned gui_id_t (compose with id_combine to avoid slot
       collisions); storage is proportional to in-flight animations and self-evicts once settled.
       Every call that steps a value raises wants_redraw, so animations keep frames coming under
       idle-skip / boot_pace with no caller bookkeeping.

         anim_f32  -- exponential-decay damper: chase a moving `target` at `speed` (Hz-like; 10 ~=
                      250 ms to 95%, 20 ~= 150 ms).  No definite end -- ideal for hover/state blends
                      and "glide to wherever the target is now".  Retargets smoothly mid-flight.
         anim_start / anim_ease -- fixed-duration eased tween.  anim_start seeds a clock on `id` for
                      `secs` (<= 0 == instant: the next anim_ease reads settled and the caller snaps);
                      anim_ease advances it and returns eased progress in [0,1] to lerp your own
                      from/to with.  Several channels on one id share the clock (depart + ARRIVE
                      together) and the tween has a definite end (*out_active goes false at t==1).
         anim_color / anim_vec2 / anim_rect -- typed dampers (one anim_f32 per component) so a color,
                      point, or rect glides to a new data state without hand-rolling the blend. */
    f32      ( *anim_f32    )( gui_id_t id, f32 target, f32 speed );
    void     ( *anim_start  )( gui_id_t id, f32 secs );
    f32      ( *anim_ease   )( gui_id_t id, gui_ease_t ease, bool* out_active );
    u32      ( *anim_color  )( gui_id_t id, u32 target_abgr, f32 speed );
    gui_vec2_t ( *anim_vec2 )( gui_id_t id, gui_vec2_t target, f32 speed );
    gui_rect_t ( *anim_rect )( gui_id_t id, gui_rect_t target, f32 speed );

    /* The SHADER-side one-shot, for a shape that animates in the FRAGMENT rather than by moving:
       a chip that flashes once, a border whose ants run a single lap, a spinner that ticks through
       one revolution.  anim_time stamps the fx clock when the event happens; anim_once turns that
       stamp into the (rate, phase) an animating draw call is emitted with, holds request_redraw
       while it runs, and returns false on the frame the duration is up -- draw the settled state
       from there.  The emitted bytes never change in between, so unlike anim_ease (which is the
       right tool when the geometry itself moves) the window keeps its retained geometry for the
       whole transition.  The curve the phase is shaped through is draw_set_anim_curve's. */
    f32      ( *anim_time   )( void );
    bool     ( *anim_once   )( f32 t0, f32 duration, f32* out_rate, f32* out_phase );

    /*=================================  identity + item flags + drag and drop  =================================*/

    /* Id scope -- disambiguate widgets that would otherwise share an id.  Widget ids are already
       seeded by the enclosing window / child region automatically, so identical labels in
       different regions never collide; push_id adds a temporary scope level for repeated widgets
       within one region (e.g. rows in a list keyed by index).  Always pair with pop_id.

           for ( i = 0; i < n; ++i ) {
               gui()->push_id_int( i );
               gui()->selectable( name[i], &sel[i] );   // distinct id even if name[] repeats
               gui()->pop_id();
           }

       The "##" / "###" label suffixes are the per-call alternative: "Text##key" displays "Text"
       but ids from the whole string; "pre###key" ids only from "###key", so a changing visible
       prefix (a counter) keeps a stable id. */

    void ( *push_id     )( const char* id_str );
    void ( *push_id_int )( i32 i );
    void ( *pop_id      )( void );

    /* Item flags -- the push-model per-item behavior set (gui_item_flags_t).  push/pop tune every
       widget until popped (and nest); next_item_flag is a one-shot override the very next widget
       consumes, no pop needed.  The mechanism is callsite-free: widgets read the resolved flags at
       emit time, so a new flag never changes a widget signature.  GUI_ITEM_DISABLED is honored
       for every widget today (inert + dimmed).

           gui()->push_item_flag( GUI_ITEM_DISABLED, true );
           gui()->button( "A" );  gui()->button( "B" );    // both disabled
           gui()->pop_item_flag();

           gui()->next_item_flag( GUI_ITEM_DISABLED, true );
           gui()->button( "Only this one" );                 // disabled, no pop needed */

    void ( *push_item_flag )( gui_item_flags_t flag, bool enable );
    void ( *pop_item_flag  )( void );
    void ( *next_item_flag )( gui_item_flags_t flag, bool enable );

    /* disabled_begin / disabled_end -- named-scope shorthand for GUI_ITEM_DISABLED (BeginDisabled
       / EndDisabled).  disabled_begin( true ) dims + inerts the bracketed widgets; ( false ) pushes
       a no-op scope so a conditional disable still balances.  Nests: an inner ( false ) never
       re-enables widgets an outer ( true ) disabled. */
    void ( *disabled_begin )( bool disabled );
    void ( *disabled_end   )( void );


    /* Drag and drop -- drag a piece of data off one widget and drop it on another (an asset
       tile onto a scene view, a tab onto a dock). Typed payload transfer between items (see
       gui_drag_flags_t / gui_drag_payload_t in gui.h). One drag exists at a time; the payload
       bytes are copied.

       USAGE CONTRACT:
         Source -- right after the widget that should be draggable:
           if ( gui()->drag_source_begin( GUI_DRAG_NONE ) )      // true while dragging from it
           {
               gui()->drag_payload_set( "ASSET", &index, sizeof index );   // every frame is fine
               gui()->textf( "Move %s", name );                  // preview widgets follow the cursor
               gui()->drag_source_end();
           }
         Target -- right after any widget that should receive drops:
           if ( gui()->drag_target_begin() )                     // true while a drag hovers it
           {
               const gui_drag_payload_t* p = gui()->drag_payload_accept( "ASSET", GUI_DRAG_NONE );
               if ( p )                                          // non-NULL on the drop frame
                   place_asset( *(const i32*)p->data );
               gui()->drag_target_end();
           }

       drag_payload_accept highlights the target while the types match and returns the payload on
       the release frame (or every hover frame with GUI_DRAG_ACCEPT_PEEK).  drag_active reports a
       drag in flight anywhere; drag_payload_peek inspects it without being a target.  The dock
       tab-strip publishes its tab drags as type "gui.dock_tab" (payload: the window's gui_id_t).

       drag_hint -- call right after ANY widget that could receive this type of drop, hovered or
       not, to give it a thin candidate outline the instant a matching drag starts:
           gui()->invisible_button( "slot", cell );
           gui()->drag_hint( "ASSET" );                          // every compatible slot, always
           if ( gui()->drag_target_begin() ) { ... }             // this one, only once hovered

       It never gates accepting the payload -- that is still drag_target_begin / drag_payload_accept,
       which owns the bolder "accepted here" ring once the cursor actually arrives.  Skip it for a
       target whose candidacy is not worth advertising ahead of time (a widget-to-widget drop where
       only one exact target ever makes sense). */

    bool                      ( *drag_source_begin   )( gui_drag_flags_t flags );
    void                      ( *drag_source_end     )( void );
    bool                      ( *drag_payload_set    )( const char* type, const void* data, u32 size );
    bool                      ( *drag_target_begin   )( void );
    const gui_drag_payload_t* ( *drag_payload_accept )( const char* type, gui_drag_flags_t flags );
    void                      ( *drag_target_end     )( void );
    bool                      ( *drag_active         )( void );
    const gui_drag_payload_t* ( *drag_payload_peek   )( void );
    void                      ( *drag_hint           )( const char* type );

    /*==========================  multi-select -- clicks + modifiers -> one range action  =======================*/

    /* Multi-select protocol -- turns raw clicks plus modifier keys into one resolved selection
       change, the way Windows Explorer's file list behaves: click to replace the selection,
       Ctrl to toggle one item, Shift to select a range. (interact/gui_msel.c) -- the Explorer
       click/modifier rule (plain
       replaces, Ctrl toggles, Shift ranges from the anchor, Ctrl+Shift adds, Shift+arrow extends,
       Ctrl+A selects all) over CALLER-owned selection storage.  Bracket a list with
       msel_begin(id, full_count) .. msel_end(); rows report through the stock msel_item (chrome)
       or, for a custom presentation (grid tile, tree line), rect + item() + msel_feed(index, st).
       msel_end returns the frame's resolved index-range action; msel_apply plays it onto a dense
       bool array (other storage switches on .op itself).  Ranges are index math, so actions span
       rows a virtualized list (rows_clip) never emitted. */
    void       ( *msel_begin )( const char* id_str, i32 count );
    void       ( *msel_feed  )( i32 index, gui_item_state_t st );
    gui_msel_t ( *msel_end   )( void );
    void       ( *msel_apply )( gui_msel_t act, bool* sel, i32 count );

    /*===========================  queries -- io snapshot, item state, redraw state  ============================*/

    /* IO accessors -- read the same per-frame keyboard/mouse snapshot the widgets themselves
       read from, instead of querying app() directly and risking a value that disagrees with
       what gui just saw (a stale frame's mouse position, an input gui already captured). For UI
       / tool code that would otherwise re-query app() and so bypass gui's frame timing and its
       input capture.

       want_capture_mouse / want_capture_keyboard are the fence: a true return means gui owns the
       device this frame (the cursor is over a window, a widget is dragging, or a field is focused),
       so non-UI code should NOT also act on it.  Gate direct app() input reads in gameplay / tools
       on the inverse:

           if ( !gui()->want_capture_keyboard() && app()->key_pressed( APP_KEY_SPACE ) )
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
    bool ( *is_mouse_hovering_rect   )( gui_rect_t r );

    /* Last-item introspection -- ask follow-up questions about the widget you just emitted
       ("was that clicked? is it still focused?") instead of it returning everything at once
       (the ImGui IsItem* family) -- each reports on the widget just emitted,
       so call immediately after it.  hovered / active / clicked / focused mirror the widget's own
       interaction; activated / deactivated are the press / release edges (deactivated is the natural
       "commit on release" seam); visible is true when any of the item's rect survives the region
       clip; get_item_rect returns its screen rect (GetItemRectMin/Max/Size in one). */
    bool       ( *is_item_hovered                )( void );
    bool       ( *is_item_active                 )( void );
    bool       ( *is_item_clicked                )( void );
    bool       ( *is_item_focused                )( void );
    bool       ( *is_item_activated              )( void );
    bool       ( *is_item_deactivated            )( void );
    bool       ( *is_item_deactivated_after_edit )( void );
    bool       ( *is_item_visible                )( void );
    gui_rect_t ( *get_item_rect                  )( void );

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
       the directional sizers, a text field shows the I-beam).  cursor_set lets UI code request
       a shape gui cannot infer -- e.g. APP_CURSOR_HAND over a custom clickable -- for this frame;
       the last request wins and is flushed to the OS window under the pointer while gui owns the
       mouse, then reset to APP_CURSOR_ARROW next frame.  get_mouse_cursor reads the current request. */
    void         ( *cursor_set       )( app_cursor_t c );
    app_cursor_t ( *get_mouse_cursor )( void );

    /* set_keyboard_focus -- queue a programmatic focus request: the next focusable widget emitted
       (a text input box) takes keyboard focus as if clicked.  Call just before emitting the
       widget; the request persists across frames until a focusable widget consumes it, so a
       request made after this frame's field lands on the same field next frame (the "refocus
       after Enter" console pattern). */
    void         ( *set_keyboard_focus )( void );

    /* set_edit_cursor_end -- queue a caret request: the focused text field seats its caret at the
       end of its buffer (selection collapsed) the next time it runs.  Call after replacing the
       field's buffer programmatically (history recall, tab completion); the request persists
       across frames until a focused field consumes it. */
    void         ( *set_edit_cursor_end )( void );

    /* set_edit_key_hook -- register a key passthrough for the next FOCUSED text field: the hook
       (gui_edit_key_fn, gui.h) runs before the field's own key handling for every key down this
       frame, and a key it consumes is cleared from the frame io.  One-shot: re-register just
       before emitting the field each frame.  This is the Quake-console seam -- history on
       Up/Down, completion on Tab, scrollback on PgUp/PgDn/Ctrl+Home/Ctrl+End -- while every
       unconsumed key edits the line as normal. */
    void         ( *set_edit_key_hook )( gui_edit_key_fn fn, void* user );

    /* wants_redraw -- true when at least one animated widget has not yet reached its target this
       frame (the currently bound context's flag).  boot_pace already folds this across every
       context internally; the query remains for hosts that run their own pacing. */
    bool ( *wants_redraw )( void );

    /* request_redraw -- tell gui "something changed that the UI needs to reflect next frame."
       One-shot: mark the bound context so the NEXT frame_begin returns dirty
       and runs a full emit.  Self-clearing (the pin is set_force_redraw below).  Call it when a
       state change made DURING this build only the next build can show -- the click that switches
       which screen is emitted, a custom widget (gui()->item) mutating the model it draws.  Input
       edges dirty only the frame they land on; without this, that next frame reads clean and
       render() replays the stale cached geometry until the mouse moves again.

       GUI STATE DIRTIES ITSELF -- YOUR STATE IS YOURS TO DECLARE.  Every gui verb that mutates
       something displayed raises this flag internally: window_set_open, popup_open, the dock verbs
       (dock_window / undock / clear / load / maximize / inset), viewport open / close / resize,
       ctx_set_listening, set_keyboard_focus, window_set_nav, set_edit_cursor_end, theme / style /
       font changes, scroll, and every anim step.  So request_redraw is for exactly ONE residual
       case: the HOST mutating its own model between or during builds.  Stock widgets never need it
       (they re-read state in the same frame).

       Internal raises fire on the state-change EDGE, not on the call -- a verb re-asserted every
       frame with an unchanged value must not raise, or the UI can never go idle.  Any new mutating
       verb follows the same rule; a build-only ambient call (push_style_*, the next_item_* one-shots)
       must never raise, since being inside a build already means the frame is dirty. */
    void ( *request_redraw )( void );

    /* frame_dirty -- true when the current frame must perform a full widget emit: input changed,
       an animation is in flight, or last frame's render found a structural change.  This is the
       value frame_begin returns; the query remains for reads later in the frame (e.g. to pair
       external per-emit work such as a scene-texture flip with an actual emit). */
    bool ( *frame_dirty )( void );

    /* volatile_live -- true while at least one volatile block (volatile_cb) would actually patch
       on an idle frame: registered, on screen, its window's cached slot current.  A host that
       runs its OWN pacing and block-waits on input once wants_redraw/frame_dirty settle must add
       this to the gate: volatile blocks advance only when a frame runs, so a blocking wait
       freezes them until a timeout / spurious wakeup and the animation stutters at the wait
       interval.  boot_pace folds this in internally, same as wants_redraw. */
    bool ( *volatile_live )( void );

    /* set_force_redraw -- pins frame_dirty (and so frame_begin's return) true every frame,
       defeating the retained-cache clean-frame skip so the UI rebuilds and re-renders
       unconditionally.  Off by default.  Two uses: a debug lever to isolate a "did not update
       until input moved" symptom from a real emit bug, and the legitimate live-data pin -- a host
       whose sim mutates displayed state every frame (play mode) sets it so panels track the sim. */
    void ( *set_force_redraw )( bool on );

    /* force_redraw -- current state of the set_force_redraw override. */
    bool ( *force_redraw )( void );

    /*============================================================================================================
        GUI_SURFACE -- root surfaces  (core/gui_surface.c + flow/gui_region.c + interact/gui_feature.c)
        This is the foundation every window, panel, and HUD element in the GUI is ultimately
        built from: a raw block on screen that has an identity, a clip rect, and a place in the
        hover/z order -- and nothing else. pane_begin is that bare block, region_begin adds
        persisted scroll + a layout, and chrome's window_begin (GUI_CHROME) is the same pane plus
        a pool record and stock policy on top. The feat_* kit assembles window features (move /
        resize / collapse / maximize / clamp) over any pane: custom chrome is composition, not a
        privileged layer -- a window is not special, it is just one particular combination of
        these pieces.
    =============================================================================================================*/

    /*===================================  surfaces -- panes, root regions + scroll  ===================================*/

    /* pane_begin / pane_end -- the smallest possible on-screen surface: an id and a clip rect,
       competing for hover/z like a window but with no scroll, no background, no persistence --
       for a caller assembling entirely its own chrome from scratch. The MINIMAL top-level
       surface occupant (gui_pane_t, gui.h): the
       raw block every window is built from, for callers assembling their own chrome.  Opens
       identity (items inside attribute to this pane), enters the hover/z contest at the tier's
       band (same contest windows and popups compete in), and pushes the base clip (draw + hit)
       to the rect -- NOTHING else: no pool record, no persistence, no layout, no background
       paint, no scroll.  The caller owns every pixel (stock_* / draw_* over carved rects) and any
       cross-frame state; open flow inside with flow_begin( pane.rect ) if wanted.  Flags
       honored: GUI_WIN_NO_INPUT (pure display), GUI_WIN_NO_CLIP, GUI_WIN_DEBUG_BAND.  vp
       GUI_VP_MAIN = primary surface (GUI_VP_INVALID and a closed viewport map to it).
       Root-level, never nests; always pair with pane_end.
       region_begin below = this + persisted scroll + a layout; window_begin = this + the
       persisted record + stock chrome. */
    gui_pane_t ( *pane_begin )( const char* id_str, gui_rect_t r, gui_region_tier_t tier,
                                i32 vp, gui_win_flags_t flags );
    void       ( *pane_end   )( void );

    /* region_begin / region_end -- a fixed-position box for HUD-style content: scrollable and
       laid out like a window, but pinned at a screen rect you choose rather than movable by the
       user. A root-level layout region: an explicit screen rect with no
       window chrome (no title, no drag, no dock, no z-order competition, no pool record).
       It is the third caller of the same scroll-region engine window_begin and child_begin sit
       on, stripped to just a rect + persisted scroll/content state, for a HUD-style element that
       needs a fixed, caller-positioned box rather than a movable window -- the perf overlay is
       the reference case.  Exactly two modes per axis, chosen by the sign of w/h: > 0 pins it
       exactly, <= 0 autosizes it every frame to last frame's measured content (like child_begin's
       AutoResizeY).  No user-driven third mode: a region has no chrome to grab, so
       GUI_WIN_CHILD_RESIZE_X/_Y (child_begin's drag-grip flag) asserts if passed here.  Unlike
       window_begin / child_begin, it takes no parent
       region -- call it directly at the top of a frame.  Paints on viewport `vp` (rect in that
       surface's client space; GUI_VP_MAIN = the primary, GUI_VP_INVALID and a closed viewport
       map to it) at the z tier picked by `tier` (gui_region_tier_t: MID over windows / under
       popups, BG, FG); interactive by default -- competes for hover in the same z contest as
       windows (opt out with GUI_WIN_NO_INPUT; see gui_region.c).  Always returns true; always
       pair with region_end. */
    bool ( *region_begin )( const char* id_str, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                            i32 vp, gui_win_flags_t flags );
    void ( *region_end   )( void );

    /* scroll_by -- nudge the currently open region's scroll offset by (dx, dy) px (0=top origin);
       a large delta drives to an edge, so +BIG reaches the bottom / tail and -BIG the top.  Applied
       THIS frame (re-bases the live pen), so call it right after opening the region, before content
       -- no one-frame lag, unlike the wheel.  Pairs with GUI_WIN_ANCHOR_BOTTOM to drive a console's
       wheel + PageUp/Down + jump-to-tail keys without any offset bookkeeping in the caller. */
    void ( *scroll_by     )( f32 dx, f32 dy );

    /*=================================  window features as mechanisms  =================================*/

    /* The feat_* kit -- the individual behaviors a window is made of (dragging by its titlebar,
       resizing by its edge, collapsing, maximizing), each offered as its own freestanding
       function so a custom panel can pick and choose exactly the ones it wants over a plain
       pane, instead of getting the whole window package or none of it. Every window feature as
       a freestanding id-keyed mechanism, so chrome is assembled feature by feature over a pane
       -- anything can be a move handle, a collapse, or a maximize.  State rule: in-flight gesture state
       is arbitrated by active_id (one drag at a time); PERSISTENT state is the caller's
       pointers -- you see every byte.  Call these inside the owning pane/window bracket
       (hover gating reads the ambient scope).  The open latch needs no mechanism: it is a
       caller bool your close button clears; scroll is region_begin ("region owns scroll").

           gui_pane_t p     = gui()->pane_begin( "tool", st->rect, GUI_REGION_MID, GUI_VP_MAIN, 0 );
           gui_rect_t r     = p.rect;
           gui_rect_t title = gui_rect_cut_top( &r, 26.0f );          // titlebar = a band...
           gui()->feat_move( p.id, title, &st->rect.x, &st->rect.y ); // ...that drags
           if ( gui()->stock_button( gui_rect_cut_right( &title, title.h ), "x##c" ) )
               st->open = false;                                      // close = your bool
           r.h = gui()->feat_collapse( p.id, !st->folded, 26.0f, r.h ) - 26.0f;
           gui()->feat_resize( p.id, &st->rect, GUI_RESIZE_R | GUI_RESIZE_B, 120, 80 );
           ... body: carve r, or flow_begin( r ) ...
           gui()->pane_end();

       feat_move     -- drag handle over any rect: press in `handle` (deferred: click vs
                        drag) grabs; the caller-owned origin follows the cursor.  True on
                        frames it moved.
       feat_resize   -- edge-resize the caller-owned rect: `edges` masks the exposed sides
                        (GUI_RESIZE_L/R/T/B), min_w/h floors against the grabbed edge's
                        pinned far side.  Returns the live (hot/dragging) edges.
       feat_collapse -- tweened height channel over a caller bool: full_h open, head_h
                        closed, eased between after YOUR toggle.  Returns this frame's height.
       feat_maximize -- rect <-> work-area swap (work passed IN -- the B rule): saves *r
                        into *restore on the way up, tweens both directions, tracks a
                        resizing work area while maximized.  Writes *r every call.
       feat_clamp    -- boundary policy over passed-in bounds: the handle row stays
                        reachable (never above work's top; `margin` sliver at other edges). */
    bool ( *feat_move     )( gui_id_t id, gui_rect_t handle, f32* x, f32* y );
    u8   ( *feat_resize   )( gui_id_t id, gui_rect_t* r, u8 edges, f32 min_w, f32 min_h );
    f32  ( *feat_collapse )( gui_id_t id, bool open, f32 head_h, f32 full_h );
    void ( *feat_maximize )( gui_id_t id, bool maximized, gui_rect_t* r, gui_rect_t* restore,
                             gui_rect_t work );
    void ( *feat_clamp    )( gui_rect_t* r, gui_rect_t work, f32 margin );

    /*============================================================================================================
        GUI_RECT -- rect kit  (stateless carve math)
        Plain math for slicing screen space into rectangles -- nothing here remembers state, opens
        a region, or draws a pixel; it only computes rects for you to hand to a widget, item(),
        push_layout_overlay, or a draw_* call. Pure rect producers: no pen, no region state, no
        draw -- feed the results to elements, item(), push_layout_overlay, or draw_* directly. The
        inline half of this library (cut / inset / align / anchor_box + the geometry types) lives
        in gui_rect.h.
    =============================================================================================================*/

    /* content_rect -- the current region's available area as a screen rect (cursor_screen_pos joined
       with content_avail).  split -- carve a rect into panels along an axis using the overloaded
       column unit ( >1 px, ==1 fill, (0,1) fraction ), writing each panel rect into out[] and
       returning the count ( <= GUI_LAYOUT_COLS ).  Pure rect math: fill each panel with
       push_layout_overlay, and nest by splitting a returned rect again.  Single-pass and known-size --
       it never measures content, so size panels with px / fraction / fill, not content-driven sizes. */
    gui_rect_t ( *content_rect )( void );
    u32        ( *split        )( gui_rect_t area, gui_axis_t axis, const f32* sizes, f32 gap, gui_rect_t* out );

    /* carve -- a whole nested partition from one flat f32 `form`: the recursive form of split.  The
       form is a GUI_END-terminated list in the same overloaded unit as cols, with GUI_CUT_X /
       GUI_CUT_Y sentinels marking which tracks subdivide (a size followed by a CUT is a container of
       that size on the named axis; otherwise a leaf).  Opens with a leading CUT filling `area`.  Leaf
       rects land in out[] in reading order; returns the leaf count ( <= max ).  One resolve per
       container, no per-leaf storage -- store a form as data and carve it each frame. */
    u32        ( *carve )( const f32* form, gui_rect_t area, f32 gap, gui_rect_t* out, u32 max );

    /* anchor -- place a child rect inside `parent` from a normalized anchor frame (UE4 Slate model),
       the general free-placement primitive for overlays / HUDs.  Per axis: min == max point-pins a
       fixed `size` child to that parent fraction (hung off the line by `pivot`, shifted by `off`);
       min < max stretches the child between the two fractions with `off` as per-edge insets.  Pure
       rect math -- fill the result with push_layout_overlay or draw into it.  The corner / edge cases
       are the inline gui_rect_align / gui_anchor_box (gui_rect.h); reach for anchor when you need a
       fraction-relative position or a stretch-with-margins band.  See gui_anchor_t for the fields. */
    gui_rect_t ( *anchor )( gui_rect_t parent, gui_anchor_t a );

    /*============================================================================================================
        GUI_FLOW -- layout engine  (flow/)
        This is the part of the GUI that decides WHERE things go. Think of it as a pen that
        walks down the window, handing each widget in turn a rect to draw itself into, according
        to whichever template (stack, columns, grid, form, pack) is currently declared. The
        stateful pen: templates (stack / cols / grid / form / pack), sizing, avail,
        row virtualization, and the rect<->flow seams (empty, canvas, push_layout_overlay).
        Produces rects and opens regions; draws nothing, and no widget core depends on it.
    =============================================================================================================*/

    /*================================  containers -- child boxes + sub-layouts  ================================*/

    /* Child regions -- a smaller scrollable panel nested inside the current window, with its own
       scroll and clipping, like a window within a window. A nested scrollable layout box inside
       the current window (or another
       child).  child_begin carves a box of height h (width w, or the remaining content width
       when w <= 0) from the layout pen, clips and scrolls its contents independently, and
       gives it its own scrollbar; flags take the GUI_WIN_*SCROLL policy bits.  h <= 0
       auto-sizes the height to the content (AutoResizeY).  GUI_WIN_CHILD_RESIZE_X / _Y add a
       draggable grip on the right / bottom border (flow children only): that axis becomes
       user-owned and persisted, seeded from w/h then driven by the drag, the way a window owns
       its size.  window_set_next_size_constraints (GUI_CHROME: window/) bounds the resolved size, so an
       auto-sized box can grow with its content up to a max height and then scroll.  Always pair
       with child_end -- the parent layout resumes directly below the box.  Fill it with any
       widgets (e.g. selectable rows for a list box).  Always returns true. */

    bool ( *child_begin )( const char* id_str, f32 w, f32 h, gui_win_flags_t flags );

    /* Sub-layout -- pack several widgets into what would normally be a single cell (e.g. put a
       button and a label side by side inside one column of a grid). Carve the next cell into
       its own little layout, the way a window or child hosts one, but transient: no scroll, no
       clip, no persistent state, no frame.  push_layout consumes
       one cell (advancing the parent like any widget), opens a layout filling it (default single
       column; shape it with row / grid / widgets inside), and pop_layout closes it -- the parent
       resumes at the following cell.  The cell is one standard line tall unless the row height was
       declared larger first; the sub-layout does not grow the parent to fit, and does not clip.
       Always pair, like push_id / pop_id.

           gui()->row_cols_n( 0, 3 );                     // 3 columns
           gui()->push_layout();                          // column 0 becomes a sub-layout...
               gui()->button("A"); gui()->button("B");  // ...stacked inside that one cell
           gui()->pop_layout();
           gui()->text("col 1");  gui()->text("col 2"); */

    void ( *push_layout )( void );

    /* push_layout_overlay -- open a sub-layout over an explicit screen rect rather than the next
       template cell; the parent flow is left untouched (no cell consumed).  The seam an external
       layout pass (a two-pass "layout island") uses to hand a resolved box back to the immediate
       widgets, which fill it like any region.  Pair with pop_layout. */
    void ( *push_layout_overlay )( gui_rect_t rect );

    void ( *pop_layout  )( void );
    void ( *child_end   )( void );

    /*==============================  layout verbs, sizing, virtualization, seams  ==============================*/

    /* Layout -- the template family. Every region (a window body, a child, a sub-layout) must
       first be told HOW to arrange the widgets placed in it -- as a single scrolling column
       (stack), fixed side-by-side tracks (cols), a matrix of cells (grid), a "label: control"
       form, or a toolbar-style run of natural-sized items (pack) -- and every widget from then
       on fills whatever cell that template hands it, with no idea which shape it is in.
       Declare the active region's next-item methodology (its "mode"), then shape it.
       A region opens UNDECLARED: the first header below names the mode (stack / columns / grid /
       form / ...), and a widget emitted before any header is a usage error (debug assert; release
       falls back to a stack).  The template then persists + repeats for every widget until set
       again.  Sizes use one overloaded f32: >1 px, (0,1) fraction of the available space, 1 fill
       (equal share of the rest), 0 natural (zero-width; reserved), <0 ends the list (GUI_END).
       Widgets fill whatever cell they are handed, agnostic to the shape.

           gui()->row_cols_n( 0, 2 );  gui()->button("A");  gui()->button("B");  // two columns
           gui()->row_cols( 24, (f32[]){ 200, 1, GUI_END } );                     // 200px + fill

       stack()      -- single full-width flex column, scrolling: the canonical vertical-list header,
                         the shape most regions want, named explicitly like every other mode.
       cols()       -- N explicit column tracks (GUI_END-terminated), auto height, scrolling.
       cols_n()     -- n equal flex columns, auto height.
       form()       -- a stack with a fixed-width label track on `side`: the "Label  [control]"
                         form header (label_w <= 0 = plain stack).
       layout_default() -- clear back to a plain stack (one flex column, no field split); the
                         single "reset everything" verb.
       row()        -- a stack with an explicit row height (0 = auto).
       row_cols()   -- explicit per-column tracks (GUI_END-terminated) of height row_h: cols + height.
       row_cols_n() -- n equal columns of height row_h: cols_n + height.
       row2/3/4()   -- fixed-arity weighted columns (auto height): row2( 0.3f, 0.7f ).
       field_split()  -- labeled widgets split their cell into a label + control track (overloaded
                         units, label left or right); input_text / slider_float / checkbox then lay
                         out as an aligned "Label  [control]" form from a single call.
       field_label_left() / field_label_right() -- field_split sugar: a fixed-width label column on
                         the left / right with a flex control filling the rest (0 = off).
       field_set() / field_get() -- the ambient label layout (gui_field_t): the shared authority
                         every labeled widget's own label reads, so all forms align from one set and
                         there is no second "_label" widget set.  field_set(NULL) restores the
                         default; field_get returns the live struct to poke (e.g. .hide to drop every
                         label at once).
       skip_label() -- one-shot: drop the ambient label for the next widget only (labels stay on
                         globally) -- the per-widget mirror of field.hide.
       field_row()  -- the label seam a widget routes its own label through: paints the label per the
                         ambient field and arms the next cell for the control (bare when hidden).

       Grid mode -- cols x rows partition a bounded box (the region content from the pen to its
       bottom) into a fixed matrix, both axes resolved up front; widgets fill cells row-major and
       nothing scrolls.  For titlebars, split panes (cell -> child_begin), dashboards, image grids.
       grid() takes the full descriptor (cols + rows); grid_cells() is the uniform nc x nr case.

       gui()->grid_cells( 3, 2 );  for (i<6) gui()->button(name[i]);  // 3x2 of buttons
       grid()       -- cols x rows from a gui_grid_t descriptor (tracks + gaps + align).

       Pack mode -- the print run: place items one after another along an axis at natural size, the
       widget sizing itself (vs columns/grid, where the cell sizes the widget).  pack_size() overrides
       the next item's main-axis measure (resolved against the space left on the line); pack_nextline()
       breaks to a fresh line.  The toolbar / tag-row / inline-controls case.

       gui()->bar();  gui()->button("Save");  gui()->button("Open");   // a toolbar
       bar() / strip() -- open a run: horizontal (the toolbar) / vertical.
       pack_size()  -- next packed item's main-axis size (0 natural, 1 fill, (0,1) frac, >1 px).
       pack_nextline() -- break the run to a new line.
       pack_wrap()  -- opt the run into auto-wrap: a natural / fixed item that overruns the line
                       breaks to a fresh one first (flex-wrap; a fill always fits, never wraps). */

    void         ( *layout_default    )( void );
    void         ( *stack             )( void );
    void         ( *row               )( f32 row_h );
    void         ( *cols              )( const f32* tracks );
    void         ( *cols_n            )( u32 n );
    void         ( *row_cols          )( f32 row_h, const f32* tracks );
    void         ( *row_cols_n        )( f32 row_h, u32 n );
    void         ( *row2              )( f32 a, f32 b );
    void         ( *row3              )( f32 a, f32 b, f32 c );
    void         ( *row4              )( f32 a, f32 b, f32 c, f32 d );
    void         ( *form              )( gui_label_side_t side, f32 label_w );
    void         ( *field_split       )( gui_label_side_t side, f32 label, f32 control );
    void         ( *field_label_left  )( f32 width );
    void         ( *field_label_right )( f32 width );
    void         ( *field_set         )( const gui_field_t* f );
    gui_field_t* ( *field_get         )( void );
    void         ( *skip_label        )( void );
    void         ( *field_row         )( const char* label );
    void         ( *grid              )( gui_grid_t desc );
    void         ( *grid_cells        )( u32 ncols, u32 nrows );
    void         ( *bar               )( void );
    void         ( *strip             )( void );
    void         ( *pack_size         )( f32 unit );
    void         ( *pack_nextline     )( void );
    void         ( *pack_wrap         )( void );

    /* push_layout_state / pop_layout_state -- save the region's declared shape (mode + template +
       modifiers) and restore it later, so a helper that switches into bar() / grid() / whatever
       for its own widgets can hand the caller's shape back verbatim, instead of the caller having
       to remember and re-declare it (stack() is not always right -- the caller may have been mid
       cols() or grid()).  Always pair, like push_id / pop_id; small fixed depth, coarse scope
       brackets only.

           gui()->push_layout_state();
               gui()->bar();
               gui()->button( "Save" );  gui()->button( "Open" );
           gui()->pop_layout_state();       // caller's stack() / grid() / cols() ... is back */

    void ( *push_layout_state )( void );
    void ( *pop_layout_state  )( void );

    /* align() -- set the content alignment within each cell (gui_align_t, LEFT | TOP by default).
       Persists like the row template and is independent of the columns: row() / row_cols() leave it
       untouched, layout_default() clears it.  Governs where natural-sized content sits (a text run, a
       checkbox box, a button's label); a frame-filling widget still fills its cell.  The `align`
       field of a gui_grid_t descriptor sets the same thing as part of the grid() call.

           gui()->row2( 0.5f, 0.5f );  gui()->align( GUI_ALIGN_RIGHT );   // right-aligned columns

       next_item_fit() -- one-shot override of the next cell item's size (STACK / COLUMNS / GRID),
                      instead of its implicit natural_w signal.  Same overloaded unit as a column
                      track (>1 px, (0,1) fraction, 1 fill, 0 explicit natural); the fit-then-align
                      pair -- align seats whatever box this (or the widget's own natural_w) picks.

           gui()->next_item_fit( 1.0f ); gui()->button( "Save" );  // stretch across its column

       next_item_h() -- one-shot override of the next item's HEIGHT (the vertical twin), resolved
                      against the room left below the pen: >1 px, 1 fill the rest of the region,
                      (0,1) a fraction of it, 0 the widget's own h.  Flow: lands when the item
                      opens its row; ignored mid-row and in grid cells (the matrix height wins).

           gui()->next_item_h( 1.0f ); gui()->button( "Fill" );    // rest of the region

       next_item_rect() -- one-shot: the next widget's cell IS this exact screen rect, from any
                      producer (a carve / split / anchor leaf, a hand-cut band).  The rect-first door
                      onto the WHOLE widget set -- any gui_* widget takes its rect from here instead
                      of the flow template, so one call site works under manual, carved, or flow
                      layout, and the stock_* rect renders are optional sugar over it.  Pure placement: no
                      pen advance, no highwater, no declared mode needed; reserve with empty() if a
                      region must size around it.

           gui()->next_item_rect( leaf );  gui()->button( "Save" );  // a carved cell, a flow widget

       next_item_align() -- one-shot align for the next item only (flexbox's align-self), over the
                      region's persistent align(); restored at the following emit, so call it
                      immediately before the item.

       same_line() -- keep the next widget on the line just emitted instead of breaking to a new
                      row; it takes its natural width.  `spacing` is a gap, not a track size: its
                      own natural is a literal 0 (flush), and < 0 defers to the theme default gap.
                      Mirrors ImGui::SameLine.  new_line() is its vertical mirror: a fresh line of
                      height h, 0 = literal zero-height break, < 0 = the theme's line height.

           gui()->button("OK");  gui()->same_line( 0.0f );  gui()->button("Cancel");
           gui()->text("A");  gui()->new_line( -1.0f );  gui()->text("B");  // one blank line between

       Paintless spacers -- cell-consuming composition that emits nothing at all:
       skip()      -- leave one blank cell (a hole; the natural way to step over a grid slot).
       new_line()  -- break to a fresh blank line of height h (see same_line above).
       The rules that DRAW into a cell -- separator() / separator_text() -- are chrome's, over
       these same cells; they sit with the other painting widgets in GUI_CHROME. */

    void ( *align            )( gui_align_t a );
    void ( *next_item_fit    )( f32 unit );
    void ( *next_item_h      )( f32 unit );
    void ( *next_item_rect   )( gui_rect_t r );
    void ( *next_item_align  )( gui_align_t a );
    void ( *same_line        )( f32 spacing );
    void ( *stack_same_line  )( f32 spacing );
    void ( *skip             )( void );
    void ( *new_line         )( f32 h );

    /* canvas() -- reserve a full-width drawing area of `height` px in the layout (height <= 0 fills
       the rest of the region) and return its screen rect, for custom geometry drawn with the
       draw_* / path_* calls.  It flows like any widget and the window clips it. */
    gui_rect_t ( *canvas )( f32 height );

    /* Sizing (sz_) -- helpers that turn a plain intent ("n rows tall", "wide enough for this
       text") into an actual pixel number, so call sites don't hardcode magic numbers that drift
       out of step with the theme. The one family that turns intent into a pixel dimension; layout verbs
       (row, cols, child_begin, window_set_next_size) consume what these produce.  Grid-first,
       in order of preference:

       sz_u( n ) -- n grid quanta in pixels (the theme's grid_quantum lattice, 4 by default):
       the unit-first spelling for any authored px size (tracks, row heights, child sizes), so
       geometry stays on the theme lattice and retunes with it.  q <= 1 degenerates to raw px.

       sz_row_gap() -- the vertical gap the flow places between consecutive rows, and the
       top/bottom pad a window body / child opens with.  Owed once above the first row, once
       below the last, and once between every pair.

       sz_rows_h( n ) -- fixed box height for n uniform WIDGET_H rows stacked with the default
       pad/gap (a fixed-size list of buttons/fields, a popup sized to its item count).  Reads
       through the style stack, so inside a scale_push scope it speaks that step's metrics.

       sz_child_rows_h( n ) -- the OUTER height to give child_begin / a bare window so its interior
       holds exactly n such rows.  sz_rows_h is the interior; a container also carves its border off
       the box, so passing sz_rows_h( n ) to a child clips the last row -- use this instead when the
       row count must be exact inside a child.

       sz_scale_row( s ) -- one row height at a named ramp step (gui_scale_t) without pushing
       the scope: size a header band or custom chrome to a step.

       Content-fit escape hatches (prefer letting the layout measure via natural sizing):

       sz_fit_row / sz_fit_col -- content px plus the standard margin a row / cell puts around
       its content; fit( 0 ) is the bare margin (the "size without content").
       sz_line_h() -- the raw font line advance, for text-shaped custom-draw rects.  Text
       measurement itself lives with the draw family (text_size), not here.
       sz_chars( n ) -- width of n characters (n * a representative glyph advance), for sizing a
       field to a fixed character count without measuring a placeholder string.

           gui()->row( gui()->sz_fit_row( 128 ) );               // a row sized for a 128px image
           f32 w = gui()->sz_fit_col( gui()->text_size("Name").x ); // a column sized to a label */
    f32 ( *sz_u            )( f32 n );
    f32 ( *sz_row_gap      )( void );
    f32 ( *sz_rows_h       )( u32 n );
    f32 ( *sz_child_rows_h )( u32 n );    /* outer child/window height to hold exactly n rows */
    f32 ( *sz_scale_row    )( gui_scale_t s );
    f32 ( *sz_line_h       )( void );
    f32 ( *sz_chars        )( f32 n );
    f32 ( *sz_fit_row      )( f32 content_h );
    f32 ( *sz_fit_col      )( f32 content_w );

    /* content_avail() -- remaining free space in the current region from the layout pen: the width
       a flex widget would fill and the height left before the region bottom.  The ImGui
       GetContentRegionAvail analogue -- size a child_begin to the leftover, or lay out by hand. */
    gui_vec2_t ( *content_avail )( void );

    /* view_avail() -- content_avail clamped to the visible view.  The content column can run wider
       than the view when a sibling overflowed horizontally; content_avail reports that full column
       (right for passive rows), this never exceeds the visible track (right for sizing an opaque
       interactive surface -- a child box, a text editor -- which must not seat itself under the
       scrollbar gutter).  Scroll-free: a box sized by it keeps its width while the region scrolls. */
    gui_vec2_t ( *view_avail )( void );

    /* rows_clip -- makes a list with thousands of rows cost only what the visible handful cost,
       by telling the caller which row indices are actually on screen so it only needs to emit
       those. Fixed-pitch row virtualization (the ImGuiListClipper analogue).  Reserves
       `count` rows of layout extent, skips the offscreen head, and returns the visible
       [first, last) range; the caller emits only those rows, so a 10000-row list costs what its
       visible slice costs.  Rows must be fixed pitch: row_h 0 defaults to the template's fixed
       row_h (row_cols) else WIDGET_H -- pass the true height when rows are anything else.  Scroll
       range and extent measure as if every row emitted; nav only sees the emitted rows.

           gui_span_t s = gui()->rows_clip( count, row_h );
           for ( i32 i = s.first; i < s.last; ++i ) { ...emit row i... }

       rows_clip_end() -- jump past the reserved tail; needed only when more content follows the
       run in the same region (a footer), else omit. */
    gui_span_t ( *rows_clip     )( i32 count, f32 row_h );
    void       ( *rows_clip_end )( void );

    /* cursor_screen_pos -- screen position where the next item would land (GetCursorScreenPos): anchor
       custom draw_* geometry to the pen.  empty -- reserve a w x h block and return its screen rect
       (the ImGui Dummy analogue): blank space, or a slot to fill with custom draw / make clickable
       with invisible_button.  `w` is the main-axis size (honored in pack / same_line; column flow
       sizes to the track). */
    gui_vec2_t ( *cursor_screen_pos )( void );
    gui_rect_t ( *empty             )( f32 w, f32 h );

    /* flow_begin / flow_cell / flow_end -- opens the layout engine (stack / cols / grid / ...)
       inside any rect you already have, however you got it -- a carved slice, a split panel, a
       hand-computed box -- so you get flow's automatic placement without needing a window or
       child region first. The named rect <-> flow seam pair.  flow_begin opens
       the layout engine inside ANY rect, however it was produced (cut_* algebra, split, carve,
       anchor, a flow cell, custom math) -- push_layout_overlay under its first-class name.
       flow_cell takes the next flow element back out AS a rect (w / h <= 0 = natural: the
       resolved track width / one standard row), so the two verbs cross the seam in both
       directions and nest to the layout stack depth -- the recursive contract:

           carve -> flow_begin -> flow_cell -> carve -> flow_begin -> ...

       A fresh flow opens UNDECLARED: name a mode inside (stack / cols / ...).  Flow never
       scrolls -- when the carved area needs scroll / clip / persistence, open a core surface
       first (region_begin) and flow inside it.  Always pair flow_begin with flow_end. */
    void       ( *flow_begin )( gui_rect_t rect );
    gui_rect_t ( *flow_cell  )( f32 w, f32 h );
    void       ( *flow_end   )( void );

    /* split_begin / split_next / split_end -- two panels side by side sharing a Y level: a fill
       left panel and a right panel `right_w` px wide, each an independent flow region (declare a
       mode inside each), heights cached per id.  A layout composition (flow/gui_split.c).  The
       worked example, with button_width / button_fill sizing the right panel, is under GUI_CHROME
       -- these three verbs are documented once, there. */
    void ( *split_begin   )( const char* id_str, f32 right_w );
    void ( *split_next    )( void );
    void ( *split_end     )( void );

    /*============================================================================================================
        GUI_STYLE -- style service  (style/)
        This is the theming layer: it turns a widget's state (idle, hovered, pressed) into an
        actual color or size, and it never draws anything itself. Every widget, stock or custom,
        asks this band "what color/size should I be right now?" instead of hardcoding one. This
        band is just the neutral MECHANISM -- base style access, push/pop stacks, per-slot reads,
        density scale, indicator-shape selectors; the actual named theme presets (Dark, Light, ...)
        are chrome's style kit and live with it (GUI_CHROME).
    =============================================================================================================*/

    gui_style_t*       ( *style_get   )( void );   /* mutable base -- marks the theme anonymous      */
    const gui_style_t* ( *style_peek  )( void );   /* read-only base -- does NOT mark it anonymous   */
    void               ( *style_apply )( void );   /* rescale the active metrics from the base       */

    /* style_bake -- expands a theme's small, hand-authored description (a handful of seed
       colors and a ramp) into the full grid of colors every widget actually reads. Derives
       s->col[][] from s->palette: the step between what a theme AUTHORS
       (seven seeds and a six-number ramp, gui_palette_t) and what a render READS (the 10x4
       colour grid).  Pure and in-place; touches no metric.

       Explicit rather than implicit, because a kit's usual shape is bake THEN overwrite: an
       automatic bake would have to run before those writes (no effect) or after them (silently
       eaten).  Naming the step makes the order yours.

           gui_style_t* e = gui()->style_edit();
           e->palette.seed[ GUI_SEED_SURFACE ] = charcoal;
           e->palette.seed[ GUI_SEED_ACCENT  ] = gold;
           e->palette.ramp[ GUI_RAMP_HOVER   ] = 0.5f;
           gui()->style_bake( e );                                 // all 40 cells derive
           e->col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE ] = ember;      // the one bespoke cell */
    void               ( *style_bake  )( gui_style_t* s );

    /* style_source_set -- the way a game or tool takes over the WHOLE application's look: register
       a function that gets re-run every time the style needs deriving, instead of setting colors
       once and having them get silently overwritten later. Registers the OWNER of the DEFAULT
       style set (set 0), the one chrome and any unbracketed UI resolve through: the promotion
       seam a kit uses to restyle the whole application.  The source is invoked immediately, then again at every style landing (font
       activation, theme_set / theme_reset / style_apply) AFTER the layout metrics rescale -- so
       the kit re-derives against fresh numbers instead of being clobbered by the default compile.
       Inside the source, write through style_edit() and read metrics via style_peek / the sz_
       family.  fn NULL restores the default owner: chrome's theme compiler. */
    void ( *style_source_set )( gui_style_source_fn fn, void* user );

    /* Style SETS -- lets two completely different looks exist on screen at once, e.g. an editor
       keeps its own theme while a game panel inside it paints with its own -- instead of one
       theme installation overwriting the other. Two looks installed side by side instead of one
       overwriting the other.

       A set is one installed copy of the WHOLE style -- colors, metrics, skin, density ramp --
       with its own owner.  Set 0 is chrome's;
       style_set_create takes another and style_set_push / _pop bracket the UI that resolves
       through it.  So an editor's chrome keeps its theme while a game's kit paints its own, and
       neither has to win.

       The source is invoked exactly like style_source_set's, at create and at every landing; a
       set that installs only the rows it cares about inherits the theme for the rest.  A push
       is a mirror plus a replay of live overrides, so a push_style_color bracketing the switch
       still applies inside it, and reads cost the same in any set.

           g_kit_set = gui()->style_set_create( kit_style_source, NULL );   // once
           gui()->style_set_push( g_kit_set );
           ... the kit's windows and widgets ...
           gui()->style_set_pop();

       Sets are capped at GUI_STYLE_SET_MAX (gui.h).  An unbalanced push cannot outlive the
       frame: style resets to set 0 at the frame boundary, like the style stacks. */
    gui_style_set_t ( *style_set_create  )( gui_style_source_fn fn, void* user );
    void            ( *style_set_push    )( gui_style_set_t set );
    void            ( *style_set_pop     )( void );
    gui_style_set_t ( *style_set_current )( void );

    /* Style stacks -- temporarily override one color or size for a section of UI, then restore
       the previous value automatically (like a save/restore pair). The push-model theme
       override.  A color names a (role, phase) cell of the
       color grid; a var names a gui_style_var_t scalar.  push overrides until the matching pop
       (pop takes a count, like ImGui); next_style_* overrides for just the next widget, no pop.
       Colors are abgr (GUI_COLOR); vars are f32 px.  Like the item flags, this is callsite-free:
       every widget already reads the grid + metrics through the resolver, so an override reaches
       them without any widget change.

       GUI_PHASE_ALL as the phase selects the whole row -- recolour all four text cells --
       and still counts as ONE push, so it takes one pop.

           gui()->push_style_color( GUI_ROLE_BG, GUI_PHASE_IDLE, GUI_COLOR( 0xFF, 0, 0, 0xFF ) );
           gui()->push_style_var( GUI_VAR_PAD, 20.0f );
           gui()->button( "Big Red" );
           gui()->pop_style_var( 1 );
           gui()->pop_style_color( 1 );

       push_style_seed is the third stack and the BULK verb: it replaces a source colour
       (gui_style_seed_t) and re-derives the grid, so every role fed by that seed moves together
       and each keeps its four-step ramp.  Reach for it whenever the ask is "recolour this",
       and reach for GUI_PHASE_ALL only on a row that does not react -- TEXT, BORDER -- since
       writing one value into four cells is exactly what kills a hover.

           gui()->push_style_seed( GUI_SEED_ACCENT, gold );   // fills, washes, rings, nav
           ... a gold panel ...
           gui()->pop_style_seed( 1 );

       push_style_ext is the fourth stack, over the EXTENDED palette (gui_style_ext_t) rather
       than the role/phase grid: a flat colour, no fan, no re-derivation -- a plain save/restore
       of the one slot, exactly like push_style_var.

           gui()->push_style_ext( GUI_EXT_WARN, GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF ) );
           ... a brighter warning banner ...
           gui()->pop_style_ext( 1 ); */

    void ( *push_style_color )( gui_style_role_t role, gui_style_phase_t phase, u32 abgr );
    void ( *pop_style_color  )( u32 count );
    void ( *next_style_color )( gui_style_role_t role, gui_style_phase_t phase, u32 abgr );
    void ( *push_style_var   )( gui_style_var_t var, f32 value );
    void ( *pop_style_var    )( u32 count );
    void ( *next_style_var   )( gui_style_var_t var, f32 value );
    void ( *push_style_seed  )( gui_style_seed_t seed, u32 abgr );
    void ( *pop_style_seed   )( u32 count );
    void ( *push_style_ext   )( gui_style_ext_t ext, u32 abgr );
    void ( *pop_style_ext    )( u32 count );

    /* The RESOLVED reads -- ask "what color should I actually draw right now," accounting for
       any active theme plus any push_style_color override in scope. The other half of the
       stacks above, and what every render actually calls.  style_color returns a (role, phase)
       cell of the installed style (kit-owned when a
       style source is registered) with any live push_style_color / next_style_color override
       already applied; item_phase distils an interact state into the phase to ask for.  THE
       color door for a widget of your own -- the same seam the stock renders and chrome's
       internal COL_* macros read, so a push_style_color around your widget behaves exactly as
       it does around a stock one.  nav counts as HOT, so a keyboard-navigated widget lights
       like a hovered one; GUI_PHASE_INERT is the inert variant a render picks deliberately.

           gui_comp_button_t b = gui()->comp_button( "save", r );
           u32 face = gui()->style_color( GUI_ROLE_BG, gui()->item_phase( b.state ) );

       style_color_selected washes the same read toward the theme's accent, for a widget whose
       CALLER knows the item is chosen.  There is no item_look beside item_phase and there cannot
       be: a phase is distilled from interact state, while nothing the interact server tracks
       knows what your data has selected.  Pass selected in, and a selected row keeps its hover
       and press feedback instead of freezing on one cell:

           u32 face = chosen ? gui()->style_color_selected( GUI_ROLE_BG, gui()->item_phase( b.state ) )
                             : gui()->style_color( GUI_ROLE_BG, gui()->item_phase( b.state ) );

       style_edit -- the raw INSTALLED style of the current set, mutable: the kit tuning door for
       INSTALLING a look, and the same gui_style_t a theme is authored as.  Ad-hoc writes last
       until the next style landing re-installs them; a kit that OWNS the look registers
       style_source_set (or style_set_create) so its style is re-derived rather than clobbered at
       every landing.  Do not read ->col[][] through it at paint time -- that bypasses the style
       stacks; use style_color. */
    gui_style_phase_t ( *item_phase          )( gui_item_state_t st );
    u32               ( *style_color         )( gui_style_role_t role, gui_style_phase_t phase );
    u32               ( *style_color_selected )( gui_style_role_t role, gui_style_phase_t phase );
    gui_style_t*      ( *style_edit          )( void );

    /* The extended-palette read: style_color's sibling with no phase to pass, resolving a flat
       gui_style_ext_t slot the same way -- installed value, any push_style_ext override already
       applied.  GUI_EXT_INFO/OK/WARN/ERROR/DROP are always valid; a kit's own registered id
       (style_ext_add) is valid only in the set that registered it, this landing. */
    u32 ( *style_ext )( gui_style_ext_t ext );

    /* Claim a slot in the CURRENT set's extended palette beyond the reserved severity four,
       seeded with a default colour -- call from a style SOURCE so the registration lands in the
       set being installed, mirroring style_brush_add exactly (idempotent per landing, not
       cumulative). */
    gui_style_ext_t ( *style_ext_add )( u32 default_abgr );

    /*==========================  the FACE plane -- art where a colour was  ==========================*/

    /* The face plane -- lets a theme swap in actual art (a gradient, a nine-slice sprite frame)
       anywhere a flat color would otherwise go, without editing a single widget's code. A face
       is a BRUSH installed on a (role, phase) cell: the same coordinate the colour
       grid uses, in a parallel plane, replacing that cell's flat fill with a gradient or a
       nine-slice.  This is the payoff of the brush -- because a face is addressed by the
       coordinate every render already resolves, installing one restyles every widget that paints
       through the grid (stock, chrome, and a user's own) with none of them edited.

       Two steps.  Register the art in the set's pool, then name the handle from cells:

           void my_theme( void* user )                       // a style source
           {
               gui_style_t*     st = gui()->style_edit();
               gui_style_face_t f  = gui()->style_brush_add(
                   &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = my_button_art } );

               st->face[ GUI_ROLE_BG ][ GUI_PHASE_IDLE ] = f;
           }

       Register inside a style SOURCE, not once at startup: the pool is cleared whenever a set is
       installed (a theme, font or scale change), and a source is precisely the thing re-run at
       that moment.  A handle is stable within a frame, not across a landing.

       A cell with no face is 0 (GUI_FACE_NONE) and falls straight through to its colour, so a
       theme that authors no art is unchanged and pays one indexed load.  A face SUPPRESSES the
       border its cell would otherwise have been given -- authored art carries its own edge.  A
       selected item's face is not a second stored cell either: draw_face_item washes whichever
       brush the cell already names toward the accent, the same live wash style_color_selected
       spends on a flat colour.

       push / pop / next mirror the colour verbs exactly, including the GUI_PHASE_ALL fan, and pop
       their own stack so an interleaved colour / face / var sequence unwinds correctly.  draw_face
       is the painter: fill a rect for a cell, using its face if it has one and its colour if it
       does not -- the seam every converted widget paints through, and the one a user widget should
       paint through to be skinnable by whoever installs the theme. */

    gui_style_face_t ( *style_brush_add )( const gui_brush_t* brush );

    void ( *push_style_face )( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face );
    void ( *pop_style_face  )( u32 count );
    void ( *next_style_face )( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face );

    /* The resolved read (NULL = the cell names no face) and the painter over it. */
    const gui_brush_t* ( *style_face )( gui_style_role_t role, gui_style_phase_t phase );

    void ( *draw_face )( gui_rect_t r, gui_style_role_t role, gui_style_phase_t phase );

    /* THE MIX -- what makes a widget's color/art smoothly animate between states (idle to
       hovered, say) instead of popping instantly. The continuous coordinate over the same grid
       (gui_style_mix_t, gui.h), and what makes a widget MOVE between cells instead of snapping
       between them.

           gui_style_mix_t m = gui()->style_mix( id, st, selected );   // one probe, damped
           gui()->draw_face_mix( box, GUI_ROLE_BG, m );                // surface
           u32 ink = gui()->style_color_mix( GUI_ROLE_TEXT_PRIMARY, m );       // ...and its ink, together

       style_mix is the only call here that touches storage: one 16-byte damper slot per item IN
       MOTION, which evicts the moment the item settles.  Read it ONCE per item and spend it on
       every row that item paints, so its parts travel together instead of each running a damper
       of their own.  GUI_ID_NONE opts out with no probe at all, and the three GUI_VAR_ANIM_*
       rates put the feel of the whole widget set in the theme's hands -- set them to 0 and the
       library snaps, down the same code path.

       draw_face_item is the one-call form for a widget that paints a single surface: it reads
       the mix itself.  Faces cross-fade rather than blend, since art does not interpolate. */
    void ( *draw_face_item )( gui_rect_t r, gui_id_t id, gui_item_state_t st, bool selected );
    void ( *draw_face_mix  )( gui_rect_t r, gui_style_role_t role, gui_style_mix_t mix );

    gui_style_mix_t ( *style_mix       )( gui_id_t id, gui_item_state_t st, bool selected );
    u32             ( *style_color_mix )( gui_style_role_t role, gui_style_mix_t mix );

    /* The schema, described by the engine that owns it -- so a style editor WALKS the five axes
       (role, phase, seed, ramp, var) instead of keeping parallel tables in step with enums it
       does not own.  Display names for each, plus what kind of number a var holds
       (gui_style_class_t): its class says whether it is a size, a stroke, a radius, the lattice
       pitch, or an enum pick, which is exactly what an editor needs to group it and choose a
       slider or a combo.  An unnamed index reads "?" rather than running off the end. */
    const char*       ( *style_role_name  )( gui_style_role_t role );
    const char*       ( *style_phase_name )( gui_style_phase_t phase );
    const char*       ( *style_seed_name  )( gui_style_seed_t seed );
    const char*       ( *style_ramp_name  )( gui_style_ramp_t ramp );
    const char*       ( *style_var_name   )( gui_style_var_t var );
    gui_style_class_t ( *style_var_class  )( gui_style_var_t var );
    f32               ( *style_var_max    )( gui_style_var_t var );   // tuning-slider ceiling, not a clamp
    const char*       ( *style_class_name )( gui_style_class_t cls );
    const char*       ( *style_ext_name   )( gui_style_ext_t ext );   // "?" past the reserved four

    /* scale_push / scale_pop -- scope a named density step (gui_scale_t: DENSE / STD / ROOMY /
       BAR) over the widgets until the pop: the theme's row + pad + gap for that step land on
       the style-var stack, so every metric read and counting helper (sz_rows_h, sz_fit_row)
       inside speaks the step.  Push before opening the region/child it styles.  To size against
       a step without pushing it, query sz_scale_row( s ) from the sizing family. */
    void ( *scale_push )( gui_scale_t s );
    void ( *scale_pop  )( void );

    /* scale_push_font -- a density step WITH a type role riding along, closed by the same
       scale_pop.  The plain scale ramp is whitespace-only by design (row / pad / gap, the
       glyphs stay body-sized); this is the explicit opt-in that pairs a step with a glyph
       size -- e.g. scale_push_font( GUI_SCALE_DENSE, GUI_TYPE_SMALL ) for an outliner that
       is tighter AND smaller-set.  The role falls back to the body font wherever it is off
       or unresolvable, so the pairing never needs a guard.

       type_push / type_pop -- bracket any scope with a type role on its own (no density
       change): the authored SMALL / LARGE size (GUI_VAR_TYPE_SMALL / _LARGE, absolute px at
       em=12; 0 = role off) swaps measurement and glyphs inside while layout metrics and the
       style stay put -- cells remain body-sized.  NORMAL, an off role, or a failed bake are
       all saved no-ops: authoring against a role is always safe. */
    void ( *scale_push_font )( gui_scale_t s, gui_type_role_t role );
    void ( *type_push       )( gui_type_role_t role );
    void ( *type_pop        )( void );


    /*============================================================================================================
        GUI_STOCK -- components + the reference widget set  (component/ + stock/)
        Every widget here splits into two halves you can use separately: the LOGIC (does the
        user's click land on this? how far did they drag it?) and the LOOK (what does it draw?).
        Two rungs, one pair per widget.  A COMPONENT (comp_*) is a widget's LOGIC with no look:
        it consumes (id, rect), does the tedious part -- hit-testing, drag math, value snapping,
        focus / hover / active -- and reports state + the geometry a render needs.  It never
        paints.  A STOCK widget (stock_*) is one plain render over that component: the reference
        you READ AND FORK, not a privileged default.  Your own widget is the stock render's
        SIBLING -- same comp_* call, different draw_* -- which is the whole point of the stack.

        Rects come from anywhere: gui_rect.h math, split / carve / anchor, flow_cell, your own
        numbers.  (The flow-placed, labeled, theme-styled versions of these widgets live in
        GUI_CHROME; next_item_rect feeds a chrome widget an explicit rect when you want that
        set instead.)

            gui_comp_button_t b = gui()->comp_button( "save", r );          // logic
            u32 face = gui()->style_color( GUI_ROLE_BG, gui()->item_phase( b.state ) );  // look
            gui()->draw_round_rect( r, 8,8,8,8, 0.0f, face );          // your paint

        The two style reads that pairing needs (item_phase / style_color) sit with the rest of
        the style surface in GUI_STYLE above -- ONE vocabulary, whichever tier you build on.
    =============================================================================================================*/

    /*=====================================  the component / render pairs  ======================================*/

    /* slider -- hit / drag / snap / keyboard nav; returns the resolved fraction plus the value
       BAR and HANDLE rects.  Absolute-position mapping: the handle CENTER tracks the cursor, so
       value and knob never disagree.  The positional form covers the common case; comp_slider_ex
       takes the full desc (gui_comp_slider_desc_t: snap step, handle width, nav step).
       stock_slider draws a styled groove + handle, leaving any value text to the caller. */
    gui_comp_slider_t     ( *comp_slider      )( const char* id_str, gui_rect_t rect, f32* v, f32 lo, f32 hi );
    gui_comp_slider_t     ( *comp_slider_ex   )( const gui_comp_slider_desc_t* desc );
    bool                  ( *stock_slider     )( gui_rect_t r, const char* id_str, f32* v, f32 lo, f32 hi );

    /* button -- the press protocol; the simplest component and the shape the rest follow.  It
       takes a pure ID: the displayed label is the render's business (stock_button passes the
       label as both, so the "##hidden" / "###stable" grammar still applies). */
    gui_comp_button_t     ( *comp_button      )( const char* id_str, gui_rect_t rect );
    bool                  ( *stock_button     )( gui_rect_t r, const char* label );

    /* check -- toggle over an inscribed square box (returned as .box: the hit AND the paint
       target).  cycle -- a "< value >" stepper composing a comp_button per cap; it takes `count`
       for the wrap but NOT the strings, which the render draws at .label.  selectable --
       comp_button plus an optional *selected toggle (NULL = a click-only row). */
    gui_comp_check_t      ( *comp_check       )( const char* id_str, gui_rect_t rect, bool* v );
    bool                  ( *stock_check      )( gui_rect_t r, const char* id_str, bool* v );
    gui_comp_cycle_t      ( *comp_cycle       )( const char* id_str, gui_rect_t rect, i32* idx, i32 count );
    bool                  ( *stock_cycle      )( gui_rect_t r, const char* id_str, i32* idx, const char* const* items, i32 count );
    gui_comp_selectable_t ( *comp_selectable  )( const char* id_str, gui_rect_t rect, bool* selected );
    bool                  ( *stock_selectable )( gui_rect_t r, const char* label, bool* selected );

    /* input -- the richest component: runs the SAME edit engine chrome's input_text drives (keys,
       mouse selection drag, clipboard, undo, horizontal scroll, caret blink) and returns PAINTABLE
       geometry -- content rect, run origin, selection + caret bars -- so a render draws a text
       field with only public verbs (push_clip / draw_text / draw_rect), never touching the edit
       state or measuring a glyph.  A press claims keyboard focus; buf is caller-owned,
       NUL-terminated, edited in place.  Enter / submit policy stays with the caller
       (set_edit_key_hook, or chrome's input_text for the labeled form-row treatment). */
    gui_comp_input_t      ( *comp_input       )( const char* id_str, gui_rect_t rect, f32 pad, char* buf, u32 bufsz );
    bool                  ( *stock_input      )( gui_rect_t r, const char* id_str, char* buf, u32 bufsz );

    /* The inert three -- no interaction, so no logic to extract; render-only by design.
       stock_panel -- framed backdrop (the DIM surface), for grouping.  stock_label -- a text run
       seated per align.  stock_meter -- framed fill bar whose fill color is a CALL PARAMETER
       (per-widget color is kit business, not a style slot). */
    void ( *stock_panel )( gui_rect_t r );
    void ( *stock_label )( gui_rect_t r, gui_align_t align, const char* str );
    void ( *stock_meter )( gui_rect_t r, f32 frac, u32 fill_abgr );

    /*============================================================================================================
        GUI_CHROME -- convenience / editor UI  (window/ + dock/ + popup/ + widgets/ + table/)
        This is the product layer: the actual editor-style building blocks -- movable windows,
        docking, popups, menus, toolbars, the ready-made widget set, tables, and named themes --
        that an application assembles its UI out of. Everything here is BUILT from the strata
        below (frame / draw / core / surface / rect / flow / style); nothing in this band is
        privileged, it is just one particular, convenient way of combining those lower pieces.
        The imgui-style design layer over the strata beneath it: persistent windows, docking,
        popups / menus / toolbars, the stock flow-adapted widget set, tables, and the theme
        system (S2: a compiler that resolves down to the strata beneath it).
    =============================================================================================================*/

    /*=====================================  window/ -- persistent windows  =====================================*/

    /* Panels -- open a window panel; must be matched with window_end().
       flags is a bitmask of gui_win_flags_t (0 / GUI_WIN_NONE for the defaults) that
       switches off built-in behavior per window -- title bar, collapse, or edge resize.

       window_begin() returns false when the window is collapsed (title bar only) or closed.
       Guard the body widgets with it -- skipped widgets cost nothing -- and always call
       window_end() regardless of the return value (THE BEGIN / END RULE, top of this header):

           if ( gui()->window_begin( "Tools", GUI_WIN_NONE ) )
           {
               gui()->text( "..." );          // skipped while collapsed
           }
           gui()->window_end();               // always called */

    /* window_set_next_pos / _size -- queue geometry for the NEXT window_begin, applied per the
       condition (gui_cond_t) and then cleared.  Decouples the value from when it is applied:
       ONCE seeds an initial position/size (apply once on first appearance, then user-owned),
       ALWAYS forces it every frame (layout managers, snapping, animation -- pair with NOMOVE /
       NORESIZE), APPEARING re-applies it each time the window is shown after being absent.
       Call immediately before window_begin. */
    void ( *window_set_next_pos  )( f32 x, f32 y, gui_cond_t cond );
    void ( *window_set_next_size )( f32 w, f32 h, gui_cond_t cond );

    /* window_set_next_viewport -- assign the NEXT window_begin to a specific viewport.  Sticky: it
       lands on the window record and persists across frames until reassigned.  Omit to inherit the
       ambient viewport -- the one most recently emitted into this frame -- so windows created from
       within a viewport's panels naturally land on the same surface without explicit assignment.
       If the assigned viewport is later closed, the window automatically reverts to the primary. */
    void ( *window_set_next_viewport )( i32 vp );

    /* window_set_next_size_constraints -- queue a one-shot [min,max] size box for the NEXT
       child_begin, then cleared.  The Dear ImGui SetNextWindowSizeConstraints analogue, in its
       most useful form: it bounds the child's resolved width / height, so an auto-sized (h <= 0)
       box grows with its content up to max_h and then scrolls, never collapses below min_h, and a
       CHILD_RESIZE_* drag cannot leave the range.  A bound <= 0 is "unconstrained" on that side
       (e.g. 0, 0, 0, max_h to cap height only).  Call immediately before child_begin. */
    void ( *window_set_next_size_constraints )( f32 min_w, f32 min_h, f32 max_w, f32 max_h );

    bool ( *window_begin )( const char* title, gui_win_flags_t flags );
    void ( *window_end   )( void );

    /* window_set_open / window_is_open -- drive a CLOSEABLE window's visibility by title (the same
       key window_begin hashes).  The window's close (X) button hides it; the host re-opens it by
       calling window_set_open( title, true ) from a button.  window_is_open reports the current
       state (a window with no record yet -- never begun -- reads as open). */
    void ( *window_set_open )( const char* title, bool open );
    bool ( *window_is_open  )( const char* title );

    /* Window state-transition animation (maximize / minimize / restore).  On by default: the window
       tweens between rects through the gui() animation service.  Off snaps instantly.  A global
       preference, not per-context. */
    void ( *window_anim_enable     )( bool on );
    bool ( *window_anim_is_enabled )( void );

    /*==========================  dock/ -- dock tree, tab groups, layout persistence  ===========================*/

    /* Docking -- lets the user (or the host, in code) arrange windows into a tiled, tabbed
       layout that fills a viewport, the way a modern IDE's panels dock together, instead of
       every window floating loose on top of each other. Tile + tab windows into a dock tree
       that fills a viewport (the DockSpaceOverViewport analogue). The programmatic path: build
       a layout in code, then windows whose titles were
       dock_window'd render into their node (no per-window title bar -- the node draws a shared tab
       strip) instead of free-floating.  Mouse drag-to-dock and layout persistence (dock_save/load
       below) build on the same tree.  Free-floating windows still overlap on top of the dockspace.

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
       window_is_docked()        -- true while the window is tabbed into some node (dormant included).
       dock_window_maximize()    -- maximize the window's node over its WHOLE dockspace (fullscreen
                                    the docked pane -- the other nodes are obscured and stop emitting)
                                    or restore the tiled layout; animated like the floater maximize
                                    (window_anim_enable gates it).  GUI_WIN_DOCK_MAXIMIZE gates only
                                    the tab strip's button; this verb works regardless, so a host can
                                    bind fullscreen-toggle to a hotkey without offering the chrome.
       window_is_dock_maximized() -- true while the window's node holds the dockspace maximize.

       A dockspace is EMIT-GATED like every immediate-mode element: on frames the host does not call
       dockspace_over_viewport, the viewport's tree is DORMANT -- retained but inert.  Windows tabbed
       in a dormant tree keep their membership but render nothing (window_begin returns false,
       inactive-tab semantics), and title drags offer no dock chips; floating tab groups are
       independent of the tree and unaffected.  Re-emitting the dockspace revives the layout exactly
       as it was, so a host can swap whole UI modes in and out just by (not) running the dock code
       path.  dock_clear (below) is the only thing that destroys the tree.

           gui_dock_id_t root  = gui()->dockspace_over_viewport( 0, GUI_DOCKSPACE_NONE );
           gui_dock_id_t left  = gui()->dock_split( root, GUI_DIR_LEFT, 0.25f, &root );
           gui()->dock_window( "Scene Tree", left );
           gui()->dock_window( "Viewport",   root );   // center; tab more windows here with root */

    gui_dock_id_t ( *dockspace_over_viewport )( i32 vp, gui_dockspace_flags_t flags );
    gui_dock_id_t ( *dock_split              )( gui_dock_id_t node, gui_dir_t dir, f32 ratio,
                                                gui_dock_id_t* out_remain );
    /* dock_split_root() -- split the WHOLE viewport tree, carving a new leaf along a full edge (`dir`).
       Unlike dock_split (a single leaf), this wraps the root in a new split so the pane spans the entire
       side -- the way to place a full-height column beside an existing top/bottom stack.  Returns the
       new leaf id (dock windows into it), or GUI_DOCK_NONE.  Also the commit path of an edge drop. */
    gui_dock_id_t ( *dock_split_root          )( i32 vp, gui_dir_t dir, f32 ratio );
    void          ( *dock_window              )( const char* title, gui_dock_id_t node );
    void          ( *dock_undock              )( const char* title );
    bool          ( *window_is_docked         )( const char* title );
    void          ( *dock_window_maximize     )( const char* title, bool on );
    bool          ( *window_is_dock_maximized )( const char* title );

    /* Floating tab groups -- lets two ordinary floating windows merge into one tabbed group
       without needing a dock tree at all, the way dragging one browser window's tab onto
       another merges them. Tabbing WITHOUT split panes.  window_tab() merges window `title` onto
       window `onto_title`'s frame: a free target grows a floating tab group around itself (shared
       frame, tab strip in place of a title bar; drag the strip's empty band to move it, its edges
       to resize); a target already tabbed somewhere -- a group or a dockspace leaf -- just gains
       the tab.  The same merge exists as a gesture: title-drag one free window onto another's
       title bar and drop on the center chip.  dock_undock() pulls a window back out; a group
       dissolves by itself once a single tab remains.  A window flagged GUI_WIN_NO_TAB_TARGET
       never hosts tabs (no drop chip; refused as onto_title) -- flag control panels whose body
       the host gates on window_begin's return.  To keep a whole DOCKSPACE tabs-only instead,
       pass GUI_DOCKSPACE_NO_SPLIT to dockspace_over_viewport: only the center (tab) drop chip
       is offered and the split verbs above refuse. */
    void ( *window_tab )( const char* title, const char* onto_title );

    /* Layout persistence.  dock_save() serializes viewport vp's dock tree into buf as a small ASCII
       blob and returns the byte count a full write needs (like snprintf -- pass a 0 bufsz to size
       first).  dock_load() rebuilds the tree from such a blob; returns false on a bad header.  The
       host owns the file: write the blob on change, read + load it at startup.  CALL dock_load at a
       safe point -- between frames or at the top of the build before any docked window's window_begin
       -- never from inside a docked window (it frees + rebuilds the tree). */
    u32  ( *dock_save )( i32 vp, char* buf, u32 bufsz );
    bool ( *dock_load )( i32 vp, const char* text );

    /* dock_clear() -- DESTROY viewport vp's dock tree: free every node and clear the root.  Windows
       lose their tab membership permanently and free-float from their next begin (at the rect their
       node last gave them).  Not needed to merely stop docking for a while -- a dockspace that is
       not emitted goes DORMANT (see above) and revives intact.  Clear is for discarding a layout
       wholesale, e.g. before hand-building a fresh one.  Same safe-point rule as dock_load: top of
       the build, never from inside a docked window.  Floating tab groups stay standing. */
    void ( *dock_clear )( i32 vp );


    /* Host-reserved top band (pixels) above viewport vp's dock area -- the height of a main menu
       bar / toolbar strip the host draws itself; the dock tree lays out below it.  Sticky until
       re-published; pass 0 to reclaim.  Publish before dockspace_over_viewport in the build. */

    void ( *dockspace_inset )( i32 vp, f32 top );

    /*==========================  popup/ -- popups, tooltips, menus, combo + listbox  ===========================*/

    /* Popups -- a small window that appears on demand and goes away again: a right-click menu,
       a confirmation dialog, a dropdown's list. Transient overlay windows on top of everything.
       A regular popup auto-closes when the user clicks outside it; a modal blocks input behind
       it and dims the background, closing
       only via popup_close_current.  The string id namespaces both the open request and the body,
       so popup_open("x") and popup_begin("x") must use the same id.  Popups stack (a popup opened
       while inside another nests under it); a click keeps the deepest popup under the cursor and
       closes the rest.  Popup / tooltip bodies lay out like a window body: declare a layout header
       (stack / columns / ...) before emitting widgets.

           if ( gui()->button( "Open" ) )    gui()->popup_open( "menu" );
           if ( gui()->popup_begin( "menu", GUI_WIN_NONE ) ) {
               gui()->stack();
               if ( gui()->selectable( "Cut",  NULL ) ) { ... }
               if ( gui()->selectable( "Copy", NULL ) ) { ... }
           }
           gui()->popup_end();                       // ALWAYS (see THE BEGIN / END RULE)

       popup_begin / popup_modal_begin return true only when the popup is open AND visible: the
       return gates the body, popup_end is unconditional.  This one matters more than most -- an
       open popup detaches the parent layout context at begin, so a skipped end would strand it.
       Auto-sized popups (the default) measure their content on the appearing frame off-screen and
       snap into place the next frame, so there is no first-frame size pop. */

    void ( *popup_open          )( const char* id_str );
    bool ( *popup_begin         )( const char* id_str, gui_win_flags_t flags );
    bool ( *popup_modal_begin   )( const char* id_str, const char* title, gui_win_flags_t flags );
    void ( *popup_end           )( void );
    void ( *popup_close_current )( void );
    bool ( *popup_is_open       )( const char* id_str );

    /*
        Context menus -- open a popup on a right-click.  _item binds to the previous widget (the one
        emitted just before the call); _window binds to empty space in the current window.  Use them
        in place of the popup_open + popup_begin pair:

            gui()->selectable( "Row", NULL );
            if ( gui()->popup_context_item_begin( "row_ctx" ) ) { ... }
            gui()->popup_end();
    */
    bool ( *popup_context_item_begin   )( const char* id_str );
    bool ( *popup_context_window_begin )( const char* id_str );

    /*
        Tooltips -- the small text bubble that appears near the cursor when you hover over
        something and explains what it does. A non-interactive overlay shown at the cursor while
        the previous widget is hovered. set_item_tooltip is the one-liner; tooltip_begin /
        tooltip_end wrap a multi-widget
        body (the return gates the body; tooltip_end is unconditional).

           gui()->button( "Hover me" );
           gui()->set_item_tooltip( "Does the thing" );

        tooltip_begin does NOT test hover -- it OPENS the window, unconditionally, and the caller
        guards.  set_item_tooltip does that guard for you; the multi-widget form cannot, because
        only the caller knows which item the body belongs to:

           gui()->slider_float( "rate", &v, 0.0f, 40.0f );
           if ( gui()->is_item_hovered() )
           {
               if ( gui()->tooltip_begin() )
               {
                   gui()->stack();      // the body is a fresh region: declare a mode first
                   gui()->text( "..." );
               }
               gui()->tooltip_end();    // UNCONDITIONAL -- it reattaches what begin detached
           }

        Forgetting the guard is quiet rather than loud: every unguarded tooltip in the frame opens
        the ONE shared tooltip window at the cursor, so they stack and paint their borders through
        each other's text instead of simply appearing at the wrong time.

        help_marker draws a dim "(?)" hint that pops `text` on hover -- the Dear ImGui footnote,
        typically emitted on the same line after a control:

           gui()->checkbox( "No mouse", &flag );
           gui()->same_line( 0.0f );
           gui()->help_marker( "Disable mouse inputs and interactions." );
    */
    void ( *set_item_tooltip )( const char* str );
    bool ( *tooltip_begin    )( void );
    void ( *tooltip_end      )( void );
    void ( *help_marker      )( const char* str );

    /* Menus -- the File / Edit / View-style menu bar and its dropdowns, built as a coordination
       layer over the popup stack above. A menu bar holds menu_begin entries;
       each opens a submenu popup that holds menu_items and further menu_begin entries (nesting on
       the popup stack).  Disabled state reuses the item-flag stack: push_item_flag(GUI_ITEM_DISABLED).

       main_menu_bar_begin pins a bar across the top of the display; menu_bar_begin fills the strip a
       window reserved with GUI_WIN_MENUBAR (and returns false on a window without the flag).  Both
       return true only when visible -- the return gates the entries; the matching end is
       unconditional, exactly like window_begin / popup_begin.

           if ( gui()->main_menu_bar_begin() ) {
               if ( gui()->menu_begin( "File" ) ) {
                   if ( gui()->menu_item( "Open", "Ctrl+O", NULL ) ) { ... }
                   gui()->menu_item( "Show grid", NULL, &show_grid );   // checkable
                   if ( gui()->menu_begin( "Recent" ) ) {              // submenu
                       gui()->menu_item( "a.txt", NULL, NULL );
                   }
                   gui()->menu_end();                                  // ends are unconditional
               }
               gui()->menu_end();
           }
           gui()->main_menu_bar_end();

       menu_begin renders horizontally in a bar (its popup drops below) and as a full-width row with
       a submenu arrow inside a menu (its popup opens to the side); the orientation follows the active
       layout mode, so no flag is needed.  menu_item returns true on the clicked frame and dismisses
       the whole menu chain; shortcut is display-only (may be NULL); selected may be NULL (a plain
       command) or a bool* (a checkable item, toggled on click). */

    bool ( *main_menu_bar_begin )( void );
    void ( *main_menu_bar_end   )( void );

    /* main_menu_bar_h() -- the band height main_menu_bar_begin occupies (theme-derived).  Use it
       to stack host strips (toolbars, dockspace_inset) below the bar instead of re-deriving the
       height from font metrics -- it stays truthful when the theme or scale ramp retunes. */
    f32  ( *main_menu_bar_h )( void );
    bool ( *menu_bar_begin  )( void );
    void ( *menu_bar_end    )( void );
    bool ( *menu_begin      )( const char* label );
    void ( *menu_end        )( void );
    bool ( *menu_item       )( const char* label, const char* shortcut, bool* selected );

    /* Toolbar -- a row of icon buttons, like an editor's main toolbar. An icon strip built on
       bar() (flow/).  toolbar_begin id-scopes the strip so
       two toolbars' buttons never collide, then opens a bar() run; toolbar_end pops it.  Emit
       inside any window / child -- it owns no window of its own, matching bar() itself.  It does
       NOT push a scale -- wrap it in the caller's own scale_push/scale_pop (GUI_SCALE_BAR is the
       density step authored for icon toolbars, but any GUI_SCALE_* works) so a single app can mix
       toolbar sizes, e.g. a large main-panel strip next to a regular-scale one.

       toolbar_button / toolbar_toggle are square icon cells (press / latched-on); their id_str is
       the id only ("##save") -- pass a display label there and it is still just the id, nothing
       is drawn from it.  toolbar_dropdown_begin/end is the split-button form: the icon plus an
       adjacent down-arrow column, opening an arbitrary-widget popup below the button -- the same
       anchor / dismiss mechanics as combo_begin/combo_end, so put ANY widgets in the body,
       including menu_item rows for the icon + label + shortcut three-column layout menus already
       give you. tooltip may be NULL.

           gui()->scale_push( GUI_SCALE_BAR );
           gui()->toolbar_begin( "main" );
               if ( gui()->toolbar_button( "##save", icon_save, "Save (Ctrl+S)" ) ) save();
               gui()->toolbar_toggle( "##wire", icon_wire, &wireframe, "Wireframe" );
               gui()->toolbar_separator();
               if ( gui()->toolbar_dropdown_begin( "##view", icon_eye, "View Mode" ) ) {
                   gui()->menu_item( "Lit", NULL, NULL );
                   gui()->menu_item( "Wireframe", NULL, NULL );
               }
               gui()->toolbar_dropdown_end();
           gui()->toolbar_end();
           gui()->scale_pop(); */

    bool ( *toolbar_begin           )( const char* id_str );
    void ( *toolbar_end             )( void );
    bool ( *toolbar_button          )( const char* id_str, gui_icon_id_t icon, const char* tooltip );
    bool ( *toolbar_toggle          )( const char* id_str, gui_icon_id_t icon, bool* v, const char* tooltip );
    bool ( *toolbar_dropdown_begin  )( const char* id_str, gui_icon_id_t icon, const char* tooltip );
    void ( *toolbar_dropdown_end    )( void );
    void ( *toolbar_separator       )( void );

    /*===================================  widgets/ -- the stock widget set  ====================================*/

    /* Widgets -- return true on the frame they are activated or changed.
       All widgets must be called between a matched window_begin / window_end pair, and only
       when window_begin returned true -- a collapsed window draws no clip, so widgets emitted
       into it render straight onto the screen.  The bool guard is the caller's job. */

    void ( *text        )( const char* str );
    void ( *textf       )( const char* fmt, ... );
    void ( *bullet_text )( const char* str );

    /* text_colored / text_disabled -- a text run in an explicit colour / the dim secondary colour.
       text_wrapped -- a run word-wrapped to the region content width (paragraphs, help blurbs).
       bullet -- a standalone bullet glyph.  separator -- a thin horizontal rule centered in its
       cell (separator_text, the labeled form, is below with the other headers); both honor
       GUI_VAR_SEPARATOR_SHAPE.  The paintless cell spacers skip / new_line are GUI_FLOW's. */
    void ( *text_colored  )( u32 abgr, const char* str );
    void ( *text_disabled )( const char* str );
    void ( *text_wrapped  )( const char* str );
    void ( *bullet        )( void );
    void ( *separator     )( void );

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

    /* plot_lines / plot_histogram -- a read-only sparkline over a caller-owned f32 array (the
       ImGui PlotLines / PlotHistogram analogues): lines connect the samples, histogram raises
       one bar per sample from the zero line (both stride-sample when there are more samples
       than pixels).  `offset` rotates the read order -- a ring buffer passes its write index
       and the plot scrolls without any memmove.  scale_min >= scale_max auto-fits the data
       range; h <= 0 takes the default three-row height.  Hovering highlights the sample and
       shows "index: value" in a tooltip.  overlay draws centered at the top (NULL / "" =
       none); the label trails past the right edge like listbox / input_text_multiline. */
    void ( *plot_lines     )( const char* label, const f32* values, i32 count, i32 offset,
                              const char* overlay, f32 scale_min, f32 scale_max, f32 h );
    void ( *plot_histogram )( const char* label, const f32* values, i32 count, i32 offset,
                              const char* overlay, f32 scale_min, f32 scale_max, f32 h );

    /* arrow_button -- a square, framed, non-text button drawing a triangle pointing `dir`.  The id
       comes from the label (use a "##id" string, nothing is displayed).  Combine with
       push_item_flag( GUI_ITEM_BUTTON_REPEAT, true ) for press-and-hold stepping (spin buttons). */
    bool ( *arrow_button )( const char* id_str, gui_dir_t dir );

    /* checkbox -- indicator box + its own label; the label follows the ambient field (aligned
       column under a form / field_split, trailing otherwise, or dropped when hidden / skipped).
       The body is the hit -- clicking the label toggles (intrinsic to a checkbox). */
    bool ( *checkbox    )( const char* label, bool* v );

    /* radio_button -- one option of a mutually-exclusive set: shows on while *v == value, a click
       sets *v = value.  Emit several against the same v (same_line between them for a row) to form
       a group; returns true only on the frame a click changes the selection. */
    bool ( *radio_button )( const char* label, i32* v, i32 value );
    /* slider_float -- draggable [lo,hi] slider; returns true while dragging.  The current value is
       drawn centered on the track by default ("%.3f"); set GUI_ITEM_NO_VALUE_TEXT (push or
       next_item_flag) to hide it for a bare slider. */
    bool ( *slider_float )( const char* label, f32* v, f32 lo, f32 hi );

    /* slider_float_step -- slider_float that quantizes the value to `step` (e.g. 0.25 snaps to the
       quarter marks); step <= 0 is continuous, identical to slider_float. */
    bool ( *slider_float_step )( const char* label, f32* v, f32 lo, f32 hi, f32 step );

    /* slider_int -- integer slider over [lo,hi]; every track position lands on a whole value, drawn
       centered.  format is the printf form of the shown value ("%d" when NULL/empty, DragInt
       parity) -- a format with no conversion specifier prints verbatim, so an option-list slider
       can pass items[*v] and show its rung's name instead of its index.  Same GUI_ITEM_NO_VALUE_TEXT
       suppression as slider_float. */
    bool ( *slider_int )( const char* label, i32* v, i32 lo, i32 hi, const char* format );

    /* next_slider_animate -- one-shot latch for the NEXT slider call: a plain click (press+release
       with no drag) eases the value to the clicked position over `duration` seconds instead of
       jumping, using `ease`.  Off by default -- an un-armed slider ignores this and jumps exactly
       as before, so the animation costs nothing unless a caller opts in.  Actually dragging the
       slider is untouched either way: live cursor tracking always wins over the tween.

           gui()->next_slider_animate( GUI_EASE_OUT_CUBIC, 0.15f );
           gui()->slider_float( "volume", &vol, 0.0f, 1.0f );

       To make a slider animated by default, wrap it: call this before your own slider function
       each time you use it. */
    void ( *next_slider_animate )( gui_ease_t ease, f32 duration );

    /* drag_int -- a framed integer field driven by a left/right drag (the DragInt analogue): no
       track, so no max travel -- v_speed units of value per pixel.  v_min < v_max bounds it; both
       equal leaves it unbounded.  format is the printf form of the shown value ("%d" when NULL,
       e.g. "HP: %d").  Returns true only on frames the drag changes the value. */
    bool ( *drag_int )( const char* label, i32* v, f32 v_speed, i32 v_min, i32 v_max, const char* format );

    /* drag_float -- the floating-point DragFloat: a framed value changed by a left/right drag,
       v_speed units per pixel, no track travel.  v_min < v_max bounds it; both equal is unbounded.
       fmt is the printf form ("%.3f" when NULL).  drag_float2/3/4 lay N equal sub-boxes (vector edit). */
    bool ( *drag_float  )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float2 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float3 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );
    bool ( *drag_float4 )( const char* label, f32* v, f32 v_speed, f32 v_min, f32 v_max, const char* fmt );

    bool ( *color_edit3 )( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags );
    bool ( *color_edit4 )( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags );

    /* color_picker3/4 -- the full picker block inline: a saturation/value square with a hue bar
       (and an alpha bar for picker4 without NO_ALPHA), then a per-component drag row and a hex
       entry field (NO_INPUTS trims it to the square + bars).  label is an id only -- nothing is
       rendered from it.  color_edit's swatch popup hosts this same body, so embedding one inline
       is exactly the popup experience without the popup. */
    bool ( *color_picker3 )( const char* label, f32 col[ 3 ], gui_color_edit_flags_t flags );
    bool ( *color_picker4 )( const char* label, f32 col[ 4 ], gui_color_edit_flags_t flags );

    /* next_input_filter -- one-shot character filter for the NEXT input widget (see
       gui_input_filter_t): typed and pasted characters outside the filter's vocabulary are
       dropped.  Immediate mode: call it before the field every frame to keep it filtered.

           gui()->next_input_filter( GUI_INPUT_FILTER_DIGITS );
           gui()->input_text( "port", buf, sizeof( buf ) );        // accepts 0-9 only

       The numeric entries (input_int/_float, Ctrl+Click on any drag / slider box) and the
       color picker's hex field already install their own. */
    void ( *next_input_filter )( gui_input_filter_t filter );

    bool ( *input_text    )( const char* label, char* buf, u32 bufsz );

    /* input_text_ex -- like input_text but with an on_change callback fired after any frame
       that modifies the buffer.  Pass NULL for on_change to suppress.  cb_user is forwarded
       verbatim to the callback. */
    bool ( *input_text_ex )( const char* label, char* buf, u32 bufsz,
                              gui_text_cb_fn on_change, void* cb_user );

    /* input_text_with_hint -- like input_text but shows `hint` in dim text inside the box
       when the buffer is empty and the field is not focused.  The hint is never written to buf. */
    bool ( *input_text_with_hint )( const char* label, const char* hint, char* buf, u32 bufsz );

    /* input_text_multiline -- multi-line text editor box of pixel height h (0 = eight lines).
       Enter inserts a newline (it never submits or drops focus; Escape reverts to the
       focus-gain content and leaves).  Arrow keys move the caret in 2D with a sticky
       preferred column, Home/End are line-local (Ctrl jumps to the buffer ends), PageUp/Dn
       page by the visible height.  Selection, clipboard, and undo/redo match input_text;
       paste keeps newlines.  No word wrap: long lines pan, chasing the caret.  The box is a
       child region (the listbox recipe), so vertical overflow gets the standard region
       scrollbar / wheel / clipping, and the label trails the box's right edge.  Returns true
       on any frame that modifies the buffer. */
    bool ( *input_text_multiline )( const char* label, char* buf, u32 bufsz, f32 h );

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

    /* msel_item -- selectable's presentation as the stock row of a multi-select scope
       (GUI_CORE msel_begin .. msel_end): paints from the caller's `selected`, feeds
       (index, state) to the protocol, and never auto-closes a popup (a multi-selection is
       built across several clicks).  The row id folds in `index`, so repeated labels are fine. */
    bool ( *msel_item   )( const char* label, i32 index, bool selected );

    /* Combo box -- the standard dropdown selector: a framed preview box (selected text + a down
       arrow) with a trailing label that drops a popup of rows below it on click. combo_begin
       opens the dropdown: it returns true
       only while the dropdown is open, so -- like window_begin's collapse -- the return gates the
       rows and combo_end is unconditional.  preview_value is the text shown in the closed box (the
       caller's current selection, usually items[current]).  A row clicked in the body dismisses the
       combo automatically, so emit selectables and set your selection from their return:

           if ( gui()->combo_begin( "mode", items[cur], GUI_COMBO_NONE ) ) {
               for ( i32 i = 0; i < n; ++i )
                   if ( gui()->selectable( items[i], NULL ) ) cur = i;
           }
           gui()->combo_end();

       flags is gui_combo_flags_t: the HEIGHT_* group caps the dropdown to a fixed row count
       (then it scrolls), 0 (GUI_COMBO_NONE) is the ~8-row default.  combo() is the one-liner over
       an array of strings (*current_item is the selected index; out of range shows an empty
       preview).  Both return true on the frame the selection changes. */
    bool ( *combo_begin )( const char* label, const char* preview_value, gui_combo_flags_t flags );
    void ( *combo_end   )( void );
    bool ( *combo       )( const char* label, i32* current_item, const char* const items[], i32 count );

    /* List box -- a framed, independently scrolling box of selectable rows with a trailing label.
       listbox_begin opens the box (w / h in pixels; w <= 0 fills the line after the label, h <= 0
       is ~7 rows tall) and always returns true -- always pair with listbox_end, and fill it with
       selectables exactly like a child_begin:

           if ( gui()->listbox_begin( "items", 0, 0 ) ) {
               for ( i32 i = 0; i < n; ++i ) {
                   bool sel = ( cur == i );
                   if ( gui()->selectable( names[i], &sel ) ) cur = i;
               }
               gui()->listbox_end();
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

           if ( gui()->tree_node( "Parent" ) )
           {
               gui()->text( "Child" );
               gui()->tree_pop();
           }

       indent / unindent -- shift the content column right (or back) by w pixels (w <= 0 = one row
       height) so a block of widgets lays out inset; the mechanism behind tree_node, usable alone.
       Balance every indent with an unindent of the same width.  Flow layouts only. */
    bool ( *tree_node )( const char* label );
    void ( *tree_pop  )( void );
    void ( *indent    )( f32 w );
    void ( *unindent  )( f32 w );

    /* box_begin / box_end -- draws a card-like background behind a run of widgets, sized to
       whatever those widgets turned out to need -- lighter weight than a full child region
       (no scrolling, no clip). A styled SURFACE behind a run of widgets, sized to whatever they
       turned out to be.  The decorator that sits between a widget's own face (a thing, so it has
       a rect) and child_begin (a scroll region, with a clip, a scroll link and a layout frame --
       far too much machine to put a card behind three labels).  A box owns no region: it insets
       the content column by one pad, paints `role` behind, and gives the column back.

           gui()->box_begin( "summary", GUI_ROLE_PANEL );
           gui()->text( "Frames" );
           gui()->slider_float( "budget", &ms, 0.0f, 33.0f );
           gui()->box_end();

       The surface is painted BEFORE its content, because painting order is emit order -- so its
       HEIGHT comes from last frame's measure (x, w and the top are exact: a box spans the
       content column at the pen).  That frame of lag is what GUI_VAR_ANIM_SIZE eases, so a box
       whose content grows is seen growing rather than seen wrong once; the layout always
       reserves the larger of painted and measured, so nothing below is ever drawn over.  A first
       appearance therefore paints no surface for exactly one frame.

       `role` picks the row of the style grid the surface comes from (PANEL for a card, BG for a
       control-coloured well), so a theme that authored a FACE for that cell skins every box in
       the build without touching a call site.  The box is an id scope like a child -- two boxes
       may each hold an "ok".  STACK and COLUMNS only: neither a grid nor a pack has a content
       height to measure, so a box in one is an inert scope.  Always balance; nesting caps at 8. */
    void ( *box_begin )( const char* label, gui_style_role_t role );
    void ( *box_end   )( void );

    /* Tab bar -- a row of clickable chips inside ONE window that switches which section of that
       window's body is shown below it (a Settings dialog's General / Audio / Video tabs). An
       in-window tabbed content switcher (the ImGuiTabBar analogue): a strip of
       clickable chips with only the selected tab's body emitted below it.  Distinct from docking,
       which tabs whole windows into a dock node -- this tabs SECTIONS of one window's body.

       tab_bar_begin opens the bar and reserves the strip row (false only when the nesting cap is
       hit).  Each tab_item_begin draws one chip and returns true only for the selected tab.  Both
       returns gate their body; both ends are unconditional.  The active selection persists per bar
       id; the first tab is the default.  p_open (optional, may be NULL): when non-NULL a close (x)
       appears on the chip and clicking it sets *p_open = false (the caller drops the item next
       frame).

           if ( gui()->tab_bar_begin( "settings", GUI_TAB_BAR_NONE ) )
           {
               if ( gui()->tab_item_begin( "General", NULL, GUI_TAB_ITEM_NONE ) )
               {
                   gui()->checkbox( "Vsync", &vsync );
               }
               gui()->tab_item_end();

               if ( gui()->tab_item_begin( "Audio", NULL, GUI_TAB_ITEM_NONE ) )
               {
                   gui()->slider_float( "Volume", &vol, 0.0f, 1.0f );
               }
               gui()->tab_item_end();
           }
           gui()->tab_bar_end(); */
    bool ( *tab_bar_begin  )( const char* id_str, gui_tab_bar_flags_t flags );
    void ( *tab_bar_end    )( void );
    bool ( *tab_item_begin )( const char* label, bool* p_open, gui_tab_item_flags_t flags );
    void ( *tab_item_end   )( void );

    /*  split_begin / split_next / split_end -- two panels side by side sharing a Y-level.

        split_begin( id, right_w ) opens a split: the left panel fills, the right panel is
        right_w pixels wide.  split_next() closes the left panel and opens the right.
        split_end() closes the right panel.  Each panel is an independent flow region -- declare
        a layout mode (stack/cols/...) inside each as usual.  Heights are cached per-id across
        frames (one-frame lag on first appearance, then stable).

        Use button_width() to size the right panel to fit a specific button label exactly:

            const char* title = "Bake & Preview";
            gui()->split_begin( "##src", gui()->button_width( title ) );
                gui()->stack();
                gui()->combo_begin( ... ); ... gui()->combo_end();
                gui()->slider_int( ... );
            gui()->split_next();
                gui()->stack();
                gui()->button_fill( title );
            gui()->split_end();

        button_width( label ) -- natural pixel width of a button with that label.
        button_fill  -- a button that fills the remaining height of its containing region.
        Identical to button() but height = content_avail().y.  Designed for the right panel
        of a split so it matches the adjacent left panel's content height naturally. */

    f32  ( *button_width  )( const char* label );
    bool ( *button_fill   )( const char* label );

    /*==========================  table/ -- multi-column rows over the layout engine  ===========================*/

    /* Tables -- a spreadsheet-style grid of rows and columns, with sortable/resizable/reorderable
       headers and a scrollbar, for anything a plain list of rows is too flat to show. A
       multi-column layout with self-fitting cells (one table clip, no per-cell clip) and
       optional scrolling, sortable headers, and resizable columns.  Conceptually a grid whose
       rows accumulate and scroll (like flow) with column tracks resolved once per table (like
       grid), plus frozen header support.

       USAGE CONTRACT:
         1. table_begin()            -- open the table; returns true (always, like child_begin).
                                        Consume it paired with table_end() regardless.
         2. table_setup_column()     -- call ncols times between table_begin and the first row.
                                        The calls may be omitted; all columns default to stretch.
         3. table_headers_row()      -- optional; draws and clips a non-scrolling header strip and
                                        runs header sort-click interaction.
                                        Call after all table_setup_column, before the first data row.
         4. for each row:
              table_next_row()       -- begin a new data row.  First call sets row 0.
              for each column:
                table_next_column()  -- advance to the next column and return true; the cell sizes the
                                        widget and long text ellipsizes to the column (self-fit, no
                                        per-cell clip).  Returns false past the last column.
                <emit widgets>       -- normal widget calls; they land inside the cell.
         5. table_end()              -- close the table; restores the parent layout.

       Column widths use the overloaded-unit rule (same as columns / grid):
           > 1.0  fixed pixels   1.0  fill / stretch   (0,1)  fraction   0.0  natural (= stretch)
       Height: 0 = auto (8 rows tall), > 0 = fixed pixels.

           if ( gui()->table_begin( "my_table", 3, GUI_TABLE_NONE, 0 ) )
           {
               gui()->table_setup_column( "Name",  GUI_TABLE_COL_STRETCH,  0     );
               gui()->table_setup_column( "Value", GUI_TABLE_COL_FIXED,    80.0f );
               gui()->table_setup_column( "Unit",  GUI_TABLE_COL_FIXED,    40.0f );
               for ( i32 i = 0; i < count; ++i )
               {
                   gui()->table_next_row( 0 );
                   if ( gui()->table_next_column() ) gui()->text( name[i]  );
                   if ( gui()->table_next_column() ) gui()->text( value[i] );
                   if ( gui()->table_next_column() ) gui()->text( unit[i]  );
               }
               gui()->table_end();
           }

       table_set_column_index( col ) -- jump to a specific column (0-based) rather than advancing.
                                        Returns false when that column is hidden this frame.
       table_get_column_count()      -- number of columns the table was opened with.
       table_get_column_index()      -- current column index (-1 before the first next_column).
       table_get_row_index()         -- current row index (-1 before the first next_row).
       table_get_sort_specs( out )   -- read raw sort state (column + direction); out is filled
                                        whenever a table is open, and the RETURN says the sort
                                        changed this frame.  Use when you want to sort your own
                                        data structure by hand.
       table_sort_order( order, n, val_fn, cmp_fn, user )
                                     -- built-in sort: reorder a display-order index array to match
                                        the active sort.  Pass val_fn for automatic alphabetical /
                                        numeric ordering, or cmp_fn for a custom comparator.  Cheap
                                        to call every frame (only reorders when the sort changed --
                                        a header click, or the first frame of a DEFAULT_SORT column).
       table_set_bg_color( target, abgr ) -- override the current row's or cell's background.

       COLUMN MANAGEMENT.  Widths, display order, and visibility persist per table id, and the
       user drives all three directly: drag a boundary to resize (GUI_TABLE_RESIZABLE),
       double-click one to size that column to its content, drag a header sideways to reorder
       (GUI_TABLE_REORDERABLE), and right-click the header for the built-in menu (size-to-fit,
       reset, and a checkbox per column with GUI_TABLE_HIDEABLE).  GUI_TABLE_NO_CONTEXT_MENU
       suppresses that menu for a table that wants the right button itself.  Programmatically:

       table_is_column_visible( col )        -- is that logical column shown this frame.
       table_set_column_visible( col, vis )  -- show / hide it (refused for NO_HIDE and for the
                                                last visible column).
       table_get_hovered_column()            -- logical column under the cursor, -1 if none.  Pair
                                                with GUI_TABLE_HIGHLIGHT_COL for the tint.
       table_fit_column( col )               -- size a column to its widest measured content
                                                (col < 0 = every visible column).
       table_reset_columns()                 -- widths, order, and visibility back to the setup. */

    bool ( *table_begin            )( const char* id_str, i32 ncols, gui_table_flags_t flags, f32 height );
    void ( *table_end              )( void );
    void ( *table_setup_column     )( const char* label, gui_table_col_flags_t flags, f32 width );
    void ( *table_headers_row      )( void );
    void ( *table_next_row         )( f32 min_h );
    /* table_rows_clip -- rows_clip's table face: call after the header with the same min_h the
       rows pass to table_next_row (0 = WIDGET_H), then loop only the returned [first, last).
       Stripes/dividers keep phase and the scrollbar sees all `count` rows; use with SCROLL_Y. */
    gui_span_t ( *table_rows_clip  )( i32 count, f32 min_h );
    bool ( *table_next_column      )( void );
    bool ( *table_set_column_index )( i32 col );
    i32  ( *table_get_column_count )( void );
    i32  ( *table_get_column_index )( void );
    i32  ( *table_get_row_index    )( void );
    bool ( *table_get_sort_specs   )( gui_table_sort_specs_t* out );
    bool ( *table_sort_order       )( i32* order, i32 count, gui_table_sort_value_fn val_fn,
                                      gui_table_sort_cmp_fn cmp_fn, void* user );
    void ( *table_set_bg_color     )( gui_table_bg_target_t target, u32 abgr );

    bool ( *table_is_column_visible  )( i32 col );
    void ( *table_set_column_visible )( i32 col, bool visible );
    i32  ( *table_get_hovered_column )( void );
    void ( *table_fit_column         )( i32 col );
    void ( *table_reset_columns      )( void );

    /* window_set_drag() -- select how windows may be dragged (global default TITLEBAR).
       Call between frames; affects every window. */
    void ( *window_set_drag )( gui_win_drag_t mode );

    /* window_set_nav() -- aim keyboard navigation at a window by title (the explicit-focus entry).
       Clears the nav cursor so the window's first item takes focus and engages the nav highlight.
       Nav otherwise follows the front-most window automatically; Ctrl+Tab cycles among windows and
       Alt enters the main menu bar.  An open popup / menu always captures nav while it is open. */
    void ( *window_set_nav )( const char* title );

    /*==========================  theme -- chrome's named style presets (style kit)  ===========================*/

    /* Theme -- switch the whole application's look with one call by name ("Dark", "Light", ...).
       Named style presets that form the root of the push/pop stack.

       theme_list()  -- returns the built-in theme array and writes the count to *count_out.
       theme_set()   -- copies the named theme into the base style and immediately resets the
                        push stacks; returns false if the name is not found (no-op).
       theme_get()   -- returns the active theme name, or NULL after a raw style_get() edit.
       theme_reset() -- if a named theme is active, restores the base from it; then clears the
                        color + var push stacks (the "large style change" escape hatch -- call
                        this instead of issuing many paired push/pop calls just to revert).

           u32 n;
           const gui_theme_t* list = gui()->theme_list( &n );
           gui()->theme_set( list[0].name );       // switch to first built-in
           // ... many style pushes ...
           gui()->theme_reset();                   // clear everything, back to base */

    const gui_theme_t* ( *theme_list  )( u32* count_out );
    bool               ( *theme_set   )( const char* name );
    const char*        ( *theme_get   )( void );
    void               ( *theme_reset )( void );

    /*============================================================================================================
        GUI_DEBUG -- overlays, dashboard, stepper  (debug/)
        Tools for looking INSIDE the GUI while it runs: performance numbers, hit-test rects, a
        memory dashboard, a frame-by-frame command stepper. None of it is needed to ship a
        product; a release build can drop this band entirely. Diagnostic surfaces and
        retained-cache levers, hotkey-armed via debug_enable.
    =============================================================================================================*/

    /* Debug overlay -- a separate draw list painted last, on top of the UI.  Pass a bitmask
       of gui_dbg_layer_t to debug_set_layers() to choose which visualizations show; pass
       GUI_DBG_NONE (0) to turn it off.  Compiled in for Debug builds only: in Release,
       set_layers is a no-op and get_layers returns 0.  The two slots stay in the vtable in
       every build so func_api_size is identical across a hot-reload. */

    void ( *debug_set_layers )( u32 layers );
    u32  ( *debug_get_layers )( void );

    /* Master debug switch -- flip this on and gui's debug hotkeys and overlays take over
       entirely; the host does not need to add anything to its own loop. When on, gui owns the
       debug hotkeys and overlay emission -- the host adds nothing to its loop.  Every hotkey
       below is gated behind a master ARM so the broad
       single-letter keys never fire during normal use:

         '.'     master arm (main row or NP_DOT): toggle every debug hotkey below on / off as a
                 group; off by default, so nothing below responds until it is armed.  Disarming
                 resets every debug mode back to normal (overlays off, selector menu closed,
                 render mode normal, layers cleared) and re-arming restores the selector menu's
                 remembered lever values (debug_restore, gui_frame_overlay.c).  While a stepper
                 freeze is live the main-row '.' scrubs the replay instead; NP_DOT still disarms.
         NP1-NP5 debug overlay layers (window frames / interaction rects / resize bands / layout /
                 clips; Debug builds)
         NP6     content-rect outlines over scrollable regions (GUI_DBG_CONTENT -- drawn in
                 the main list so the box scrolls with the content it measures)
         NP7     region screen geometry (GUI_DBG_REGION -- view rect, reserved scrollbar
                 gutters, body interaction clip)
         F9      render mode: normal -> wireframe -> batch tint
         F10     pipeline dashboard window (backend memory maps / uploads / batches)
         NP+     perf overlay tier   (off / FPS / +timings / +counts & lever status / +retained)
         NP-     state overlay tier  (off / ids / +focus,nav / +popups)

       While armed, a dense checkbox/slider selector menu (right edge of the viewport) is also up:
       retained skip (tessellation cache), force redraw, and idle skip are toggled there as
       checkboxes, alongside the NP+/NP- tiers as sliders.  A host that
       writes set_force_redraw (or set_retained_skip / set_idle_skip) itself every frame should
       check debug_hotkeys_armed() first and stand down while armed, or its own write will fight
       the menu's checkbox every frame the two disagree -- see debug_hotkeys_armed below.

       The perf / state overlays and the dashboard are emitted internally, last in the default
       context's build (at its ctx_end), so they draw on top and are counted like any widget.
       Letter hotkeys are fenced by want_capture_keyboard, so typing never toggles them. */
    void ( *debug_enable     )( bool enable );
    bool ( *debug_is_enabled )( void );

    /* debug_hotkeys_armed -- true while the NP_DOT master arm is on, i.e. the selector menu is up
       and owns force redraw / retained skip / idle skip.  A host with its own per-frame lever
       write (sb_gui_editor's scene-pass set_force_redraw is the reference case) should gate that
       write on !debug_hotkeys_armed() so the menu's checkbox wins instead of being silently
       reverted the next time the host's own trigger condition re-fires. */
    bool ( *debug_hotkeys_armed )( void );

    /* Debug render mode -- how the main UI draw list is rasterized (gui_render_mode_t): NORMAL,
       WIREFRAME (triangle edges), or BATCH (per-draw-call color tint).  A pipeline + push-constant
       switch, so it is live in every build (not gated to Debug like the overlay layers above). */
    void              ( *debug_set_render_mode )( gui_render_mode_t mode );
    gui_render_mode_t ( *debug_get_render_mode )( void );

    /* Dump the retained cache's slot table (each window's vertex/index/command bounds) to stdout.
       Debug builds also assert every frame that no two slots share buffer space, so a geometry-
       corruption bug traps at its source instead of showing as flicker/warping downstream. */
    void                ( *debug_dump_geometry )( void );

    /* Style-record census -- the session-wide histogram of what the tessellator emits, and of the
       arena entries each distinct record costs across window slots (the figure a shared palette
       entry reclaims).  F7 dumps it interactively; this is the scripted entry, so a driver can
       run a fixed workload under several themes or DPI scales and label each run.

       `tag` labels the dump in the log; NULL dumps nothing, so ( NULL, true ) is a bare clear.
       Each record prints with a content HASH, which is what makes two runs comparable: a hash
       present in both runs is a record no style var moved.

       Debug builds only (GUI_PRIM_CENSUS).  The slot exists in every build; Release warns once
       and does nothing. */

    void                ( *debug_prim_census )( const char* tag, bool clear );

    /* Prim palette mode -- the A/B lever behind the debug selector menu's "style pal" slider,
       one axis of three (gui_palette_mode_t, gui.h):

         GUI_PALETTE_LEARNING  (default) a style the frame-global palette holds costs the emitting
                               window no arena record at all, and a record the palette does not
                               hold yet earns a shared entry once the frame has drawn it again --
                               which is how a UI layer the engine has never seen gets the same
                               coverage chrome does.
         GUI_PALETTE_FROZEN    the table answers with what it already learned; anything new mints
                               a per-slot record.  Measures the learned set's coverage -- and only
                               says anything once a session HAS learned something.
         GUI_PALETTE_OFF       every window mints its own copy exactly as it did before the palette
                               existed: more prim records, the same pixels.

       ANY visible difference between the modes is a palette bug, which is what the lever is for.

       Not free to flip -- cached geometry carries the answers the old mode gave, so every change
       but LEARNING -> FROZEN re-places every window's geometry.  A debug lever, not a per-frame
       one.  Scripted so a driver can census the same workload each way and diff the dumps. */

    void                ( *debug_set_prim_palette )( gui_palette_mode_t mode );
    gui_palette_mode_t  ( *debug_prim_palette     )( void );

    /* Retained-skip: when on (default), an unchanged frame skips tessellation.  Toggle to benchmark
       or confirm that the hash-upfront path produces identical output to the reference. */
    void ( *set_retained_skip )( bool on );
    bool ( *retained_skip     )( void );

} gui_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( GUI_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    MOD_GATEWAY_STATIC( gui_api_t, gui )
    #define MOD_USE_GUI    /* static build */
    #define MOD_FETCH_GUI  true
#else
    MOD_GATEWAY_DYNAMIC( gui_api_t, gui )
    #define MOD_USE_GUI    MOD_DEFINE_API_PTR( gui_api_t, gui )
    #define MOD_FETCH_GUI  MOD_FETCH_API( gui_api_t, gui )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GUI_API_H
