#ifndef GUI_H
#define GUI_H
/*==============================================================================================

    runtime_service/gui/gui.h -- gui module types (the public type header).

    This file has no functions, just definitions -- the shape of every "noun" the GUI uses.
    For the deep dive on why it is built this way, see GUI_ARCHITECTURE.md in this directory.

    GUI is ORB's in-house immediate-mode 2D interaction renderer, built from two servers
    that never see each other:

    1. An INTERACT SERVER that tracks what the user is doing (hover, click, focus, drag).
    2. A RENDER SERVER that turns draw commands into GPU triangles.

    Everything else -- layout, style, stock widgets, window chrome -- is a library layered
    on top of those two, in the strata order the section banners below follow. Chrome
    (windows, docking, popups) is the topmost layer and is OPTIONAL: a game can build its
    whole UI directly out of the lower strata and never open a window.

    It sits between two engine layers: windowing and input come in from `app` (Win32);
    GPU submission goes out through RHI (Vulkan). A host drives one cycle every frame:

        frame_begin -> ctx_begin / widgets / ctx_end -> frame_end -> render.

    A context (gui_context_t, addressed by a plain i32 handle) owns one bundle of retained
    UI state -- windows, docks, nav, scroll links. GUI_CTX_DEFAULT (0) is always live after
    init(); ctx_create() opens a secondary context for an isolated sub-UI (e.g. an in-game
    menu separate from the editor). A context renders into one or more viewports -- OS
    windows, each backed by its own RHI surface.

    The unit roster is the `unit` list under `target gui` in orb.targets.

    A GUI value lives in one of three places, which decides how long it survives:
    1. Ambient singular state -- one shared copy for the whole app (e.g. s_interaction, s_io).
    2. Per-context retained state -- One gui_context_t, survives across frames (g_ctx).
    3. Frame scratch -- wiped clean at the start of every frame (e.g. s_build, s_scope).

    Two caches exist purely to make an idle screen cheap to redraw:
    1. CPU emit skip (s_frame_dirty, gui_frame_loop.c): one global bool. If nothing changed --
       no input, no animation, no render delta -- the entire widget-building pass is skipped
       and last frame's draw list is reused as-is.
    2. GPU tessellation cache (gui_build_cache.c): the same idea, but per window instead of
       global. Only a window whose content actually changed gets re-tessellated; every other
       window on screen reuses its existing GPU geometry untouched.

    Below, types are listed in DEPENDENCY order -- a struct comes after the enums it embeds --
    and each section banner is tagged with the gui_api.h strata band it belongs to, so the type
    list and the function list read in the same order. Band -> sections:

    GUI_FRAME    -- context config, font config, boot descriptor, limits, mem + render stats
    GUI_DRAW     -- angle algebra, color packing, stroking, draw vertex, volatile cb,
                    semantic draw commands, direction
    GUI_CORE     -- ids, item state, item flags, drag and drop, easing
    GUI_SURFACE  -- pane, region z tier (win flags shared from GUI_CHROME)
    GUI_RECT     -- anchor frame, split axis
                    (the leaf geometry types + rect algebra live in gui_rect.h, included first)
    GUI_FLOW     -- layout template / modes, pack, field label side
    GUI_STYLE    -- the color grid (role x phase), density ramp, style vars, shape picks, the
                    style struct: ONE schema, and every layer above names its look in it
    GUI_CHROME   -- themes, window drag / cond / flags, dockspace, combo, tab bar, tables,
                    color edit flags
    GUI_DEBUG    -- overlay layers, render mode

==============================================================================================*/

#include "orb.h"
#include "runtime_service/gui/log/gui_log.h"        // GUI_LOG leaf floor: gui_log + GUI_WARN_ONCE
#include "runtime_service/gui/rect/gui_rect.h"      // GUI_RECT leaf kit: geometry types + carve math

// clang-format off
/*==============================================================================================

    GUI_CORE -- ids

==============================================================================================*/

/* Widget id -- a hashed value creates a unique value to identify a widget */

typedef u32 gui_id_t;
#define GUI_ID_NONE 0u

/* Icon handle -- identifies one symbol packed into the runtime icon atlas (register_icon).
   The atlas is a second R8 coverage texture that lives beside the font atlas and batches in
   the same flush; icons draw as tinted quads via image / draw_icon_in.  0 means "no icon"
   (an unregistered name or a full atlas), and draw helpers no-op on it. */

typedef u32 gui_icon_id_t;
#define GUI_ICON_NONE 0u

/* Sprite handle -- identifies one piece of authored RGBA art packed into the sprite atlas
   (register_sprite / load_sprite).  Where an icon is a COVERAGE mask the vertex colour paints,
   a sprite carries its own colour and the vertex colour only tints it -- the difference between
   a symbol and a picture.  A sprite optionally carries nine-slice insets (sprite_set_slice), and
   then any rect it fills keeps its corners at authored size while its edges and middle stretch or
   tile: that is what makes a frame, a panel skin, or a button face art rather than code.  0 means
   "no sprite" (an unregistered name or a full atlas), and every draw helper no-ops on it. */

typedef u32 gui_sprite_id_t;
#define GUI_SPRITE_NONE 0u

/* Viewport handle -- a plain i32 index into the global viewport pool, naming a render
   surface backed by an OS window.  Returned by viewport_open; passed to render,
   viewport_resize, viewport_close, and window_set_next_viewport.  A valid handle is
   non-negative; GUI_VP_INVALID (-1) signals failure or no assignment, matching the raw
   i32 window id and rhi context handle. */

#define GUI_VP_INVALID (-1)
#define GUI_VP_MAIN 0    /* the host's primary swapchain surface (first window opened) */

/* Opaque dock-node handle -- one region of a viewport's dock tree.  Returned by dockspace_over_viewport
   (the tree root) and dock_split (the new sibling), and passed to dock_split / dock_window to name a
   target region.  0 (GUI_DOCK_NONE) signals "no node" -- a failed call or an unassigned slot. */

typedef u32 gui_dock_id_t;
#define GUI_DOCK_NONE  0u

/* Context handle -- a plain i32 index into the internal context pool.
   GUI_CTX_DEFAULT (0) is always valid after init().
   GUI_CTX_INVALID (-1) signals a failed ctx_create or an unset handle. */

#define GUI_CTX_DEFAULT  0
#define GUI_CTX_INVALID  (-1)

/*==============================================================================================
    GUI_FRAME -- context configuration

    Context configuration -- sizes the per-context resource pools at creation time.
    Pass to ctx_create(); zero fields (and a NULL cfg) mean "the internal maximum" -- the
    compile-time caps the library was built with.  The default context (slot 0) always uses
    those maxima; a config only ever scales a secondary context DOWN.
    max_dock_nodes == 0 in an explicit cfg is valid and disables docking for that context.
==============================================================================================*/

typedef struct
{
    u32  max_windows;    // persisted window pool
    u32  state_slots;    // keyed state pool: tiny-class slot count; the small class gets
                         //   3/4 of it, the big class is fixed (GUI_STATE_BIG_SLOTS)
    u32  popup_depth;    // max popup nesting
    u32  max_dock_nodes; // dock-tree node pool; 0 = no docking (NULL cfg keeps the default)

} gui_ctx_config_t;

/* NOTE: render surfaces are NOT sized here -- viewports are a single global table shared by
   every context (OS windows and RHI contexts are a genuinely global, small, fixed-size resource;
   see s_vp_pool, core/gui_ctx.h), not a per-context pool. */

/* Pre-built config -- a deliberately small profile for lightweight in-game UI contexts */
#define GUI_CTX_CONFIG_GAME_UI \
    ( ( gui_ctx_config_t ){ 8, 64, 4, 0 } )

/*==============================================================================================
    Shared leaf types -- spans, easing, callback typedefs
==============================================================================================*/

/* visible index range [first, last) returned by the row clippers (rows_clip / table_rows_clip) */
typedef struct { i32 first, last; } gui_span_t;

/* one entry of a draw_rects batch: a solid fill plus its color (see GUI_CMD_RECT_LIST) */
typedef struct { f32 x, y, w, h; u32 abgr; } gui_rect_col_t;

/* Four independent animation channels -- the fixed storage unit the animation service (gui_anim4)
   steps in one keyed slot.  A widget packs what it needs (2 for a hover/active blend, 4 for a color
   or an x/y/w/h rect) and leaves the rest at 0; the layout is four contiguous floats. */
typedef struct { f32 x, y, z, w; } gui_anim4_t;

/* Easing shapers for the fixed-duration tween (anim_ease).  Named across the public boundary as an
   enum so callers never reach into the base math_ease.h symbols and the gui owns the canonical
   curve table.  The short menu is the set worth naming for UI transitions; add here as needed. */
typedef enum
{
    GUI_EASE_LINEAR = 0,   // no shaping
    GUI_EASE_SMOOTH,       // smoothstep01 -- gentle in/out, the everyday default
    GUI_EASE_IN_CUBIC,     // accelerate from rest
    GUI_EASE_OUT_CUBIC,    // decelerate into the target
    GUI_EASE_INOUT_CUBIC,  // accelerate then decelerate, stronger than SMOOTH
    GUI_EASE_OUT_EXPO,     // sharp arrival, long tail-in (snappy panels)
    GUI_EASE_OUT_BACK,     // slight overshoot past the target then settle
    GUI_EASE_COUNT
} gui_ease_t;

/* Callback fired by input_text_ex after any frame that modifies the buffer.
   buf is the live caller-owned buffer (may be read or written); len is the current byte
   length (excluding NUL); bufsz is the total buffer capacity. */
typedef void ( *gui_text_cb_fn )( char* buf, u32 len, u32 bufsz, void* user );

/* Key hook consulted by the FOCUSED text field before its own key handling
   (see set_edit_key_hook).  Called once per key down this frame; key is an app_key_t value,
   repeat is true on OS auto-repeat ticks (false on the initial press).  Return true to
   consume: the key is cleared from the frame io, so neither the field nor any later widget
   acts on it -- the Quake-console passthrough (history, completion, scrollback keys). */
typedef bool ( *gui_edit_key_fn )( u32 key, bool ctrl, bool shift, bool repeat, void* user );

/* The installed-element-style OWNER (see style_source_set / style_set_create, GUI_STYLE).
   Invoked at every style landing (font activation, theme_set / theme_reset / style_apply) AFTER
   the layout metrics rescale, so the owner re-derives its look against fresh numbers.  The
   source writes the installed style through gui()->style_edit(), which points at the set being
   installed for the duration of the call. */
typedef void ( *gui_style_source_fn )( void* user );

/* A style SET -- one installed copy of gui_style_t, the whole schema.  Set 0 is chrome's and always
   exists; gui()->style_set_create() takes another, and style_set_push / _pop bracket the UI
   that resolves through it.  Two looks stay installed side by side, so an editor's chrome and
   a game's kit can each own one instead of overwriting a single shared palette. */
typedef u32 gui_style_set_t;

#define GUI_STYLE_SET_DEFAULT ( ( gui_style_set_t )0 )   /* chrome's set */
#define GUI_STYLE_SET_MAX     4                          /* installed sets, chrome's included */

/* Monotonic wall-clock source (seconds), supplied by the host to the built-in perf overlay.
   gui has no timing service of its own (it is a leaf of rhi + app), so the host hands it a
   tick-seconds callback -- typically sys()->tick_seconds -- and gui uses it to measure the
   per-frame emit (build) and render (flush) cost the overlay reports.  See frame_set_hooks(). */
typedef f64 ( *gui_clock_fn )( void );

/* Host OS services for end-of-frame pacing (see frame_set_hooks / boot_pace).  gui links only
   app + rhi, so the sleep and the block-on-input wait are handed in as callbacks -- typically
   sys_sleep_milliseconds and sys_wait_for_os_events_ms.  A NULL member disables the feature that
   depends on it (no sleep -> boot_pace never sleeps; no wait -> idle skip unavailable). */
typedef void ( *gui_sleep_fn )( i32 milliseconds );
typedef void ( *gui_wait_events_fn )( i32 timeout_ms );

/*==============================================================================================
    GUI_STYLE -- the color grid: what a color is FOR, and when

    THE color vocabulary of the whole GUI, and there is no second one.  Chrome, the stock
    widgets, and a kit's own renders all name a color as a (role, phase) cell, and all of them
    resolve through the same instanced style -- so "the editor look" and "the game look" are two
    instances of one schema rather than two schemas.  There is no flat color enum: nine roles
    times four phases ARE the 36 cells of gui_style_t.col below.

        push_style_color( GUI_ROLE_BG,   GUI_PHASE_HOT, abgr );   // one cell, until the pop
        push_style_color( GUI_ROLE_TEXT_PRIMARY, GUI_PHASE_ALL, abgr );   // the whole phase row
        next_style_color( GUI_ROLE_BG,   GUI_PHASE_ALL, abgr );   // just the next widget
        push_style_seed ( GUI_SEED_ACCENT, abgr );                // re-seed, ramp intact

    Note what the last two do NOT have in common.  GUI_PHASE_ALL writes one value into all four
    cells of a row, which FLATTENS the ramp -- pushed on BG it gives you a button that no longer
    reacts to hover, so it is the right verb for TEXT or BORDER and the wrong one for anything
    interactive.  push_style_seed re-derives instead: the cells stay four different colours, one
    ramp step apart, just built from a new source.  That is what "recolour this UI" almost always
    means, and until the seeds existed there was no way to say it.

    The grid is shared rather than per-widget-type (one BG row, not Button + Checkbox + ...):
    to recolor one button, bracket it with push/pop, or use next_style_color for a one-shot.
    Deliberately NO per-widget slots (btn_bg_hover, slot_border_hot, ...) -- per-widget color is
    either a call parameter (stock_meter's fill) or a token in the kit above.  Colors are packed
    with GUI_COLOR (byte order R,G,B,A).

    A kit wanting colors of its OWN keeps them in its own struct and passes them to draw_*; a
    color no engine code reads has nothing to gain from living in the engine's grid.

    Two doors, and the difference matters: gui()->style_color( role, phase ) is the RESOLVED
    read (push_style_color / next_style_color overrides win) -- use it in any render, stock or
    your own; style_color_selected washes the same read for a widget that can be selected (see
    the SELECTED section below).  gui()->style_edit() is the raw installed struct of the CURRENT
    set, for a kit INSTALLING a look; reading ->col[][] through it at paint time bypasses the
    style stacks.
==============================================================================================*/

/* What the color is FOR.  Nine roles cover every surface the GUI paints.

   PANEL vs BG is the container / control split, and it is the one distinction that has to exist:
   a window body and a child region are surfaces the layout CARVES, while a button face, an input
   field, and a check box are surfaces a widget FILLS.  They recede and advance in opposite
   directions (a panel sits under its contents, a control sits over its panel), so one shared
   "background" cannot serve both.

   TITLE is the third surface kind: a caption band that LABELS a container rather than being one
   -- a window title bar, a tab, a menu bar, a table header.  It earns a row of its own because
   its four states are genuinely its own: folding it into PANEL would force the phase axis to
   mean something different for that one role, and a tab would have to reach into three roles
   just to say active / hovered / idle.  As its own role, a tab says TITLE and picks a phase,
   like everything else.

   ACCENT vs MARK is the same split one level down: two roles instead of one, because a role's
   four cells only make sense as a ramp if all four describe the SAME surface at different
   interaction depths.  A role whose phase slots each hold a different concern -- a value fill at
   IDLE, a nav ring at HOT, a check mark at ACTIVE, an empty track at INERT -- is a token table
   wearing a ramp's clothing: nothing keeps its ACTIVE cell related in hue to its IDLE cell, the
   one generic accessor the grid offers (style_col( role, item_phase( st ) )) can resolve to the
   wrong colour for what a widget means (a slider that reasons "lift WITHIN the role, INERT at rest"
   lands on the value-fill token, so a hovered slider paints its EMPTY half the filled colour),
   and push_style_color( role, PHASE_ALL ) has nothing coherent to recolor.  Split, both rows are
   honest ramps: ACCENT is the value a control HOLDS (empty track / fill / engaged / dragged) and
   MARK is the indicator it SHOWS (mark / nav ring / captured-nav ring / inert), so item_phase
   works correctly on either.

   GRAB is the movable part of a track control -- a slider knob, a scrollbar thumb -- and it earns
   a row for a reason no other surface has: it is the one element that must stay legible against
   TWO lifting neighbours at once.  A track control paints three layers, and each of the other two
   already owns a row: the track body lifts along BG (it is a control face, so it hovers like a
   button), and the value fill lifts along ACCENT.  A knob drawn from either row therefore
   collides with that neighbour in some phase -- on BG it matches the hovered track exactly, on
   ACCENT it matches the fill.  GRAB is authored per theme as the palette's CONTRAST ANCHOR,
   opposite in polarity to the theme itself (light on a dark theme, dark on a light one), which is
   a value no phase of a shared row can hold in both directions.

   TEXT_PRIMARY vs TEXT_SECONDARY is the same "a ramp must describe one surface" rule ACCENT/MARK
   was split for.  A single TEXT role cannot honestly hold both "the body ink, unreactive across
   IDLE/HOT/ACTIVE, with a caller-chosen quieter ink for the rare disabled READING (gui_text_
   disabled -- not the automatic GUI_ITEM_DISABLED dim, see the INERT section below)" and "a
   permanently quieter ink for hints, captions and inactive labels" -- those are two different
   surfaces that happen to both be text.  Split, PRIMARY's INERT cell stays that one hand-picked
   disabled-text ink, and SECONDARY is its own honest (if equally unreactive) ramp for the muted
   case -- SECONDARY never actually reads its own INERT cell (nothing asks for a doubly-quiet
   secondary ink today), which is fine: see UNUSED cells, below.

   PANEL vs PANEL_CHILD is the same "a ramp must describe one surface" rule ACCENT/MARK and
   TEXT_PRIMARY/SECONDARY were split for.  A window body and a nested scroll region both sit on
   PANEL's plane, but they are not one surface: the window body is flush with the page, while a
   child region is recessed AT REST and still needs its own standing-based HOT/ACTIVE -- a drop
   target and a scoped focus are as real for a child as for the window it lives in, independent of
   the window's own reading (child_standing_phase, flow/gui_layout_child.c).  Folding "recessed"
   into PANEL's INERT cell, as a single early build did, cost every child region that reading
   entirely -- INERT is one cell, not a phase, so a recessed panel could never also show as hot.
   Split, PANEL's INERT cell
   goes back to being the one thing every other role's INERT cell already was -- a non-interactive
   surface's look, permanent (gui_stock_panel's decorative backdrop, an empty dock-leaf
   placeholder) or temporary (a window fenced off by an active modal -- see the phase table below)
   -- and PANEL_CHILD carries its own full IDLE/HOT/ACTIVE/INERT ramp, seeded from a recessed
   ground.

   There is no STATUS row here.  INFO / OK / WARN / ERROR used to be four roles wearing the same
   ramp shape as everything above, but a severity signal is not a surface a widget hovers or
   presses -- it is a standing fact a caller already knows before it draws.  Baking it into the
   role/phase grid bought four roles' worth of derivation and 8 cells per set for colours nothing
   ever animates.  The severity ladder now lives in the EXTENDED PALETTE (gui_style_ext_t,
   further down) instead: still theme-reachable and re-seedable, at the cost of one flat colour
   each rather than a ramp. */

typedef enum
{
    GUI_ROLE_PANEL = 0,       // container surface: window body
    GUI_ROLE_PANEL_CHILD,     // nested container: scroll region, embedded child panel
    GUI_ROLE_TITLE,           // caption band over a container: title bar, tab, menu bar, table header
    GUI_ROLE_BG,              // control surface: button face, input field, check box, cycle end caps
    GUI_ROLE_BORDER,          // frame line, focus ring, resize edge
    GUI_ROLE_TEXT_PRIMARY,    // glyphs, caret -- the body ink; INERT is gui_text_disabled's ink, not muted
    GUI_ROLE_TEXT_SECONDARY,  // a permanently quieter ink: hints, captions, shortcuts, inactive labels
    GUI_ROLE_ACCENT,          // the value a control HOLDS: slider / progress fill, empty track
    GUI_ROLE_MARK,            // the indicator a control SHOWS: check, radio dot, nav ring
    GUI_ROLE_GRAB,            // movable part of a track control: slider knob, scrollbar thumb

    GUI_ROLE_COUNT

} gui_style_role_t;

/* WHICH of a role's four colors -- the interaction step that selects the cell.  Called a PHASE,
   not a state, because gui_item_state_t is the interact server's flag set and GUI_STATE_* is the
   retained per-id pool: a phase is what item_phase() DISTILLS a state into for the style.

   The same three interaction steps mean the analogous thing for every WIDGET role -- INERT, the
   fourth column, does not: it is a genuinely different reading per role, confirmed by auditing
   every real call site in the widget set (2026-08-14).  PANEL and TITLE are the one further
   exception, and the reason they need one: a window's body and caption band cover far too much
   screen for a per-pixel cursor read to mean anything there, so their whole ramp answers "what is
   this WINDOW's standing" instead of "what is the cursor doing to this widget" (2026-08-14):

   IDLE     = AT REST                                       | PANEL/TITLE: open, unfocused
   HOT      = HOVER || NAV                                  | PANEL/TITLE: a drag would land here
   ACTIVE   = PRESSED / CAPTURED / FOCUSED                  | PANEL/TITLE: this window holds focus
   INERT    = Non-Interact / Empty Value / Caller Decides

    role     IDLE              HOT                  ACTIVE                 INERT
    -------  ----------------  -------------------  ---------------------  ------------------

    PANEL    open, unfocused   valid drop target    focused / foreground   behind modal fence
    CHILD    recessed surface  valid drop target    focus inside child     behind modal fence
    TITLE    bar, inactive tab chip hov / drop      focused bar, live tab  de-emphasized bar

    BG       control face      hovered face         pressed / focused      plot backdrop
    BORDER   frame line        hovered / resize     focused window ring    subdued frame
    TEXT_PRI body text, caret  unused               unused                 disabled-text ink
    TEXT_SEC secondary text    unused               unused                 unused
    ACCENT   value fill        engaged fill         dragged fill           empty track
    MARK     check, radio dot  nav ring             captured-nav ring      unused
    GRAB     knob / thumb      hovered knob         dragged knob           unused

    status   the signal        hovered signal       pressed signal         the FIELD (banner)

   PANEL/HOT, CHILD/HOT and TITLE/HOT (the band, not the chip) share one formula: a wash toward
   the GUI_EXT_INFO hue, read while a drag gesture is in flight and this surface is the computed
   landing target -- a frame-level fact, not a cursor-over-pixel one, which is why it stays IDLE
   under an ordinary mouse-over.  Only the window path is wired today: PANEL and the TITLE band
   read window_route_is_drop_target (chrome/dock/gui_dock_route.c), true only for a drag-to-dock
   gesture.  CHILD has no query wired to it yet -- a child is not part of the dock tree, so
   lighting one up needs a generic drag-payload target check that does not exist yet -- so its
   HOT cell is baked and ready but never read until that lands.  A tab CHIP (col_tab_bg) is the
   one place under TITLE that still reads HOT as plain cursor hover, same as every other role: it
   is a small, individually-hoverable target the way a button is, unlike the band it sits on.

   PANEL/ACTIVE, CHILD/ACTIVE and TITLE/ACTIVE all read "the keyboard cursor is scoped to this
   surface", by different means: PANEL and CHILD both lift their ground a faint step so the eye
   can find the live surface without the fill competing with the content painted over it -- PANEL
   reads the window's own focus (nav.focused_win), CHILD reads whether the focused widget is
   scoped to THIS child specifically (s_interaction.focused_win, which pane_tag stamps to the
   child's id on entry, not the enclosing window's) -- while TITLE/ACTIVE is authored as the bare
   window BODY colour in every built-in theme, which is what makes a live tab merge into the panel
   it owns.  BORDER/ACTIVE carries the strongest version of the window-level fact (a full focus
   ring) -- three surfaces, one signal, weighted so the ring does the convincing and the other two
   do not have to fight it for attention.

   INERT is at least four unrelated ideas wearing one column, and no single word covers all of
   them honestly -- "inert" is the least wrong:

     - an empty VALUE: ACCENT's track has nothing in it yet (progress bar, scrollbar, slider
       groove before a drag engages it and the fill lifts to a different role, BG)
     - a permanently NON-INTERACTIVE surface: PANEL's decorative backdrop (gui_stock_panel), an
       empty dock-leaf placeholder, BORDER's matching frame around either, and a plot's own
       backdrop (BG) -- these never react to the mouse because nothing behind them is an item,
       not because anything disabled them
     - a TEMPORARILY fenced-off surface: PANEL/INERT and TITLE/INERT also read for a window
       sitting behind an active GUI_WIN_MODAL's hover/focus fence (focus_allowed() false,
       core/gui_focus.c) -- a live, otherwise-ordinary window the modal holds exclusive input over
       for as long as it stays open, not a decorative one that was never interactive to begin with
     - a CALLER'S OWN one-off pick: TITLE/INERT is also read for a maximized window's titlebar;
       TEXT_PRIMARY/INERT is read only by gui_text_disabled, a hand-chosen ink -- see below

   INERT is NOT a widget-disabled colour, for any role, anywhere.  A disabled item is handled by
   two mechanisms entirely outside this grid: item_state() forces its phase to IDLE (hover/active
   can never fire, so nav and click both no-op), and item_flags_resolve() multiplies the whole
   draw's alpha by DISABLED_ALPHA.  A disabled widget's face is therefore its own ordinary IDLE
   cell, just dimmer by that alpha -- never a role's INERT cell.  gui_text_disabled is the one
   exception, and it is a caller convention, not the GUI_ITEM_DISABLED mechanism: nothing ties the
   two together, so a caller can invoke one without the other being true.

   UNUSED in the table marks a cell gui_bake.c still computes -- the grid stays uniform, so a
   theme author never hits a hole -- but that nothing in the current widget set reads.  Those
   cells bake to a loud sentinel colour (BAKE_UNUSED, gui_bake.c) instead of a plausible one, so a
   future accidental read is an obvious visual bug instead of a quiet wrong guess.

   Note what is NOT on this axis: whether the item is SELECTED.  Selection persists across
   frames; a phase does not -- so folding SELECTED into ACTIVE would cost every list row its
   hover feedback (a selected row could never also show as hovered).  Selection lives on its own
   axis, the look axis below. */

typedef enum
{
    GUI_PHASE_IDLE = 0,   // at rest
    GUI_PHASE_HOT,        // cursor over / keyboard nav on the item
    GUI_PHASE_ACTIVE,     // pressed / captured / focused
    GUI_PHASE_INERT,      // empty value, non-interactive or fenced-off surface, or a caller's own pick -- never "disabled"
    GUI_PHASE_COUNT,

    /* Not a cell -- the "whole row" selector push_style_color takes, so recoloring TEXT or BORDER
       is one balanced push instead of four.  Only the push / next verbs accept it; a read names
       one phase. */
    GUI_PHASE_ALL = GUI_PHASE_COUNT

} gui_style_phase_t;

/*==============================================================================================
    GUI_STYLE -- SELECTED: a wash, not a stored axis

    Role answers "what surface is this"; phase answers "what is happening to it right now".
    Selected answers neither -- it is a persistent fact about the CALLER's data (this row is the
    chosen one), not an identity and not a transient interaction, so it does not get a cell of
    its own in the grid.  Instead it is a live transform applied to whatever colour role+phase
    already resolved to: the resolved colour washed toward the theme's accent by GUI_RAMP_SELECT.

        style_col         ( GUI_ROLE_BG, phase )   // the plain read
        style_col_selected( GUI_ROLE_BG, phase )   // the same cell, washed toward the accent

    This is what lets "selected" compose with hover for free: style_col_selected washes whatever
    phase cell it is handed, so a selected-and-hovered row is the HOT cell washed, not a fourth
    cell nobody baked.  There is deliberately no gui_item_look() beside gui_item_phase(): a phase
    is distilled from interact state because interaction is what the server watches, while only
    the caller knows what is selected.  A widget takes selected as a bool argument, the same way
    gui_selectable already does.
==============================================================================================*/

/*==============================================================================================
    GUI_STYLE -- the MIX: where an item sits BETWEEN cells

    Phase is an enumeration, and an enumeration cannot express "most of the way to hovered".
    That is the whole reason a widget snaps: it names one cell per frame, so the only motion
    available to it is the jump from one cell to the next.

    A mix is the continuous coordinate over the grid plus the selected wash -- three weights that
    say how far the item has travelled from its resting cell:

        hot   0 -> 1   toward the HOT phase       (cursor over / keyboard nav on it)
        act   0 -> 1   toward the ACTIVE phase    (pressed / captured)
        sel   0 -> 1   into the selected wash     (chosen: toggled, open, selected row)

    Read it once per item with style_mix (gui()->style_mix), which owns the damper storage, then
    spend it on as many rows of the grid as the widget paints -- surface, border, ink.  One probe
    drives all of them, and they arrive together because they share the weights rather than each
    running a damper of its own.

    All zero is the resting item and 1/0/0 a fully hovered one, so a caller that wants no motion
    can build a mix by hand and never touch the animation service at all.
==============================================================================================*/

typedef struct gui_style_mix_t
{
    f32 hot;   // 0..1 travel toward GUI_PHASE_HOT
    f32 act;   // 0..1 travel toward GUI_PHASE_ACTIVE
    f32 sel;   // 0..1 travel into the selected wash

} gui_style_mix_t;

/*==============================================================================================
    GUI_STYLE -- The Seed Palette: What a theme AUTHORS

    Purpose: Simplified authoring.

    The grid above is what a RENDER reads. It is not what a theme WRITES. 72 literals would
    be the wrong authoring surface: many cells are structurally redundant with each other 
    (TEXT_PRIMARY is one colour across most phases, a role's HOT and ACTIVE cells usually 
    sit close together, an inert cell is usually its neighbour role's base) and a theme 
    that hand-typed all 72 would restate the same relationships dozens of times with nothing
    to keep the restatements in sync -- one literal edited and its echoes elsewhere quietly
    drift off the ramp.

    So a theme authors SEVEN colours and SIX numbers, and gui_style_bake derives the 
    full 36 cells based on those 13 values.

        seeds  -- the source colours, one per surface KIND (not per role, not per phase)
        ramp   -- how far a cell travels per interaction step, per theme
    
    A seed is a colour a designer picks; a ramp is the personality of the theme (how much a
    hover moves, how deep a press sits, how far an inert thing fades, how far a chosen surface
    washes toward the accent).  Neither is a cell.

    Baking WRITES col[][], and a kit is free to overwrite any cell afterwards.  Nothing is closed
    off: bake first for a coherent ramp, then hand-author the two or three cells you actually
    want bespoke.

        gui_style_t* e = gui()->style_edit();
        e->palette.seed[ GUI_SEED_ACCENT ] = gold;
        gui()->style_bake( e );                         // 36 cells re-derive

        e->col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE ] = ember;   // one bespoke cell
    
    Alpha rides through: a seed's alpha byte is carried onto every cell derived from it, so a
    translucent panel seed yields a translucent panel in all four phases without four literals.
==============================================================================================*/

/* The source colours.  One per surface KIND, which is a coarser axis than the role -- PANEL and
   TITLE are both the container surface, so both derive from SURFACE and the ramp separates them.
   Seven seeds cover nine roles because TITLE has no colour of its own: a caption band is a
   lifted surface, which is a derivation, not a decision.  The severity hues used to live here too
   (a severity ladder is a set of independent editorial choices no derivation can guess from an
   accent) but they were roles wearing a ramp they never used -- see GUI_ROLE_COUNT above and
   gui_style_ext_t below, where they live now as flat, unramped colours instead. */

typedef enum
{
    GUI_SEED_SURFACE = 0,   // container base: window body, panel, and the band over it
    GUI_SEED_CONTROL,       // control face base: button, input field, check box, track
    GUI_SEED_INK,           // text base -- every glyph and the caret
    GUI_SEED_LINE,          // frame line base: borders, rules, resize edges
    GUI_SEED_ACCENT,        // THE hue: value fills, hover wash, focus ring, nav highlight
    GUI_SEED_MARK,          // the affirmative indicator hue: check, radio dot
    GUI_SEED_GRAB,          // the contrast anchor: knobs and thumbs, opposite the theme

    GUI_SEED_COUNT

} gui_style_seed_t;

/* HOW FAR a derived cell travels -- the theme's personality, in six numbers, each 0..1, and
   the index into gui_palette_t.ramp.  Authored per theme rather than fixed, because a step that
   reads as one notch on a near-black surface reads as four on a near-white one: the light and
   dark built-ins carry visibly different recess values for exactly that reason.  A ramp of all
   zeroes bakes a flat, unreactive UI -- a legitimate look, and a useful debugging one.

   An array rather than named fields, for the same reason gui_style_t.var is one: the enum IS
   the field list, so a style editor walks the ramp with no table of its own. */

typedef enum
{
    GUI_RAMP_HOVER = 0,   // how far a surface washes toward the accent when hot
    GUI_RAMP_PRESS,       // how far it washes when pressed / selected -- deeper than hover
    GUI_RAMP_FADE,        // how far an inert cell fades toward the surface (the INERT phase)
    GUI_RAMP_RECESS,      // how far a recessed surface / empty track sinks below its base
    GUI_RAMP_STEP,        // one lift notch for the accent, border and anchor ramps
    GUI_RAMP_SELECT,      // how far a CHOSEN surface washes toward the accent (style_wash_selected)
    GUI_RAMP_COUNT

} gui_style_ramp_t;

/* GUI_STYLE -- the EXTENDED palette: flat, unramped colours a theme authors and a caller reaches
   by name, for a signal that is a standing fact rather than an interaction state -- no phase, no
   bake, no per-set derivation cost.  The reserved slots below are the severity ladder that used
   to live in GUI_ROLE_INFO/OK/WARN/ERROR; a kit registers its own beyond them at runtime with
   gui_style_ext_add (style/gui_style.h), mirroring gui_style_brush_add's per-set pool exactly --
   a handle only means something inside the set that issued it, and costs that set alone.

   style_ext( id ) is the resolved read; push_style_ext / pop_style_ext override a slot for a
   scope exactly like push_style_var does, since there is no ramp to re-derive on push -- a flat
   value swap is the whole operation. */

typedef enum
{
    GUI_EXT_INFO = 0,   // status hue: a neutral notice
    GUI_EXT_OK,         // status hue: healthy / passing
    GUI_EXT_WARN,       // status hue: near a limit
    GUI_EXT_ERROR,      // status hue: failed / over a limit

    GUI_EXT_RESERVED_COUNT   // the engine-authored slots -- part of every theme's palette

} gui_style_ext_t;

/* Slots one style set can hold, reserved included.  Small on purpose, like the brush pool: a
   kit needing more named colours than this is describing per-widget tokens, not a theme. */
#define GUI_STYLE_EXT_MAX 16u

/* The authored half of a style, in full: seven colours, six numbers and four status colours, 68
   bytes.  Small enough that a theme is worth having dozens of, or deriving live from a single
   accent the user picked. */

typedef struct gui_palette_s
{
    u32 seed[ GUI_SEED_COUNT ];             // the source colours
    f32 ramp[ GUI_RAMP_COUNT ];             // how far each derivation travels
    u32 ext [ GUI_EXT_RESERVED_COUNT ];     // the standard extended-palette colours (severity)

} gui_palette_t;

/*==============================================================================================
    GUI_STYLE -- global style configuration
==============================================================================================*/

/* The scale ramp -- the named density steps the UI is authored in, instead of raw pixel sizes.
   Each step is a complete metric set (row height + pad + gap, authored in gui_style_t.scales),
   so "this region is a dense list" is one declaration and the pairing that makes a density look
   right lives in the theme.  scale_push/scale_pop scope a step over the style-var stack, which
   makes every metric read (WIDGET_H / pad / gap) and every counting helper (rows_h, calc_row)
   speak that step with no widget changes.  STD is authored identical to the base metrics, so
   an unpushed UI is unchanged. */

typedef enum
{
    GUI_SCALE_DENSE,   // text lists: outliners, entity browsers, tree views, table rows
    GUI_SCALE_STD,     // the everyday widget row: forms, buttons, sliders, inputs
    GUI_SCALE_ROOMY,   // menus, combo dropdown lists, title-height rows
    GUI_SCALE_BAR,     // tab bars, icon toolbars, panel headers
    GUI_SCALE_COUNT

} gui_scale_t;

/* One ramp step's metrics.  Authored in px at em=12 like every other style metric; em-scaled,
   grid-quantized, and font-floored (a row always holds a text line) by gui_style_apply.  The
   three fields mirror GUI_VAR_ROW / _PAD / _GAP, which is what scale_push pushes them onto. */
typedef struct gui_scale_metrics_t
{
    f32 row;   // row height (the step's WIDGET_H)
    f32 pad;   // frame / content padding
    f32 gap;   // gap between consecutive widgets

} gui_scale_metrics_t;

/*==============================================================================================
    GUI_STYLE -- style vars

    The tunable scalars, the ImGuiStyleVar_ analogue, and the index into gui_style_t.var.  Each
    names one number the layout or the widgets read; push_style_var( var, value ) overrides it
    until the matching pop_style_var, next_style_var for just the next widget, and an unpushed
    var uses the style's installed value.  Values are f32 pixels (the shape picks carry a small
    integer in the same f32 slot -- one storage rule, no special case).

    Ordered by MEANING, in the two gui_style_t categories: METRICS can move rects (scale_push
    rides on the first three), SKIN only changes how paint lands inside rects composition already
    fixed.  How each var is TREATED -- scaled, snapped, left alone -- is a separate question, and
    it is answered per var by gui_style_class_t below rather than by position here.  Order is
    therefore free: a var may be inserted wherever it reads best.

    Every scalar the style has is here: if a number is worth having, it is worth being
    overridable through push_style_var, and if it is not worth overriding, it is not worth being
    a style field at all -- there is no second category of struct field sitting outside the var
    array.

==============================================================================================*/

typedef enum
{
    /* 1. METRICS -- can move a rect (scale_push/scale_pop override the first three) */

    GUI_VAR_ROW = 0,        // widget row height (the frame height)
    GUI_VAR_PAD,            // interior padding: region inset, label inset, natural-width pad
    GUI_VAR_GAP,            // space between consecutive widgets / cells

    GUI_VAR_BORDER,         // frame line width -- consumes space: child heights, bar tracks, resize zones
    GUI_VAR_INDICATOR,      // square indicator side (checkbox / radio) -- feeds the natural width
    GUI_VAR_GUTTER,         // slider knob width AND the scrollbar gutter thickness
    GUI_VAR_MIN_CELL,       // floor a flex/fraction track shrinks to before overflow
    GUI_VAR_TITLE_H,        // window title bar height -- the body starts below it
    GUI_VAR_GRID_Q,         // px lattice the metrics above snap onto (0/1 = off)

    /* 2. SKIN -- paint-only: radii, then the shape picks (an enum in the same f32 slot) */

    GUI_VAR_ROUND,          // corner radius: control frames, slider knobs, scrollbar grabs
    GUI_VAR_PANEL_ROUND,    // corner radius: windows / children / popups; 0 = square
    GUI_VAR_CHECK_SHAPE,    // checkbox/menu indicator: 0 = 'v' tick, 1 = disc, 2 = 'X' (gui_check_style_t)
    GUI_VAR_BULLET_SHAPE,   // bullet glyph: 0 = disc, 1 = square (gui_bullet_style_t)
    GUI_VAR_ARROW_SHAPE,    // directional arrow: 0 = triangle, 1 = chevron (gui_arrow_style_t)
    GUI_VAR_SEPARATOR_SHAPE,// separator rule: 0 = solid, 1 = dashed (gui_separator_style_t)
    GUI_VAR_PROGRESS_SHAPE, // progress_bar fill: 0 = solid, 1 = gradient (gui_progress_style_t)
    GUI_VAR_KNOB_SHAPE,     // slider knob: 0 = bar, 1 = circle (gui_slider_knob_t)
    GUI_VAR_MENU_CHECK,     // menu check gutter: 0 = plain, 1 = bordered box (gui_menu_check_t)

    /* 3. RATIO -- unitless 0..1 fractions.  Neither scaled nor snapped: a fraction has no
       pixels in it to scale and no lattice to land on. */

    GUI_VAR_DISABLED_ALPHA, // opacity a disabled item draws at (1 = no dim at all)

    /* 4. RATE -- how fast an item travels between cells, in Hz-like damper speed (10 ~ 250 ms to
       95%, 20 ~ 150 ms).  These are the whole motion budget of the widget set: every surface,
       border and ink that animates reads its speed from one of the three, so a theme sets the
       FEEL of the entire UI in three numbers -- and setting them to 0 makes the whole library
       snap, which is the accessibility answer and the "I hate animation" answer at once. */

    GUI_VAR_ANIM_HOT,       // rate the hover / nav highlight fades in and out
    GUI_VAR_ANIM_ACTIVE,    // rate the pressed state fades -- faster: a press must feel immediate
    GUI_VAR_ANIM_SELECT,    // rate a selection / toggle crosses to the SELECT plane
    GUI_VAR_ANIM_SIZE,      // rate a MEASURED extent eases to a new size (natural track, box height)

    GUI_VAR_COUNT,          // var count -- not a var

} gui_style_var_t;

/*==============================================================================================    
    Style Class 

    What KIND of number a var holds -- the mechanical half of the schema, declared once per 
    var beside its display name (style/gui_theme.c) and read back through gui()->style_var_class.
    Two questions, and every var answers both by naming a class:

    class    em-scaled?   lattice-snapped?   what it is
    -------  -----------  -----------------  --------------------------------------------
    METRIC   yes          yes                a size that positions a rect; seams must align
    STROKE   yes          no                 a line width -- snapping a hairline quadruples it
    SKIN     yes          no                 a paint-only radius, same reason
    PITCH    no           n/a                the lattice quantum itself, in raw pixels
    RATIO    no           n/a                a unitless 0..1 fraction -- no pixels to scale
    RATE     no           n/a                an animation speed in Hz -- a duration, not a size
    SHAPE    no           n/a                an enum pick carried in the f32 slot
    
    RATIO exists because every other non-pick class is em-SCALED, and scaling a fraction is
    simply wrong: a disabled item at 0.5 opacity would become 0.9 at a large font.  It is not
    SHAPE either, despite sharing "unscaled" -- a shape is a pick a tool offers as a combo over
    named values, a ratio is a number it offers as a 0..1 slider, and that difference is the
    whole reason an editor asks for the class.

    RATE is unscaled for the same reason and a different one: a transition that took 150 ms at a
    small font must still take 150 ms at a large one, because the eye is not typographic.  It is
    its own class rather than a RATIO because it is not bounded by 1 -- an editor offers it as a
    Hz slider running well past it, and 0 means "instant", not "invisible".

    Declaring the class at the same site as the name -- rather than inferring it from where a var
    falls in the enum -- turns a missing classification into a missing table entry a compiler-
    checked table can catch, instead of a metric that silently never scales because it happens to
    sit past some ordering marker.

==============================================================================================*/

typedef enum
{
    GUI_CLASS_METRIC = 0,
    GUI_CLASS_STROKE,
    GUI_CLASS_SKIN,
    GUI_CLASS_PITCH,
    GUI_CLASS_RATIO,
    GUI_CLASS_RATE,
    GUI_CLASS_SHAPE,
    GUI_CLASS_COUNT

} gui_style_class_t;

/* Checkbox / menu-item indicator shape (GUI_VAR_CHECK_SHAPE).  Default is the tick. */
typedef enum
{
    GUI_CHECK_TICK  = 0,   // a two-stroke 'v' check mark
    GUI_CHECK_DISC  = 1,   // a filled disc inside the box
    GUI_CHECK_CROSS = 2,   // a two-diagonal 'X' cross

} gui_check_style_t;

/* Bullet glyph shape (GUI_VAR_BULLET_SHAPE).  Default is the disc (Dear ImGui's RenderBullet). */
typedef enum
{
    GUI_BULLET_DISC   = 0,   // a small filled circle
    GUI_BULLET_SQUARE = 1,   // a small filled square

} gui_bullet_style_t;

/* Directional arrow shape (GUI_VAR_ARROW_SHAPE).  Default is the solid triangle.  Threads through
   every arrow the chrome draws -- arrow_button, the collapse fold, the combo / submenu arrow, the
   dock overlay -- since they all route through draw_arrow, exactly as check / bullet do. */
typedef enum
{
    GUI_ARROW_FILLED  = 0,   // a filled triangle pointing the direction
    GUI_ARROW_CHEVRON = 1,   // a stroked '>' chevron (two strokes to an apex)

} gui_arrow_style_t;

/* Separator rule shape (GUI_VAR_SEPARATOR_SHAPE).  Default is the solid rule.  Honored by
   separator() and the leading / trailing rules of separator_text(). */
typedef enum
{
    GUI_SEPARATOR_SOLID  = 0,   // a continuous filled rule
    GUI_SEPARATOR_DASHED = 1,   // a dashed rule

} gui_separator_style_t;

/* progress_bar fill style (GUI_VAR_PROGRESS_SHAPE).  Default is the solid fill; the gradient
   variant glosses the fill from the foreground accent to a brighter tint (top to bottom). */
typedef enum
{
    GUI_PROGRESS_SOLID    = 0,   // a flat foreground-accent fill
    GUI_PROGRESS_GRADIENT = 1,   // a top-to-bottom gradient gloss

} gui_progress_style_t;

/* Slider / drag knob shape (GUI_VAR_KNOB_SHAPE).  Default is the bar grab; the circle variant
   draws a round handle (raise GUI_VAR_ROUND instead for a pill bar). */
typedef enum
{
    GUI_SLIDER_KNOB_BAR    = 0,   // a rectangular grab (corner-rounded)
    GUI_SLIDER_KNOB_CIRCLE = 1,   // a circular handle

} gui_slider_knob_t;

/* Menu item check gutter style (GUI_VAR_MENU_CHECK).  Default is the bordered box, which draws
   an idle checkbox frame in the gutter whether or not the item is selected; the plain variant
   renders no box and only the indicator symbol when selected. */
typedef enum
{
    GUI_MENU_CHECK_PLAIN = 0,   // indicator only when selected; no idle box
    GUI_MENU_CHECK_BOX   = 1,   // bordered box always; indicator when selected

} gui_menu_check_t;

/*==============================================================================================
    ONE struct, THREE runs, and every one of them instanced per style set.  They stay together
    because the machinery treats them all identically -- themes author them, style sources
    overwrite them, the style stacks override them, style_var / style_color resolve them, and
    gui_style_apply em-scales them.  ONE test sorts a var between the two categories the enum is
    grouped by: can a read of this field move a rect?

      1. METRICS -- the spacing/size vocabulary.  One set of numbers consumed at two moments:
         composition (the composer divides space into cells -- row heights, gaps, region insets,
         scrollbar gutters, title bars) and widget self-measurement (a widget computes the
         natural size it REQUESTS through cell_next_w, then seats its label / indicator
         inside the finished cell with the same pad).  Only the composer POSITIONS rects;
         widgets only measure and request -- that is the composition contract.
      2. SKIN -- paint-only: colors, corner roundings, mark shapes.  A read of these can only
         change pixels inside a rect composition already fixed; none ever sizes or moves a cell.

    Behavior (interact/) consumes neither category: it takes finished rects.  (Its one metric
    read is GUI_VAR_BORDER, because the resize hit zone straddles the border -- border is
    geometry.)

    The struct IS the storage: the installed layer is an array of it, one per style set, and the
    resolved working run is its u32 image (asserted field-for-slot in style/gui_style_core.c).
    So gui()->style_edit() hands a kit the struct itself, and a style read is one indexed load at
    an offset the compiler already knows. 
==============================================================================================*/

/* GUI_GRID_LATTICE -- compile-time master switch for grid_quantum snapping.  1 (default) keeps
   the feature; define 0 (e.g. -DGUI_GRID_LATTICE=0) to strip every snap to identity so the lattice
   arithmetic folds out and grid_quantum costs nothing.  Independent of a style's grid_quantum,
   which still disables snapping per-style at runtime when <= 1. */

#ifndef GUI_GRID_LATTICE
#define GUI_GRID_LATTICE 1
#endif

typedef struct gui_style_s
{
    /* SKIN: the AUTHORED colour -- seven seeds, a six-number ramp and four extended colours
       (gui_palette_t, above).  Writing here changes nothing on its own; gui_style_bake derives
       col[][][] below from the seeds (the extended colours are copied straight through -- there
       is nothing to derive).  First in the struct because it is first in the pipeline, and
       because push_style_seed addresses a seed by slot exactly as push_style_var addresses a
       var. */
    gui_palette_t palette;

    /* SKIN: the 9x4 color grid -- THE color vocabulary (gui_style_role_t x gui_style_phase_t,
       above), and the DERIVED half: gui_style_bake writes all 36 cells from the palette, then a
       kit may overwrite any of them.  GUI_COLOR packs R,G,B,A bytes; a cell is read with
       style_col( role, phase ).  There is no SELECTED plane here -- style_col_selected washes a
       resolved cell toward the accent live, rather than reading a second stored one; see the
       SELECTED section above. */
    u32 col[ GUI_ROLE_COUNT ][ GUI_PHASE_COUNT ];

    /* SKIN: the FACE plane -- the same 9x4 grid again, but a cell here holds a HANDLE, not a
       colour: a 1-based index into this set's brush pool (gui_style_brush_add), looked up by
       style_face to return the gui_brush_t it names.  col and face share the u32 slot type only
       because the push/pop stack below addresses the whole struct as one flat array of u32 slots
       (gui_style_core.c) -- nothing in the struct itself marks a slot as "colour" or "handle";
       that meaning comes from which grid you index and which accessor you call.

       0 (GUI_FACE_NONE) everywhere by default, which means "just use col" -- so a theme that
       authors no art behaves exactly as it did and pays one indexed load it already had the cache
       line for.

       This plane is why the brush exists.  A colour cell can only ever say "fill it with this";
       a face cell can say "fill it with this nine-slice", and because it is addressed by the SAME
       (role, phase) coordinate every render already resolves, a theme installing faces restyles
       every widget that paints through the grid -- stock, chrome, and a user's own -- without one
       of them being edited.  The handle (not the brush body) lives in the slot space so that
       push_style_face / next_style_face / a set switch are the SAME machinery a colour push uses,
       with nothing new to keep in step; the bodies live beside the store, since a brush is
       registered once and named many times. */
    u32 face[ GUI_ROLE_COUNT ][ GUI_PHASE_COUNT ];

    /* SKIN: the EXTENDED palette -- flat colours addressed by gui_style_ext_t, no phase, no
       derivation.  Slots [0, GUI_EXT_RESERVED_COUNT) are copied straight from palette.ext at
       bake time; the rest start at 0 and are claimed at runtime by gui_style_ext_add, one style
       set's registered colours living beside its col/face grid the same way its brushes do. */
    u32 ext[ GUI_STYLE_EXT_MAX ];

    /* METRICS + SKIN scalars, indexed by gui_style_var_t -- the push_style_var vocabulary.
       Authored in px at em=12 and rescaled by gui_style_apply; the enum below documents each
       slot.  An array rather than named fields because the enum IS the field list: one order,
       one place to add a var, and a style editor can walk it. */
    f32 var[ GUI_VAR_COUNT ];

    /* The density ramp (see gui_scale_t) -- METRICS per step, what scale_push pushes onto
       GUI_VAR_ROW / _PAD / _GAP.  STD is authored to mirror those three, so an unpushed UI is
       unchanged.  Instanced with everything else, so a kit's DENSE is its own. */
    gui_scale_metrics_t scales[ GUI_SCALE_COUNT ];

} gui_style_t;

/* gui_style_get() -- returns a pointer to the mutable base style (s_style_base).  Edits take
   effect on the next gui_style_apply() / gui_theme_reset() call.  Mutating the struct directly
   without calling theme_reset marks the theme as anonymous (theme_get returns NULL). */

gui_style_t* gui_style_get( void );

/* gui_style_peek() -- read-only view of the base style.  Unlike gui_style_get() this does NOT
   mark the theme anonymous, so a style editor can populate its widgets (and keep labeling the
   active theme) and only call gui_style_get() on the frame it actually commits an edit. */

const gui_style_t* gui_style_peek( void );

/* gui_style_apply() -- recomputes the scaled active metrics from the current base style.
   Called automatically on font change; call manually after editing via gui_style_get(). */

void         gui_style_apply( void );

/* gui_dpi_land() -- activate the DPI bake a viewport carries and rescale the live metrics to
   it.  Internal seam: window_begin calls it as each window's surface comes up, so surfaces on
   differently-scaled monitors emit with their own bake in one sequential frame.  A no-op when
   that bake is already landed, when DPI is unmanaged, or while a host-driven font is active. */

void         gui_dpi_land( i32 viewport );

/* gui_style_bake() -- derive the 96-cell colour grid from s->palette, in place.  The one step
   between what a theme AUTHORS and what a render READS, and the only writer of col[][][] the
   engine has.  Pure: a function of the palette alone, so the same palette always bakes to the
   same grid, and it never touches var / scales.

   Explicit rather than automatic on purpose.  A kit writes seeds, bakes, then overwrites the
   few cells it wants bespoke -- an implicit bake would have to run either before those writes
   (no effect) or after them (silently discarded).  Naming the step makes the order the caller's,
   and makes "I hand-authored this cell" survivable.  The built-in themes bake at theme_set. */

void         gui_style_bake( gui_style_t* s );

/*==============================================================================================
    GUI_CHROME -- themes (chrome's named style presets)

    A theme is a named gui_style_t snapshot: a human-readable name paired with a seed palette,
    the layout metrics, and the density ramp.  Its col[][][] is left EMPTY -- theme_set bakes the
    grid from the palette on the way in, so a built-in theme is authored as eleven colours and a
    ramp rather than as 96 literals.  The active theme is the root layer every push_style_color /
    push_style_var / push_style_seed overrides relative to.  Switching or resetting a theme clears
    the push stacks immediately -- use this instead of managing deep push/pop sequences for large
    style changes.

        u32  n;
        const gui_theme_t* list = gui_theme_list( &n );  // enumerate built-ins
        for ( u32 i = 0; i < n; ++i ) puts( list[i].name );

        gui_theme_set( "light" );   // switch theme + clear style stacks
        gui_theme_reset();          // revert any style_get edits, clear stacks
==============================================================================================*/

typedef struct gui_theme_t
{
    const char* name;    // human-readable key used by theme_set / theme_get
    gui_style_t style;   // palette + metrics; .col is baked from .palette by theme_set

} gui_theme_t;

const gui_theme_t* gui_theme_list ( u32* count_out );    /* enumerate built-in themes            */
bool               gui_theme_set  ( const char* name );  /* switch to named theme + reset stacks */
const char*        gui_theme_get  ( void );              /* active theme name, NULL if anonymous */
void               gui_theme_reset( void );              /* restore base + clear push stacks     */

/*==============================================================================================
    GUI_FLOW -- THE OVERLOADED UNIT: one sizing rule, every track

    The single most-referenced rule in the system, and the reason there is no second sizing
    vocabulary: ONE f32 says fixed / fill / fraction / natural, and every place that divides
    space reads it the same way -- the flow templates (row / cols / grid / pack), the one-shot
    overrides (next_item_fit / next_item_h / pack_size), the field split, and the stateless
    carve math (split / carve).  Both axes, every consumer:

        > 1.0         fixed pixels -- explicit px is authored intent and is never floored
        == 1.0        fill -- an equal share of the leftover (several fills split it evenly)
        (0.0, 1.0)    fraction of the gap-adjusted available extent
        == 0.0        natural -- the item's own content size.  Pack resolves it per item with
                      content in hand.  A pre-divided COLUMN track (cols / grid columns) has no
                      content at resolve time, so it resolves against MEASURED FEEDBACK: the
                      widest natural item placed in that column last frame (one-frame lag, the
                      same feedback autosize windows run on; floored at the minimum cell width
                      until something measures).  The label-sized form column is just
                      cols( (f32[]){ 0, 1.0f, GUI_END } ).  Grid ROW tracks and the pure-math
                      callers (split / carve / field_split) have no measure to fall back on and
                      collapse to zero -- use fill / fraction / px there.
        <  0.0        GUI_END, the track-list terminator

    Flex and fraction tracks floor at GUI_VAR_MIN_CELL and the row overflows into the clip;
    fixed px never floors.  Gaps sit *between* cells and are subtracted before distribution, so
    a widget never sees or reasons about spacing -- it just fills the rect it is handed.

    Scope: this rule sizes TRACKS only.  Pure spacing calls (same_line, new_line) look similar
    but are not -- a gap has no content, so their 0 is a literal zero rather than a content
    measure; see their own docs.

    Which METHODOLOGY divides the space is a separate axis, named by the region's layout header
    -- see gui_layout_mode_t.

==============================================================================================*/

#define GUI_LAYOUT_COLS 8                     // max tracks on one axis (columns or rows)
#define GUI_END (-1.0f)                       // track-list terminator (any negative value)

/* carve markup sentinels -- nest a gui()->carve form (a single GUI_END-terminated f32 list, the same
   overloaded unit as cols).  A size FOLLOWED by a CUT is a container of that size, subdivided on the
   named axis until a matching GUI_END; a size followed by anything else is a leaf.  A form opens with
   a leading CUT that fills the whole area.  See gui()->carve. */

#define GUI_CUT_X (-2.0f)                     // open a nested column split (panels side by side)
#define GUI_CUT_Y (-3.0f)                     // open a nested row split (panels stacked)

/*==============================================================================================
    GUI_CORE -- item interaction state

    One frame of interaction for one item -- the result of the shared widget interaction state
    machine, whether run internally for a stock widget or over a caller rect via gui()->item().
    A user widget takes a rect (canvas cut, split/carve, own math), asks for behavior, and draws
    its own presentation from these flags, exactly as the stock widgets do internally.
    invisible_button( id, r ) is this reduced to its click bit.
==============================================================================================*/

typedef struct gui_item_state_t
{
    bool hover;      // cursor is over the rect this frame
    bool active;     // primary button held with this item captured (dragging / holding)
    bool pressed;    // primary button went down on the item this frame
    bool clicked;    // press + release completed with the cursor still over ("fired")
    bool focused;    // item owns keyboard input (focusable items: text / value fields)
    bool nav;        // item is the keyboard-nav cursor while the keyboard is active (fill state)
    i32  nav_adjust; // keyboard value edit: -1 / +1 arrow step this frame (captured drag items)

} gui_item_state_t;

/*==============================================================================================
    Multi-select protocol -- the frame's resolved selection action (interact/gui_msel.c).

    The engine never stores the selection; the caller does (bool array, bitset, component
    flag).  Each frame the msel scope (msel_begin .. row feeds .. msel_end) resolves clicks,
    modifiers, and keyboard extension against a persistent range anchor into ONE index-range
    action the caller applies -- msel_apply is the ready-made application for a dense bool
    array.  Index math, so an action spans rows a virtualized list never emitted.
==============================================================================================*/

typedef enum
{
    GUI_MSEL_NONE = 0,   // no selection change this frame
    GUI_MSEL_SET,        // clear everything, then select [lo..hi]      (plain / shift click)
    GUI_MSEL_ADD,        // select [lo..hi], keep the rest              (ctrl+shift click)
    GUI_MSEL_TOGGLE,     // invert [lo..hi] (single row: lo == hi)      (ctrl click)
    GUI_MSEL_ALL,        // select the whole list                       (ctrl+A in the scope)
    GUI_MSEL_CLEAR,      // clear the whole list (caller vocabulary: empty-space click, Escape)

} gui_msel_op_t;

typedef struct gui_msel_t
{
    gui_msel_op_t op;    // what to do to the caller's selection storage
    i32           lo;    // first affected row (inclusive) for SET / ADD / TOGGLE
    i32           hi;    // last affected row (inclusive)

} gui_msel_t;

/*==============================================================================================
    GUI_COMPONENT -- widget LOGIC building blocks (staging)

    A component consumes an (id, rect) + config and does the interaction math -- hit, drag,
    snap, keyboard nav -- with NO paint, reporting the geometry a caller renders however it
    likes.  The reference render (stock_*) and a user's own widget are SIBLINGS over the same
    component: same logic, different presentation.  Design is iterative; the slider is first.
==============================================================================================*/

/* Input to gui()->comp_slider -- the shape, range, and feel of a value slider.  Bundled into
   one struct because the call is parameter-rich: fill the fields that matter, leave the rest
   zero (every zero has a documented "auto" meaning).  A relative-drag field (value by cursor
   displacement, no track) is the model's other pivot -- not this call. */
typedef struct gui_comp_slider_desc_t
{
    const char* id_str;    // interaction identity (label-hashed)
    gui_rect_t  rect;      // the region the handle travels within (the groove)
    f32*        v;         // value, read and written in place (clamped + snapped)
    f32         lo, hi;    // value range; lo < hi
    f32         step;      // snap quantum in value units; <= 0 = continuous
    f32         handle_w;  // handle extent along the track, px; <= 0 = a default width
    f32         nav_step;  // keyboard arrow step, value units; <= 0 = auto (step, else 5% of range)

} gui_comp_slider_desc_t;

/* Result of gui()->comp_slider -- the interaction state plus the two rects a render needs (the
   value BAR and the HANDLE) and the resolved fraction.  The full track is the caller's input
   rect; the handle CENTER tracks the cursor, so value and knob never disagree. */
typedef struct gui_comp_slider_t
{
    gui_item_state_t state;    // raw interaction (hover/active/pressed/focused/nav/nav_adjust)
    f32              frac;     // 0..1 handle position after clamp + snap
    gui_rect_t       fill;     // the value bar: track start up to the handle center
    gui_rect_t       handle;   // the handle (thumb) at frac
    bool             changed;  // *v changed this frame

} gui_comp_slider_t;

/* THE SHARED COMPONENT SHAPE -- every gui_comp_*_t below follows it, in this order:

       gui_item_state_t state;   // ALWAYS first: the raw interaction, for picking a face
       ...geometry...            // the rects a render paints (absent when the widget has none)
       ...outcome...             // what the widget MEANS this frame -- and only what `state`
                                 //   does not already say (changed / enter, never a second
                                 //   spelling of state.clicked)

   So `state` is at offset 0 for every component, and a render reads it the same way regardless
   of which component produced it: gui()->item_phase( x.state ) -> a palette state.

   Result of gui()->comp_button -- the simplest case, and the one that settled the shape: no
   geometry beyond the caller's rect, and its outcome IS state.clicked, so it adds no field. */
typedef struct gui_comp_button_t
{
    gui_item_state_t state;    // raw interaction; .clicked is the button's outcome

} gui_comp_button_t;

/* Result of gui()->comp_check -- the toggle's interaction, the inscribed square box (the hit AND
   where the render draws the frame + mark), and whether *v flipped this frame. */
typedef struct gui_comp_check_t
{
    gui_item_state_t state;    // interaction over the box
    gui_rect_t       box;      // inscribed square: the hit and the paint target
    bool             changed;  // *v toggled this frame

} gui_comp_check_t;

/* Result of gui()->comp_cycle -- a "< value >" stepper.  The two cap buttons are each a COMPOSED
   comp_button (so they hover / nav / redraw like any button), with their rects and the center
   region for the value text.  The component takes count (for wrap) but not the item strings --
   those are the render's to draw at label.  `state` (the shared shape's first field) is the
   WHOLE widget's interaction -- hover over either cap or the value band -- so item_phase works
   on a cycle exactly as on any other component; read .prev / .next for the per-cap faces. */
typedef struct gui_comp_cycle_t
{
    gui_item_state_t  state;     // interaction over the whole stepper (either cap or the band)
    gui_comp_button_t prev;      // left cap (decrement)
    gui_comp_button_t next;      // right cap (increment)
    gui_rect_t        prev_box;  // left cap rect
    gui_rect_t        next_box;  // right cap rect
    gui_rect_t        label;     // center region for items[*idx]
    bool              changed;   // *idx changed this frame

} gui_comp_cycle_t;

/* Result of gui()->comp_selectable -- a list-row press.  The button's shape (it composes
   comp_button) plus the *selected toggle already applied; the render reads the caller's selected
   flag for its active tint and state.clicked to drive its own selection. */
typedef struct gui_comp_selectable_t
{
    gui_item_state_t state;    // interaction over the row; .clicked is the row's outcome

} gui_comp_selectable_t;

/* Result of gui()->comp_input -- a single-line text field.  The component runs the shared edit
   engine and hands back PAINTABLE geometry so a render never touches the edit state or measures
   text: the content rect (push_clip it), the run's draw origin, and the selection / caret bars
   (each w == 0 when absent).  changed / enter are the outcomes. */
typedef struct gui_comp_input_t
{
    gui_item_state_t state;      // interaction (press claims keyboard focus)
    gui_rect_t       content;    // text content rect (rect inset by pad); the run's clip band
    f32              text_x;     // x to draw buf[0] at (already scroll-offset)
    f32              text_y;     // baseline y for the run (vertically centered)
    gui_rect_t       selection;  // selection band to fill; w == 0 = none / unfocused
    gui_rect_t       caret;      // caret bar to fill; w == 0 = hidden this blink phase
    bool             changed;    // buffer changed this frame
    bool             enter;      // Enter / submit this frame

} gui_comp_input_t;

/*==============================================================================================
    GUI_RECT -- anchor frame: the general placement (UE4 Slate model): a normalized sub-rect of the parent
    (0..1 per axis) plus pixel offsets, resolved per axis by gui()->anchor.  On an axis where
    min == max the child is point-anchored: the anchor is a single line at that fraction, the child
    takes `size` and is hung off it by `pivot` (0 = near edge sits on the line, 0.5 = centered, 1 =
    far edge), shifted by the offset.  On an axis where min < max the child is stretch-anchored: its
    edges track those two parent fractions and the offsets become per-edge insets (size is ignored).
    This unifies "pin a badge 40% across" and "stretch a bar over the top with 8px margins".
==============================================================================================*/

typedef struct
{
    gui_vec2_t  min;     // normalized 0..1: anchor's near edge as a fraction of the parent
    gui_vec2_t  max;     // normalized 0..1: anchor's far edge ( == min for a point anchor )
    gui_vec2_t  pivot;   // point-anchor only: which point of the child sits on the line ( 0.5 = center )
    gui_vec2_t  size;    // point-anchor only: child w / h in px
    gui_pad_t   off;     // point: l / t shift the pivot; stretch: l / t / r / b inset the tracked edges

} gui_anchor_t;

/*==============================================================================================
    GUI_FLOW -- grid descriptor: the full-control form of gui()->grid.

    Both track lists are GUI_END-terminated and sized with THE OVERLOADED UNIT (the GUI_FLOW
    banner above); together they partition the band from the pen to the region's bottom into a
    fixed matrix that widgets fill row-major.  gui()->grid_cells( nc, nr ) is the uniform case
    and needs none of this.  Built as a compound literal at the call site:

        gui()->grid( ( gui_grid_t ){ .cols  = { 120.0f, 1.0f, GUI_END },
                                     .rows  = { 1.0f, 1.0f, GUI_END },
                                     .align = GUI_ALIGN_CENTER } );
==============================================================================================*/

typedef struct gui_grid_t
{
    f32         cols[ GUI_LAYOUT_COLS ];   // column tracks, GUI_END-terminated
    f32         rows[ GUI_LAYOUT_COLS ];   // row tracks, GUI_END-terminated
    f32         gap_x, gap_y;              // inter-cell spacing; 0 = the style's GUI_VAR_GAP
    gui_align_t align;                     // content alignment within each cell (0 = LEFT | TOP)

} gui_grid_t;

/*==============================================================================================
    GUI_FLOW -- layout mode: the next-item methodology a region is laying out under.

    A region opens UNDECLARED (NONE): the first layout header names the mode (stack / columns /
    grid / ...), and a widget emitted before any header is a usage error (debug assert; a release
    build falls back to STACK rather than faulting) -- the mode is always explicit at the top of a
    region body, never an implicit default.  The mode is the "next item methodology"; the per-cell
    sizing inside it is still the one overloaded unit rule.
==============================================================================================*/

typedef enum
{
    GUI_MODE_NONE = 0,    /* no header declared yet -- emitting a widget here is a usage error */
    GUI_MODE_STACK,       /* single flex column, rows accumulate + scroll (the vertical list)  */
    GUI_MODE_COLUMNS,     /* N pre-divided column tracks, rows accumulate + scroll             */
    GUI_MODE_GRID,        /* bounded cols x rows matrix, both axes fixed, nothing scrolls      */
    GUI_MODE_PACK,        /* natural-size print run, placed item-by-item along an axis (bar/strip) */

} gui_layout_mode_t;

/*==============================================================================================
    GUI_FLOW -- pack direction: the axis a pack() run places items along, item-by-item at natural size.
    bar() is the horizontal pack (a toolbar); strip() is the vertical pack.
==============================================================================================*/

typedef enum
{
    GUI_PACK_HORIZONTAL = 0,    /* bar:   items flow left to right, nextline wraps down    */
    GUI_PACK_VERTICAL   = 1,    /* strip: items flow top to bottom, nextline wraps across  */

} gui_pack_dir_t;

/*==============================================================================================
    GUI_RECT -- split axis: the axis gui()->split carves a rect along.  X lays the panels left-to-right
    (a column split: a sidebar + content); Y lays them top-to-bottom (a row split: header /
    body / footer).  The panel sizes use the same overloaded unit as the column tracks.
==============================================================================================*/

typedef enum
{
    GUI_AXIS_X = 0,    /* carve into vertical panels side by side (columns)  */
    GUI_AXIS_Y = 1,    /* carve into horizontal panels stacked (rows)        */

} gui_axis_t;

/*==============================================================================================
    GUI_FLOW -- field label side: where a labeled value widget (input_text / slider_float / checkbox) puts
    its label when a field split is active (gui()->field_split / field_label_left, or the ambient
    gui_field_t).  The label and control are two tracks resolved across the widget's cell with the
    same overloaded unit as columns; `side` only decides which track sits on which edge.  NONE (the
    default) trails the label at its natural width on the right; the field lives in the ambient
    gui_field_t and persists like a style until changed.
==============================================================================================*/

typedef enum
{
    GUI_LABEL_NONE  = 0,      /* off -- natural-width label trailing on the right (default) */
    GUI_LABEL_LEFT  = 1,      /* label track on the left, control fills the right */
    GUI_LABEL_RIGHT = 2,      /* label track on the right, control fills the left */

} gui_label_side_t;

/*==============================================================================================
    GUI_FLOW -- gui_field_t: the ambient label ("pair") layout, the shared authority every
    labeled widget reads.  A labeled widget is a bare control plus a label composed as a pair;
    this struct owns the LABEL half so every paired row aligns the same way -- set it once
    (gui()->field_set) and all the _label variants line up.  It is deliberately kept small and
    apart from the color palette: this is layout-theme material (like the four spacing metrics),
    not skin, so a kit can swap label geometry without touching a single color.

    Zero-initialized => the built-in default: labels shown, trailing at their own natural width
    on the right (GUI_LABEL_NONE), no forced track, left-aligned in their space.  `hide` is the
    master toggle for property panels -- flip it once and every _label row drops its label and
    the control spans the whole cell, with no per-call parameter riding along.

    label / control are two sizes in the same overloaded unit as columns (> 1 px, == 1 fill,
    (0,1) fraction, 0 natural), used only when side is LEFT / RIGHT: field_set{ .side=LEFT,
    .label=120 } is a 120px label column + a flex control; { .side=LEFT, .label=0.35f } a 35/65
    split.  In NONE (trailing) mode both are ignored and the label hugs the right at its width. */

typedef struct gui_field_t
{
    f32  label;      // label track size when side is LEFT/RIGHT (overloaded unit; 0 = natural width)
    f32  control;    // control track size (overloaded unit; 0 = fill the rest)
    u8   side;       // gui_label_side_t: 0 trailing (label hugs right), 1 left column, 2 right column
    u8   align;      // gui_align_t for the label text within its track (0 = LEFT | VCENTER)
    bool hide;       // master toggle: true skips every label, the control spans the whole cell

} gui_field_t;

/*==============================================================================================
    GUI_CORE -- item flags

    A push-model of per-item behavior tweaks, the ImGui ItemFlags analogue.  Instead of widening
    every widget signature with a new parameter, behavior is tuned through a flag set the widget
    reads at emit time, so a feature can be added without touching any call site.

    Two layers merge into the flags a widget sees:

      Stack    -- push_item_flag( flag, enable ) / pop_item_flag(): affects every widget until
                  popped (disable a run of buttons, mark a section read-only).  Nests; pop restores.
      Next     -- next_item_flag( flag, enable ): a one-shot override consumed by the very next
                  widget only, no pop needed.  Overrides the stack for that one item (it can force
                  a bit off even when the stack has it on).

    The merged value is resolved once per widget; a widget that does not care about a given flag
    simply ignores it, so unknown / future flags are inert by construction.  Bit values so several
    can be combined; 0 (GUI_ITEM_NONE) is the default no-op set.
==============================================================================================*/

typedef enum
{
    /* no tweaks -- the default behavior */
    GUI_ITEM_NONE          = 0,

    /* inert + dimmed: no hover/active/focus/click, drawn at
       reduced opacity.  Honored uniformly by item_state and
       the draw list, so it applies to every widget at once. */
    GUI_ITEM_DISABLED      = 1 << 0,

    /* a held button fires repeatedly: once on press, then after
       an initial delay at a steady rate (spinner / scroll arrows),
       instead of once on release.  Honored by item_state, so
       any button-kind widget under the flag auto-repeats. */
    GUI_ITEM_BUTTON_REPEAT = 1 << 1,

    /* slider_float: suppress the value text drawn centered on the
       track.  The value is shown by default; set this 
       (push or next_item_flag) to hide it for a bare / compact slider. */
    GUI_ITEM_NO_VALUE_TEXT = 1 << 2,

    /* selectable: do NOT close the enclosing popup when clicked.
       By default a selectable inside any popup calls popup_close_current()
       on click (Dear ImGui CloseCurrentPopup default).  Set this to opt out --
       e.g. a multi-select list inside a popup where the popup should stay open. */
    GUI_ITEM_NO_CLOSE_POPUP = 1 << 3,

    /* selectable (and any future list-y widget that opts in): do NOT register this item's label
       for keyboard type-ahead.  Type-ahead is on by default for every selectable -- typing a
       letter jumps the nav cursor to the first row whose label starts with it, the native
       listbox/combobox behavior -- set this to opt a row out (see gui_nav.c). */
    GUI_ITEM_NO_TYPEAHEAD   = 1 << 4,

    /* Room to grow without disturbing call sites or the vtable -- e.g. a future
    GUI_ITEM_READ_ONLY (editable widgets show but reject input), GUI_ITEM_NO_NAV, etc. */

} gui_item_flags_t;

/*==============================================================================================
    GUI_CORE -- drag and drop

    Item-to-item payload transfer (the ImGui BeginDragDropSource/Target analogue).  A widget
    becomes a drag SOURCE by calling drag_source_begin right after it emits; while the user
    drags from it, the source sets a typed payload (a small byte blob, copied) and emits preview
    widgets that follow the cursor.  Any other widget becomes a drop TARGET by calling
    drag_target_begin right after it emits; while a drag hovers it, drag_payload_accept matches
    the type tag, highlights the target, and returns the payload on the release frame.  One drag
    exists at a time (one mouse), so the payload store is a single ambient slot.  See gui_api.h
    for the usage contract.
==============================================================================================*/

#define GUI_DRAG_TYPE_CAP     16    // bytes of a payload type tag, including the NUL
#define GUI_DRAG_PAYLOAD_CAP  64    // payload bytes copied by drag_payload_set

typedef enum
{
    GUI_DRAG_NONE        = 0,

    /* drag_payload_accept: return the payload while the drag hovers the target (every frame),
       instead of only on the release (drop) frame -- for live preview effects at the target. */
    GUI_DRAG_ACCEPT_PEEK = 1 << 0,

    /* drag_source_begin: no cursor-following preview tooltip is opened (emit nothing between
       begin/end).  drag_payload_accept: skip the accept highlight around the target. */
    GUI_DRAG_NO_PREVIEW  = 1 << 1,

} gui_drag_flags_t;

/* The caller-facing payload view returned by drag_payload_accept / drag_payload_peek.  The bytes
   were copied at drag_payload_set time and stay valid until the drag ends (the release frame
   included) -- copy them out if they must outlive the drop. */
typedef struct
{
    const char* type;   // tag stamped by drag_payload_set
    const void* data;   // payload bytes (copied at set time)
    u32         size;   // payload byte count

} gui_drag_payload_t;

/*==============================================================================================
    GUI_DRAW -- angle algebra

    Angles -- the arc / pie / spinner / progress sweep parameters (draw_arc, draw_pie, ...) are
    radians in screen space (y down, so a positive angle turns clockwise; 0 points right / +x).
    Author in friendly degrees and convert at the call site:

    gui()->draw_arc( cx, cy, r, gui_radians( 0 ), gui_radians( 270 ), 3.0f, col );
    gui()->draw_pie( cx, cy, r, gui_radians( -90 ), gui_radians( 90 ), col );

    Stateless pure math, so these are inline here (no vtable entry) like the rect helpers above.
==============================================================================================*/

#define GUI_PI 3.14159265358979f

/* Degrees -> radians (the unit the arc / pie / sweep parameters take). */
static inline f32 gui_radians( f32 degrees ) { return degrees * ( GUI_PI / 180.0f ); }

/* Radians -> degrees (to read a stored sweep back in friendly units). */
static inline f32 gui_degrees( f32 radians ) { return radians * ( 180.0f / GUI_PI ); }

/*==============================================================================================
    GUI_DRAW -- color packing

    GUI_COLOR(r,g,b,a) packs 0-255 byte values into a u32 such that memory byte order
    is [R, G, B, A], matching VK_FORMAT_R8G8B8A8_UNORM vertex attribute layout.
==============================================================================================*/

#define GUI_COLOR( r, g, b, a ) \
    ( ( ( u32 )( a ) << 24 ) | ( ( u32 )( b ) << 16 ) | ( ( u32 )( g ) << 8 ) | ( u32 )( r ) )

/*==============================================================================================
    GUI_DRAW -- the BRUSH: what fills a rect

    If colour were the only thing a rect could be filled with, every richer fill would need to be
    a VERB of its own -- draw_gradient, draw_texture_in, draw_checker, draw_shadow -- and none of
    them would compose: a widget written against draw_fill could never be handed a gradient, and
    a window frame could never be handed authored art, without forking the code that paints it.

    A brush is a plain descriptor that widens "fill colour" into a value: a KIND plus the fields
    that kind reads.  No handle, no lifetime, no allocation -- it is passed as a compound literal
    exactly like every other desc in the library, stored in a theme, or held by a widget:

        gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = frame } );

    GUI_BRUSH_SOLID with col_a is EXACTLY draw_fill, so a brush costs nothing over a flat colour
    and only adds what else it can say.  A future effect brush (SDF rounding, glow, blur-behind)
    is one more kind, not one more verb, and every render that already speaks brushes picks it up
    for free.
==============================================================================================*/

typedef enum
{
    GUI_BRUSH_SOLID = 0,   // col_a floods the rect -- the draw_fill equivalent, and the zero value
    GUI_BRUSH_GRADIENT,    // col_a -> col_b across the rect (axis from GUI_BRUSH_VERTICAL)
    GUI_BRUSH_SPRITE,      // one sprite stretched over the rect, multiplied by col_a as a tint
    GUI_BRUSH_NINE,        // the sprite's nine-slice expanded over the rect (corners kept, per
                           //   sprite_set_slice; falls back to SPRITE when it carries no insets)

} gui_brush_kind_t;

typedef enum
{
    GUI_BRUSH_VERTICAL = 1 << 0,   // GRADIENT: ramp top->bottom instead of left->right
    GUI_BRUSH_TILE     = 1 << 1,   // NINE: repeat the edge / centre pieces at authored size
                                   //   rather than stretching them (a patterned border)
    GUI_BRUSH_FLIP_X   = 1 << 2,   // SPRITE / NINE: mirror horizontally
    GUI_BRUSH_FLIP_Y   = 1 << 3,   // SPRITE / NINE: mirror vertically

} gui_brush_flags_t;

/* A tint of 0 means UNTINTED (white), not "invisible": a sprite brush that had to spell out
   GUI_COLOR(255,255,255,255) to show its own colours would make the common case the loud one.
   Pass an explicit alpha in col_a to fade a sprite. */
/* A brush REGISTERED in a style set's pool, named by a face cell.  0 = no face (the cell falls
   back to its flat colour).  Registration is per style SET, so a kit's art and chrome's are
   separate pools and a handle only ever means something inside the set that issued it. */
typedef u32 gui_style_face_t;
#define GUI_FACE_NONE 0u

/* Brushes one style set can hold.  Small on purpose: a face pool is a THEME's art -- a frame, a
   button face, a track, a thumb, a header -- not an asset library.  A kit needing more art than
   this is describing sprites, and should hand them to widgets directly. */
#define GUI_STYLE_BRUSH_MAX 32u

typedef struct
{
    u8              kind;     // gui_brush_kind_t
    u8              _pad;
    u16             flags;    // gui_brush_flags_t
    u32             col_a;    // SOLID / GRADIENT: the (first) colour.  SPRITE / NINE: tint (0 = none)
    u32             col_b;    // GRADIENT: the far-edge colour
    gui_sprite_id_t sprite;   // SPRITE / NINE: the art
    f32             scale;    // SPRITE / NINE: px scale applied to slice insets and tile pitch,
                              //   so one sprite serves several UI scales (0 or 1 = authored size)

} gui_brush_t;

/*==============================================================================================
    GUI_DRAW -- line / path stroking

    Thickness, pixel-snapping, and where a stroke sits relative to the ideal path it is drawn from.
    Implementation in gui_emit_path.c.

    Pixel model: integer coordinates fall on the lines *between* pixels, so a crisp axis-aligned
    stroke is one whose two edges both land on integers.  draw_line strokes a single segment: a
    horizontal / vertical one snaps to the pixel grid and renders perfectly crisp (like a
    separator); any other angle is stroked with a 1px antialiased edge so diagonals stay smooth.
    draw_polyline / path_stroke connect several points with miter-limited corners (always
    antialiased) -- use them for multi-segment outlines, arrows, and diagonal runs.
==============================================================================================*/

/* Where the stroke sits across the ideal path (the line the coordinates describe).  CENTER runs
   the path down the middle of the stroke; INSIDE / OUTSIDE push the whole width onto one side (the
   left-hand normal of travel is the "inside").  CENTER_BIASED is CENTER plus a parity-aware snap so
   an odd-thickness axis-aligned line lands on whole pixels instead of straddling two -- the crisp
   default for UI rules and borders.  (The snap only bites on axis-aligned single segments; a
   diagonal or a multi-segment polyline treats CENTER_BIASED as CENTER and relies on antialiasing.) */
typedef enum
{
    GUI_STROKE_CENTER_BIASED = 0,   // centered + snapped to the pixel grid (default)
    GUI_STROKE_CENTER,              // centered on the path, no snap
    GUI_STROKE_INSIDE,              // whole width on the interior side of a CW-screen ring
    GUI_STROKE_OUTSIDE,             // whole width on the exterior side of a CW-screen ring

} gui_stroke_align_t;

#define GUI_PATH_MAX 256            /* max points path_line_to accumulates before a stroke */

/*==============================================================================================
    GUI_DRAW -- the effect band: a shape the FRAGMENT resolves

    Everything above this line is geometry the CPU tessellates outright.  Without a way for the
    fragment to resolve a shape itself, rounding a corner means walking an arc table and fanning
    triangles -- a rounded rect at ~37 vertices, with hard stair-stepped edges and no room for a
    texture -- and a soft shadow means six stacked rects pretending to be a gaussian.  Both are
    the same shortfall: an effect the rasterizer could evaluate exactly, approximated in vertices
    because the vertex has nowhere to say what shape it belongs to.

    The effect band is that missing sentence.  Every vertex carries a signed-distance coordinate
    and one packed word naming the shape, so a rounded box is FOUR quads whose fragment shader
    knows the exact boundary: analytic antialiasing, arbitrary softness, and the texture still
    sampling underneath it.  Crucially the mode travels PER VERTEX, not in a push constant, so an
    SDF surface, a glyph run and a plain fill share one draw call -- an effect can never split a
    batch, which is what makes it affordable to use one on every widget.

    Mode 0 (GUI_FX_NONE) is what every other primitive writes, and the fragment tests it first:
    text, lines, sprites and square fills pay one compare and are byte-for-byte unchanged.

    Vertex attribute layout (matches the gui pipeline), 28 bytes, single interleaved binding:
        location 0 : FLOAT2     (x, y)      offset  0   -- pixel-space position
        location 1 : UNORM16X2  (uv u32)    offset  8   -- texture UV, [0,1] at 1/65535
        location 2 : UNORM8X4   (abgr u32)  offset 12   -- packed color, R8G8B8A8_UNORM
        location 3 : HALF2      (fxc u32)   offset 16   -- effect coordinate (see below)
        location 4 : UINT       (fx u32)    offset 20   -- packed effect word (flat)
        location 5 : UINT       (tex u32)   offset 24   -- sampling model + bindless slot (flat)

    FOUR of those six are packed, and the shader is unaware of it: the fetch unit widens every
    normalized and half format to float, so the vertex stage still declares vec2 / vec4 and reads
    the same values it would if all six fields were 32-bit.  The packed layout is 28 bytes against
    36 unpacked (-22%), for no shader change and no visible precision loss -- the two decisions
    worth recording are WHY each packing is safe, because both have a failure mode that is
    invisible until it is not:

      uv as UNORM16X2 -- 1/65535, against a largest atlas of 1024 px, is 64 steps per texel, so a
        glyph's sample lands where it did.  What it cannot represent is U OUTSIDE [0,1], and one
        primitive needs that: a dashed line spans U 0..len/period and lets the sampler's REPEAT
        tile the atlas stipple row.  That is what GUI_FX_TILE_U exists for -- the repeat count
        moves into the effect word and the VERTEX stage multiplies, so the stored UV stays inside
        [0,1] and the interpolated one is unchanged.  Any future primitive that wants to tile does
        the same; storing U > 1 directly would silently CLAMP.

      fxc as HALF2 -- half is only ~3 decimal digits, and the effect coordinate reaches hundreds of
        pixels at the centre of a large panel, where its ulp is half a pixel.  It is safe anyway,
        and for a reason specific to how the field is used: an SDF only matters near its ZERO
        crossing, and linear interpolation weights each vertex by proximity, so the error there is
        dominated by the NEAR vertex -- whose value is small (radius + feather, tens of pixels) and
        whose ulp is therefore ~0.008 px.  The far vertex's half-pixel error arrives multiplied by
        a barycentric weight of a few percent.  Measured at the boundary of a 1600 px panel the
        total is under 0.02 px.  A uniform fixed-point encoding would be WORSE (0.06 px
        everywhere) despite sounding safer: what matters is precision where the field is zero, not
        precision on average.

    THE TEXTURE TRAVELS PER VERTEX, for the same reason the effect word does: so it cannot split
    a batch.  Coverage, SDF and sprite art are a pixel format and a sampler apart, so they cannot
    share a texture -- if the texture rode per-DRAW instead of per-vertex, they could not share a
    draw call either, and a window's background fill, its SDF label and an icon would be three
    draws alternating by z-order.  Carried per vertex, the only thing that opens a new draw call
    is a clip-rect change.

    This is the one place the design leans on being bindless.  Slate must batch by texture because
    it binds a descriptor per batch; here the fragment indexes a 2048-entry array, so the slot is
    just a number and a number can live in a vertex.  The fragment indexes with nonuniformEXT since
    neighbouring primitives in one draw legitimately name different textures.
==============================================================================================*/

/* What the fragment does with the effect coordinate.  Four bits, so the band has room to grow
   without touching the vertex again. */
typedef enum
{
    GUI_FX_NONE      = 0,  /* no effect -- (ex, ey) and the parameters are ignored (the default) */
    GUI_FX_BOX       = 1,  /* filled rounded box: coverage 1 inside the boundary, feathered across it */
    GUI_FX_RING      = 2,  /* the same boundary as a BAND of `border` px lying INSIDE it             */
    GUI_FX_PULSE     = 3,  /* a BOX whose alpha breathes on pc.time -- the band's first clock reader */

    /* The two modes that are not SHAPES.  Both leave coverage at 1 and act somewhere else in the
       pipeline -- proof the word is really "what the fragment does", not "which SDF to evaluate". */
    GUI_FX_TILE_U    = 4,  /* VERTEX stage: multiply u by a repeat count (see GUI_FX_TILE_MAX)  */
    GUI_FX_TEXT_EDGE = 5,  /* SDF text drawn with a second colour OUTSIDE the glyph boundary    */

    GUI_FX_SEG       = 6,  /* CAPSULE: a line segment `radius` px thick, with round caps        */

    /* The CIRCULAR-SECTOR modes.  All read the effect coordinate as a SIGNED offset from the
       shape centre, already rotated so the sector's bisector points +y in that local frame -- see
       the note below on why these need no fold and therefore cost ONE quad. */
    GUI_FX_ARC       = 7,  /* annular sector: a band of `tube` px centred on radius ra, round caps */
    GUI_FX_PIE       = 8,  /* filled wedge: the disc of radius ra cut to the aperture, sharp edges */

    /* The SELF-SAMPLED sector modes.  From 9 up a mode declares its shape solid colour by
       definition: the fragment does not consult the texel (coverage forced 1), which FREES the
       vertex's 32-bit uv word to carry mode-specific parameters -- the quad stamps the same value
       on all four corners, interpolation is flat, and unorm16 round-trips k/65535 exactly.  The
       same ARC word partition (ra | tube | aperture) still applies; only the uv payload differs. */
    GUI_FX_ARC_DASH  = 9,  /* ARC whose coverage is cut by an angular dash pattern: uv.x = the dash
                              period as a fraction of a full turn, uv.y = the on-duty fraction   */
    GUI_FX_ARC_GRAD  = 10, /* ARC whose colour sweeps from the vertex colour at the sector start
                              to a second RGBA8 riding the uv word (x = r|g<<8, y = b|a<<8)      */

    /* The FRAMEBUFFER-TILING pattern modes.  Self-sampled like the two above, but the pattern
       coordinate is gl_FragCoord / SV_Position -- exact float pixels straight from the
       rasterizer -- rather than the interpolated HALF2 effect coordinate, whose ulp reaches a
       full pixel at the far corners of a fullscreen backdrop, and backdrops are exactly the
       shapes that get that big.  The phase rides the packed words, so the pattern anchors to
       the SHAPE, not the screen: a backdrop drags with its window instead of sliding under it. */
    GUI_FX_CHECKER   = 11, /* two colours alternating in cell-sized squares: col_b rides the uv
                              word (the ARC_GRAD lanes); cell + phase live in the effect word   */
    GUI_FX_GRID      = 12, /* line lattice every `cell` px, `thickness` px wide, antialiased.
                              The vertex colour draws the LINES only -- layer it over any fill.
                              Phase rides the uv word as a per-axis fraction of the cell        */

} gui_fx_mode_t;

/* The effect coordinate (ex, ey) is the shape-local quantity `|p| - c`, where p is the vertex's
   offset from the shape centre and c the centre-rect half-extent (half-size minus the corner
   radius).  The absolute value is why an SDF box is tessellated as four QUADRANT quads rather
   than one: within a quadrant the sign of p is constant, so |p| is affine in p and the hardware's
   linear interpolation reproduces it exactly.  Across one quad it would fold at the centre line.
   The fragment then needs only `d = length( max( ex_ey, 0 ) ) - radius`.

   Note precisely WHY the fold has to happen at the vertex: it is the SUBTRACTION of c that forces
   it.  c is not in the packed word, so the fragment cannot redo `|p| - c` itself.  An axis with no
   subtraction needs no fold at all -- which is the whole reason GUI_FX_SEG costs half what a box
   does.  A capsule subtracts a half-length on the along-axis only, so that axis folds (two quads,
   split at the midpoint perpendicular) while the across-axis rides SIGNED and the fragment squares
   it inside length().  Two quads, and the shape is a rotated segment with exact round caps:

       ex = |p_along| - halflen        ey = p_across  (signed)
       d  = length( vec2( max( ex, 0 ), ey ) ) - radius

   That form is the true distance to the segment inside and out, so unlike the rounded box it needs
   no interior-distance term to stay correct in its core.

   THE SECTOR MODES FOLD NOTHING, and that is what makes an arc affordable.  Re-read the rule above:
   the fold is forced by the SUBTRACTION of c, not by the absolute value.  A circular shape has
   c = 0 -- the centre rect of a disc is a point -- so there is nothing to subtract, the effect
   coordinate is just p, and p is affine in the vertex position over the WHOLE shape.  One quad
   interpolates it exactly.  The fragment applies its own abs() to the interpolated value, which is
   exact because the value it folds is.

   Getting the fold for free is not the point though; getting the ANGLE is.  |p| destroys the sign
   and with it any way to tell where on the circle a fragment lies, which is why no amount of
   quadrant-folding could ever have expressed an arc.  Signed p keeps it, so a sector costs:

       ex, ey = p rotated so the sector's bisector points +y     (the CPU does this rotation)
       q      = ( |ex|, ey )                                     (the fragment's own fold)
       ARC    = the distance to the circle of radius ra, cut to the aperture, minus the tube
       PIE    = that disc intersected with the angular half-plane

   The rotation is on the CPU because it is per-shape, not per-fragment: four vertices pay for it
   instead of every pixel, and it means the packed word carries ONE aperture rather than two
   absolute angles.  The frame is a reflection (det -1), which is harmless -- the shape is symmetric
   about the bisector by construction, and the pipeline does not cull.

   One precision note specific to these modes.  Every other mode's coordinate is near ZERO at the
   boundary, which is the whole argument for HALF2 above; a sector's is near ra, so its ulp at the
   boundary is the ulp of ra rather than of a corner radius.  At UI radii (<= 64 px) that is 0.03 px
   and invisible; by 512 px it is 0.25 px.  Arcs that large are not a UI shape, but that is where
   the limit is and it is not the same limit the box has. */

/* The packed effect word.  Every field is a fixed-point pixel quantity sized to its physical
   range: a corner radius can be half a panel, a shadow's falloff is tens of pixels, a border is
   single digits.  Quantization is 1/8 px on radius and border and 1/4 px on feather -- all finer
   than the rasterizer can show. */
#define GUI_FX_MODE_BITS     4
#define GUI_FX_RADIUS_MAX    511.875f    /* 12 bits at 1/8 px */
#define GUI_FX_FEATHER_MAX   127.75f     /*  9 bits at 1/4 px */
#define GUI_FX_BORDER_MAX    15.875f     /*  7 bits at 1/8 px */

/* THE FRAME CLOCK the fragment sees, in seconds, wrapped to GUI_FX_TIME_WRAP.  It rides the PUSH
   CONSTANT, not the vertex, and that is the whole point: time is the same number for every shape
   in the frame, so spending 4 bytes per vertex to repeat it would tax every glyph on screen to say
   nothing new.  In the push constant it is free in the other direction too -- the flush writes it
   once before the first draw of a surface and never touches it again, so unlike a per-draw effect
   parameter it splits no batch.  A time-driven effect therefore re-emits NO geometry and adds NO
   draw call: the retained cache keeps last frame's vertices and only the constant moves.
   Caveat, and it is the whole cost: the idle skip means a frame is only presented when something
   asks for one.  A purely shader-driven animation has no emit to raise wants_redraw, so whatever
   owns the effect must call gui()->request_redraw() while it runs -- exactly as a volatile widget
   does.  Time advancing is not the same as the frame advancing.
   The wrap is a power of two so f32 still resolves ~0.1 ms at the far end, and so any effect whose
   period divides 1024 s -- every power-of-two fraction of a second -- runs continuously across it.
   Any other period sees one discontinuity every ~17 minutes. */
#define GUI_FX_TIME_WRAP     1024.0

/* Clamp a fixed-point field into the range its bit width can hold, BEFORE the shift.  Every packer
   below goes through it, because the alternative -- masking after the shift -- turns an
   out-of-range value into a WRAPPED one instead of a clamped one: a 600 px corner radius (a
   "fully round" pill on a tall panel, the idiom being `draw_set_rounding( 9999 )` clamped to half
   the short side) would wrap to 88 px, and the shape would simply look wrong with nothing to
   point at.  Saturating is not merely safer, it is the only answer that is ever wanted:
   every one of these fields is a physical pixel quantity whose max is past what the rasterizer can
   show, so a value beyond it means "as much as you have".  Negatives clamp to 0 for a second
   reason -- converting a negative float to unsigned is undefined behaviour in C. */
static inline u32
gui_fx_fixed( f32 v, f32 scale, u32 max_q )
{
    if ( !( v > 0.0f ) ) return 0u;                    /* also catches NaN */
    f32 q = v * scale + 0.5f;
    return ( q >= (f32)max_q ) ? max_q : (u32)q;
}

/* mode | radius (1/8 px) | feather (1/4 px) | border (1/8 px).  Each field saturates at its own
   maximum (GUI_FX_*_MAX above) rather than wrapping -- see gui_fx_fixed. */
static inline u32
gui_fx_pack( gui_fx_mode_t mode, f32 radius, f32 feather, f32 border )
{
    u32 r = gui_fx_fixed( radius,  8.0f, 0xFFFu );
    u32 f = gui_fx_fixed( feather, 4.0f, 0x1FFu );
    u32 b = gui_fx_fixed( border,  8.0f, 0x7Fu  );
    return ( (u32)mode & 0xFu ) | ( r << 4 ) | ( f << 16 ) | ( b << 25 );
}

/* PULSE re-partitions the word rather than asking for a wider one: the 28 parameter bits are FULL,
   so a mode that wants a new field spends the one it does not use.  Radius and feather keep their
   positions -- the shape is a BOX and the fragment decodes them with the same two shifts -- and the
   7 bits RING spends on `border` become rate + depth.  This is the pattern any future mode follows.
     rate  -- 4 bits at 1/4 Hz.  Quantized to quarters ON PURPOSE: rate * GUI_FX_TIME_WRAP is then
              always a whole number of cycles, so every pulse crosses the clock wrap seamlessly.
     depth -- 3 bits over 0..1.  The fraction of alpha the pulse removes at its trough; 0 is a
              still box, 1 fades fully out and back. */
#define GUI_FX_RATE_MAX      3.75f       /* 4 bits at 1/4 Hz */
#define GUI_FX_DEPTH_STEPS   7.0f        /* 3 bits over 0..1 */

static inline u32
gui_fx_pack_pulse( f32 radius, f32 feather, f32 rate, f32 depth )
{
    u32 r  = gui_fx_fixed( radius,  8.0f, 0xFFFu );
    u32 f  = gui_fx_fixed( feather, 4.0f, 0x1FFu );
    u32 hz = gui_fx_fixed( rate,    4.0f, 0xFu   );
    u32 dp = gui_fx_fixed( depth, GUI_FX_DEPTH_STEPS, 0x7u );
    return (u32)GUI_FX_PULSE | ( r << 4 ) | ( f << 16 ) | ( hz << 25 ) | ( dp << 29 );
}

/* TILE_U spends the whole parameter field on one number: how many times the source U span repeats
   across the primitive.  The stored UV then stays in [0,1] (which is all UNORM16X2 can hold) and
   the VERTEX stage multiplies, so the value the fragment interpolates is exactly what a wide U
   would have given -- the two ends are stored exactly, and everything between is a lerp either way.
   24 bits at 1/16 covers any line on any display; the quantization is a sub-sixteenth of a period
   of phase at the far end of a stipple, which has no correct value to begin with. */
#define GUI_FX_TILE_MAX      1048575.9375f   /* 24 bits at 1/16 */

/* TEXT_EDGE re-partitions the word the way PULSE did, and can spend ALL 28 bits because it is the
   first mode whose SHAPE does not come from (ex, ey) -- an SDF glyph's boundary is in the texture,
   so radius and feather have nothing to say.  What it buys is Slate's SecondaryColor without
   Slate's vertex field: a second colour outside the glyph edge, from ONE quad and one draw.
     width -- 8 bits at 1/8 px, 0..31.875.  Limited in practice by the SPREAD baked into the SDF
              atlas (gui_font_t.sdf_range): the field saturates past it, so the outline simply
              stops growing rather than tearing.  Scale the width with the text.
     colour -- RGBA at 5 bits each.  Coarse on purpose, and it costs nothing where it is used:
              black and white are exact, and an outline is a pixel or two wide, which is not enough
              area to show a 1/32 step in a hue. */
#define GUI_FX_EDGE_MAX      31.875f         /* 8 bits at 1/8 px */

static inline u32
gui_fx_pack_tile_u( f32 repeats )
{
    u32 n = gui_fx_fixed( repeats, 16.0f, 0xFFFFFFu );
    return (u32)GUI_FX_TILE_U | ( n << 4 );
}

/* ARC / PIE re-partition the word one more time, and they are the first modes to spend the whole 28
   bits on GEOMETRY.  There is no feather field: a sector is a stroke, a stroke wants exactly one
   pixel of antialiasing, and the 9 bits a feather would cost buy the aperture instead -- which is
   the parameter without which the shape does not exist at all.  The fragment hardcodes the 1 px
   band (see gui.frag); tess_fx_arc sizes its skirt from the same constant.
     ra       -- 12 bits at 1/8 px, the sector's own radius: the CENTRELINE for an ARC, the outer
                 edge for a PIE.  Shares GUI_FX_RADIUS_MAX with the box modes.
     tube     --  7 bits at 1/8 px, HALF the stroke thickness, so the band spans ra +/- tube.  PIE
                 packs 0.  A thickness past 2 * GUI_FX_ARC_TUBE_MAX is a hoop rather than a stroke
                 and the symbol layer keeps the polyline for it, exactly as draw_circle does.
     aperture --  9 bits over 0..pi: HALF the swept angle, measured from the bisector.  Half rather
                 than the full sweep because the CPU has already rotated the coordinate to put the
                 bisector on +y, which buys the symmetry that turns two absolute angles into one
                 number.  512 steps over pi is 0.35 degrees -- finer than a progress readout at any
                 radius a UI draws. */
#define GUI_FX_ARC_TUBE_MAX        15.875f   /* 7 bits at 1/8 px: max HALF-thickness */
#define GUI_FX_ARC_APERTURE_STEPS  511.0f    /* 9 bits over 0..pi */
#define GUI_FX_PI                  3.14159265358979f

static inline u32
gui_fx_pack_arc( gui_fx_mode_t mode, f32 ra, f32 tube, f32 aperture )
{
    u32 r = gui_fx_fixed( ra,   8.0f, 0xFFFu );
    u32 t = gui_fx_fixed( tube, 8.0f, 0x7Fu  );
    u32 a = gui_fx_fixed( aperture, GUI_FX_ARC_APERTURE_STEPS / GUI_FX_PI, 0x1FFu );
    return ( (u32)mode & 0xFu ) | ( r << 4 ) | ( t << 16 ) | ( a << 23 );
}

/* CHECKER / GRID -- the framebuffer-tiling patterns.  Both spend the 12 bits the box modes give
   the radius on the CELL pitch instead, at 1/4 px (a pattern pitch is authored in whole pixels;
   1023.75 px caps well past any backdrop cell).  What remains differs per mode:
     CHECKER -- phase_x / phase_y, 8 bits each: the box origin's offset into the TWO-cell colour
                period, as a fraction of that period, so the fragment re-anchors cell PARITY to
                the shape (one cell of phase would swap the two colours).  1/256 of a period is
                1/8 px on a 16 px cell -- under the rasterizer's floor.  col_b rides the uv word
                in the ARC_GRAD lanes.
     GRID    -- thickness, 7 bits at 1/8 px (the border field's own partition): the full line
                width, straddling the lattice line.  The phase pair moves to the uv word (one
                unorm16 per axis, fraction of ONE cell -- lines repeat per cell, so parity does
                not exist) because a grid has one colour and it is the vertex colour, which
                leaves the whole uv word free. */
#define GUI_FX_CELL_MAX      1023.75f    /* 12 bits at 1/4 px */

static inline u32
gui_fx_pack_checker( f32 cell, f32 phase_x, f32 phase_y )   /* phases: fraction of 2*cell, [0,1) */
{
    u32 c  = gui_fx_fixed( cell,    4.0f,   0xFFFu );
    u32 px = gui_fx_fixed( phase_x, 255.0f, 0xFFu  );
    u32 py = gui_fx_fixed( phase_y, 255.0f, 0xFFu  );
    return (u32)GUI_FX_CHECKER | ( c << 4 ) | ( px << 16 ) | ( py << 24 );
}

static inline u32
gui_fx_pack_grid( f32 cell, f32 thickness )
{
    u32 c = gui_fx_fixed( cell,      4.0f, 0xFFFu );
    u32 t = gui_fx_fixed( thickness, 8.0f, 0x7Fu  );
    return (u32)GUI_FX_GRID | ( c << 4 ) | ( t << 16 );
}

/* abgr is the same R-in-the-low-byte word every other colour here uses (R8G8B8A8_UNORM order). */
static inline u32
gui_fx_pack_text_edge( f32 width, u32 abgr )
{
    u32 w = gui_fx_fixed( width, 8.0f, 0xFFu );
    u32 r = ( ( abgr       ) & 0xFFu ) >> 3;
    u32 g = ( ( abgr >>  8 ) & 0xFFu ) >> 3;
    u32 b = ( ( abgr >> 16 ) & 0xFFu ) >> 3;
    u32 a = ( ( abgr >> 24 ) & 0xFFu ) >> 3;
    return (u32)GUI_FX_TEXT_EDGE | ( w << 4 ) | ( r << 12 ) | ( g << 17 ) | ( b << 22 ) | ( a << 27 );
}

/*----------------------------------------------------------------------------------------------
    The packed vertex, and the two constructors that are the ONLY supported way to build one.

    Positional compound literals are what a struct of six scalars invites, and they are exactly
    what this layout cannot survive: `{ x, y, u, v, abgr }` against the packed fields would put a
    float U into a u32 UV slot -- a legal implicit conversion, so a warning at best and a silently
    black quad at worst.  The constructors take the same plain floats a positional literal would,
    and do the packing themselves, so a call site reads the same way and a wrong one does not
    compile.

    Note what they do NOT set: `tex` and `fx` stay 0 here on purpose.  Both are stamped by the
    tessellator's single commit point (tess_verts_commit) from ambient state, which is what makes
    them impossible to forget -- see the comment there.  Both are PRIMITIVE-constant (a shape has
    one effect word, a glyph run has one outline, a quad has one texture) while the effect
    COORDINATE is the only part of the band that varies per corner, which is why that is the one
    thing a constructor still takes.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32 x, y;     // pixel position
    u32 uv;       // texture UV, two unorm16 over [0,1] (gui_uv_pack)
    u32 abgr;     // packed color
    u32 fxc;      // effect coordinate |p| - c as two halves (GUI_FX_NONE ignores it)
    u32 fx;       // packed effect word (gui_fx_pack); low nibble 0 = no effect
    u32 tex;      // sampling model + bindless slot (GUI_TEX_MODE | index) -- see above

} gui_draw_vert_t;

/* IEEE binary32 -> binary16, round-half-UP (struct/hardware convention is half-to-even, so the two
   differ by 1 ulp on an exact tie -- 2049 goes to 2050 here and 2048 there).  An exponent past the
   half range clamps to the largest finite value with the sign kept, which is also where infinities
   and NaN land; a mantissa carry at the very top of the range is deliberately allowed to ripple
   into the exponent and produce inf, because that is the correctly rounded answer.

   None of those edges are reachable from an effect coordinate -- these are pixel magnitudes in the
   hundreds, so the live paths are the normal one and flush-to-subnormal near zero.  Verified
   against Python's binary16 packing across the range and every boundary listed above. */
static inline u16
gui_f16_from_f32( f32 f )
{
    union { f32 f; u32 u; } in;
    in.f = f;

    u32 sign = ( in.u >> 16 ) & 0x8000u;
    i32 exp  = (i32)( ( in.u >> 23 ) & 0xFFu ) - 127 + 15;
    u32 man  = in.u & 0x7FFFFFu;

    if ( exp >= 31 )                       /* overflow / inf / nan -> largest finite, sign kept */
        return (u16)( sign | 0x7BFFu );

    if ( exp <= 0 )                        /* below the normal range -> subnormal, or zero */
    {
        if ( exp < -10 )
            return (u16)sign;
        man |= 0x800000u;                  /* restore the implicit leading 1 */
        u32 shift = (u32)( 14 - exp );     /* 14..24 */
        u32 sub   = ( man >> shift ) + ( ( man >> ( shift - 1 ) ) & 1u );
        return (u16)( sign | sub );
    }

    /* Normal.  The round-up carry is allowed to ripple into the exponent -- that is the correct
       result when a mantissa of all ones rounds up, and it is why this is not masked. */
    u32 h = sign | ( (u32)exp << 10 ) | ( man >> 13 );
    return (u16)( h + ( ( man >> 12 ) & 1u ) );
}

/* UV -> two unorm16.  Clamped, because that is the only thing the format can do with an out-of-
   range coordinate -- a caller that wants U past 1 asks for GUI_FX_TILE_U instead.

   The assert is the point of this function: clamping is SILENT, and a primitive that quietly loses
   its tiling renders as one stretched texel rather than as an error.  Debug catches the mistake at
   the vertex that made it; release keeps the clamp, which at least stays inside the atlas. */
static inline u32
gui_uv_pack( f32 u, f32 v )
{
    ORB_ASSERT( u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f );
    u = ( u < 0.0f ) ? 0.0f : ( ( u > 1.0f ) ? 1.0f : u );
    v = ( v < 0.0f ) ? 0.0f : ( ( v > 1.0f ) ? 1.0f : v );
    return (u32)( u * 65535.0f + 0.5f ) | ( (u32)( v * 65535.0f + 0.5f ) << 16 );
}

static inline u32
gui_fxc_pack( f32 ex, f32 ey )
{
    return (u32)gui_f16_from_f32( ex ) | ( (u32)gui_f16_from_f32( ey ) << 16 );
}

/* A vertex with no effect -- what every primitive that is not an SDF surface writes. */
static inline gui_draw_vert_t
gui_vert( f32 x, f32 y, f32 u, f32 v, u32 abgr )
{
    return ( gui_draw_vert_t ){ x, y, gui_uv_pack( u, v ), abgr, 0u, 0u, 0u };
}

/* The same, carrying the per-corner effect coordinate.  The effect WORD is ambient (above). */
static inline gui_draw_vert_t
gui_vert_fxc( f32 x, f32 y, f32 u, f32 v, u32 abgr, f32 ex, f32 ey )
{
    return ( gui_draw_vert_t ){ x, y, gui_uv_pack( u, v ), abgr, gui_fxc_pack( ex, ey ), 0u, 0u };
}

/*==============================================================================================
    GUI_DRAW -- volatile widget callback

    A "volatile" callback contains ordinary UI emit calls (text, colored rects, etc).  It runs
    inline during a real (dirty) frame via gui()->volatile_cb -- its widgets render exactly like
    any other code, no special behavior, except that the block's geometry is given its own
    RESERVED, PADDED region of the window's cached tessellation (vertex/index/draw-command
    headroom past what it actually produced).  On an idle frame (frame_dirty()==false), the same
    callback is invoked again standalone with is_replay=true; the framework reconstructs just
    enough context (window/clip/cursor position + the ambient draw state stamped at real emit),
    re-tessellates the output, and patches it into the reserved region.  The output does NOT have
    to match the original -- text may grow or shrink, shapes may change -- it only has to FIT the
    reservation; outgrowing it costs one automatic real frame, after which the block re-captures
    with a larger reservation (grow-only per widget).  The block's commands are also excluded from
    the window's retained-cache hash, so an animating block never forces its window to
    re-tessellate -- see gui_volatile_cb / gui_volatile_begin.

    Interactive widgets (button, etc) are safe to call from a volatile callback but are inert
    during replay: hover/active/focus reflect whatever the last real frame established, but a
    replay can never newly acquire either state or fire a fresh click -- interaction is only ever
    resolved on real frames, which is guaranteed since input changes always force one.

    CONTRACT -- fixed layout footprint: the block's PIXELS may change every frame, but the space
    it occupies in the layout should not.  Surrounding widgets are retained and only re-lay-out on
    real frames, so a block that grows (e.g. text gaining a digit) is drawing over neighbours that
    cannot move until layout runs again.  This is CHECKED, not merely asked for: the layout extent
    each replay claims is compared against the last real emit's, and a change forces one real
    frame -- the same self-healing cost as outgrowing the geometry reservation, so a footprint that
    varies is a performance cost (a real frame per change) rather than a visual defect.  A callback
    whose layout simply does not reproduce under the replay scope is caught after a few frames and
    the check latches off for it (Debug asserts).  Still prefer fixed-size formatting ("%8.3f" with
    a mono font), a fixed canvas(), or padding: a footprint that changes every frame gives up the
    idle skip entirely, which is the whole point of the widget.
      The footprint is also the block's CLIP: the range is scissored to the cell it measured, cut
    to its region's view (volatile_cb_close).  So the contract bounds where a block can paint, not
    only where it lays out -- ink past the cell is cut rather than landing on a neighbour or on the
    window chrome, and the block's geometry batches apart from the rest of the window instead of
    splitting it in two.

    CONTRACT -- flow layouts only: the replay scope is a minimal single-column stack at the cell
    the block occupied, so stack and columns call sites replay exactly (a block in a multi-track
    row must emit ONE item -- its second widget would claim the next track on real frames but
    stack below on replay).  Grid and pack call sites are not reproduced (the stamp falls back to
    the frame cursor); do not put volatile blocks there.
==============================================================================================*/

/* `id` is the block's registry id -- the volatile_cb label hashed against the call site's id
   scope -- identical at real emit and on replay, so a SHARED callback serving many blocks can
   derive stable per-block variety (colors, phases) from it.  Position must not be used for
   that: the rect a block occupies legitimately moves (resize, relayout) and anything derived
   from it re-rolls with every pixel. */
typedef void ( *gui_volatile_fn )( gui_id_t id, bool is_replay );

/*==============================================================================================
    GUI_DRAW -- semantic draw commands

    The UI build pass emits one gui_cmd_t per visible shape into a list.  The render backend
    (gui_render.c) tessellates each command into vertices and indices at flush time.  This
    separates the UI logic from any graphics API knowledge.

    GPU draw commands (gui_gpu_cmd_t) are a backend-private type defined in gui_emit_draw.c;
    they carry index ranges and bind state for one GPU draw call.
==============================================================================================*/

typedef enum
{
    GUI_CMD_RECT_FILLED,     // filled rectangle or textured quad (glyph); rounding > 0 makes it
                             //   an SDF surface -- a filled DISC is this command at radius ==
                             //   half-extent (draw_push_circle_filled), not a type of its own
    GUI_CMD_RECT_OUTLINE,    // hollow rectangle: four edge quads (GUI_FX_RING when rounded)
    GUI_CMD_TRIANGLE,        // solid triangle
    GUI_CMD_TEXT,            // glyph run from the font atlas
    GUI_CMD_TEXT_XF,         // glyph run under a uniform scale + a rotation about its origin
    GUI_CMD_LINE,            // single stroke segment
    GUI_CMD_POLYLINE,        // multi-segment antialiased polyline
    GUI_CMD_DASHED_LINE,     // patterned line: one textured quad, atlas dash row, tiled by U
    GUI_CMD_RECT_GRADIENT,   // filled rect, col_a->col_b blended by per-vertex color (one quad)
    GUI_CMD_RECT_LIST,       // batch of solid rects from the per-frame rect pool (one cmd, N quads)
    GUI_CMD_SPRITE,          // RGBA sprite quad; nine-slice expanded at tessellation when the
                             //   sprite carries slice insets (1, 3 or 9 quads from one command)
    GUI_CMD_FX_BOX,          // the parameterized GUI_FX_BOX surface: a soft shadow (wide feather)
                             //   or a shader-clock pulse (rate/depth) -- one member, mode derived
    GUI_CMD_ROUND_RECT_EX,   // filled box with a PER-CORNER radius: four GUI_FX_BOX quadrants,
                             // each carrying its own packed word
    GUI_CMD_ARC,             // stroked circular arc, round caps: one GUI_FX_ARC quad
    GUI_CMD_PIE,             // filled wedge, sharp radial edges: one GUI_FX_PIE quad
    GUI_CMD_ARC_DASH,        // arc cut by an angular dash pattern (GUI_FX_ARC_DASH): dotted rings,
                             //   marching ants, tick dials -- still one quad
    GUI_CMD_ARC_GRAD,        // arc whose colour sweeps col_a -> col_b along the sector
                             //   (GUI_FX_ARC_GRAD): the hot/cold value arc -- still one quad
    GUI_CMD_IMAGE_XF,        // textured quad under a rotation about its centre: rotated icons,
                             //   images, markers -- the text_xf treatment for one quad
    GUI_CMD_CHECKER,         // two-colour cell pattern resolved in the FRAGMENT (GUI_FX_CHECKER):
                             //   the transparency backdrop as ONE quad, versus the 64 commands /
                             //   4096 quads a rect-pool expansion would cost at its 64x64 clamp
    GUI_CMD_GRID,            // line lattice (GUI_FX_GRID): graph-paper / node-graph backdrop in
                             //   one quad; the lattice anchors to (ox, oy) so it pans with content

} gui_cmd_type_t;

/* Sentinel half-extent for an unclipped text command: any real glyph sits well inside this, so
   the tessellator's clip test never triggers and the whole-run fast path is taken. */
#define GUI_TEXT_NO_CLIP 1e30f

/* THE SAMPLING MODEL -- the top 4 bits of a rect command's tex_idx.  What a texel MEANS to the
   fragment: the one axis the shader branches on, and the axis the two atlases are already split
   along (render/resource/gui_res_atlas.h).

   It rides the tex_idx rather than taking a field of its own because it is a property of the
   TEXTURE, not of the shape drawn with it -- so wherever the slot goes, the model goes with it for
   free.  That is what lets both of them live in the VERTEX (see gui_draw_vert_t) without widening
   anything: one u32 carries the pair, so models MIX freely inside one draw call instead of needing
   to be kept apart.  The SAMPLER is DERIVED from the mode in the fragment and never carried per
   command: coverage must stay point-sampled or glyphs stop being crisp, colour must filter or it
   blocks up the moment it is stretched.

   That derivation is the whole reason this is a MODE rather than a bool: a third sampling model
   (SDF) is one more value, not a format change.  Three of the sixteen are spent; the rest stay
   unnamed until something emits them, the same rule the effect band's spare modes follow.

   FOUR bits, matching GUI_FX_MODE_BITS -- the two mode fields answer the same kind of question and
   grow by the same rule.  The bits come out of the INDEX half, which never needed them: the RHI's
   bindless array is 2048 entries (11 bits) and the low 28 hold 268M.  This word is the shader
   contract: the shift below must equal TEX_MODE_SHIFT in gui.frag / gui.ps.hlsl (and the
   paraphrase in gui_shader.h) -- change one, change all, resplice the SPIR-V. */
#define GUI_TEX_MODE_SHIFT  28u
#define GUI_TEX_MODE_MASK   ( 0xFu << GUI_TEX_MODE_SHIFT )
#define GUI_TEX_MODE( m )   ( (u32)( m ) << GUI_TEX_MODE_SHIFT )

typedef enum
{
    GUI_TEX_COVERAGE = 0,   /* R8: the texel's R is alpha and the vertex colour supplies RGB --
                               glyphs, icons, the drawing assists, every solid fill (white texel) */
    GUI_TEX_RGBA     = 1,   /* RGBA: the texel IS the colour and the vertex colour tints it --
                               sprite art, and a caller's own texture via draw_texture_in        */
    GUI_TEX_SDF      = 2,   /* R8 SIGNED DISTANCE: 128 is the outline, and the fragment recovers
                               coverage from the screen-space derivative rather than the texel.
                               That is what lets distance-field text scale and rotate cleanly --
                               and why it must filter, which is why it could not be a COVERAGE
                               font wearing a different flag (orb_font.h, sdf_range)             */

} gui_tex_mode_t;

/* Split a command's tex_idx into its two halves: the model, and the bindless slot to sample. */
static inline gui_tex_mode_t
gui_tex_mode( u32 tex_idx )
{
    return (gui_tex_mode_t)( tex_idx >> GUI_TEX_MODE_SHIFT );
}

static inline u32
gui_tex_index( u32 tex_idx )
{
    return tex_idx & ~GUI_TEX_MODE_MASK;
}

/* One semantic draw command.  The 4-byte header carries the command type, the index of the active
   scissor rect in the per-frame clip table (assigned at clip-push time -- no per-emit search), and
   the target viewport.  z lives in gui_cmd_seg_t (per-segment, constant within a window) and is not
   repeated here.  Reducing the header from 28 bytes to 4 bytes brings the struct from 72 -> 48 bytes.
   tex_idx == 0 in rect means solid color (white texel).
   rounding (rect / rect_outline) is the corner radius baked from the ambient draw rounding at emit
   time, already clamped to the rect; 0 tessellates as a plain square shape.
   text.off is a byte offset into the frame's text pool (s_draw.text_pool), not a pointer: the
   string lives in the pool until the next frame_begin, so the command is valid through flush.
   Storing an offset instead of a const char* keeps the union at 4-byte alignment. */
typedef struct
{
    u8 type;       // gui_cmd_type_t, fits u8 (20 values)
    u8 clip_idx;   // index into per-frame s_draw.clip_table (set at push time)
    u8 vp;         // target viewport (GUI_MAX_VIEWPORTS = 4, fits u8)
    u8 _pad;
    union
    {
        struct { f32 x, y, w, h, u0, v0, u1, v1; f32 rounding; u32 tex_idx; u32 abgr; } rect;
        struct { f32 x, y, w, h, t;              f32 rounding;              u32 abgr; } rect_outline;
        struct { f32 ax, ay, bx, by, cx, cy;                     u32 abgr; } tri;
        /* clip_x0/clip_x1 are the horizontal pixel window for glyph-level clipping: the first and
           last straddling glyphs are cut and their U remapped; interior glyphs emit whole.  The
           sentinel (clip_x0 = -GUI_TEXT_NO_CLIP, clip_x1 = +GUI_TEXT_NO_CLIP) means unclipped
           and takes the original whole-run fast path. */
        /* edge is the ambient TEXT_EDGE word at emit time (0 = none): a second colour painted
           outside the glyph boundary, resolved by the fragment from the SAME quad, so an outlined
           or shadowed run costs no extra geometry, no second pass, and no batch split.  It rides
           the command rather than being re-read at tessellation because the ambient can have moved
           on by then -- a retained window re-tessellates long after its emit. */
        /* font is the registry id whose glyph metrics and atlas UVs this run resolves from -- the
           ONLY thing the font decides.  It rides the command rather than the command SEGMENT
           because a segment is the backend's batch-dispatch unit and the font is not a batch key:
           every font packs into one shared atlas, so a font change moves no texture, and even
           when it does the texture rides the vertex and cannot cut a draw call either.  Tagging a
           batch unit with a per-command property would force a segment split just for a lookup --
           see draw_set_font. */
        struct { f32 x, y;  u32 off; u32 len;  f32 clip_x0, clip_x1;  u32 abgr; u32 edge;
                 u16 font; } text;
        /* The same glyph run under a uniform SCALE and a ROTATION about (x, y) -- (x, y) is both
           the run's top-left in its own space and the pivot, so a caller places any other pivot by
           moving the origin.  rot is radians in screen space (the angle algebra above: 0 points
           +x, positive turns clockwise).
           Its own command rather than two more fields on text, for the same reason shadow is not
           a feather on rect: text is the hot path every chrome label goes through, and it is also
           the shape the text CONSUMERS assume -- the selection capture hit-tests axis-aligned runs
           and the glyph clip window cuts on a screen-x boundary, neither of which survives a
           rotation.  A separate type is what lets both of them skip a transformed run cleanly
           instead of measuring it wrong.  No clip window: the GPU scissor is its only clip.
           Nothing here is snapped to the pixel grid -- see tess_text_xf. */
        struct { f32 x, y;  u32 off; u32 len;  f32 scale, rot;        u32 abgr; u32 edge;
                 u16 font; } text_xf;
        struct { f32 x0, y0, x1, y1, thickness;                  u32 abgr; } line;
        struct { u32 pt_offset; u32 pt_count; f32 thickness;
                 gui_stroke_align_t align; bool closed;         u32 abgr; } polyline;
        /* Dashed line tessellates to one oriented textured quad: U spans 0..len/period so the
           atlas dash row tiles along the line; duty (on-fraction) picks the nearest baked row. */
        struct { f32 x0, y0, x1, y1, thickness, period, duty;     u32 abgr; } dash;
        /* Gradient rect: one quad with col_a/col_b on opposite edges; the GPU interpolates the
           per-vertex color across it.  horizontal != 0 = left->right, else top->bottom.  Always
           square.  u32, not bool, for the same no-tail-padding rule as sprite below -- the
           retained-cache hash folds these bytes raw. */
        struct { f32 x, y, w, h; u32 col_a, col_b; u32 horizontal; } gradient;
        /* Rect list: `count` solid fills from the per-frame rect pool (s_draw.rect_pool), one
           command for the whole batch -- the dense-shape escape valve (timeline bars, graph
           columns).  Always square, white texel, per-entry color; entries share the clip. */
        struct { u32 offset; u32 count; } rect_list;
        /* Sprite quad.  The sprite ID travels, not its UVs: placement in the sprite atlas can move
           under a repack, so the tessellator resolves through the sprite source contract at flush
           time exactly as a glyph does, and res_sprite_generation folds into the window hash to
           force that re-resolve.  nine != 0 asks for the slice expansion (a sprite with no insets
           draws as one stretched quad either way); it pairs with flags as a u16 rather than a bool
           so the member carries no tail padding -- the retained-cache hash folds these bytes raw
           and stale padding from a differently-typed command would read as a spurious change. */
        struct { f32 x, y, w, h; f32 scale; u32 sprite; u32 abgr; u16 flags; u16 nine; } sprite;
        /* The parameterized GUI_FX_BOX surface -- one member serving the soft shadow / glow
           (a wide `feather`, the total width of the falloff band straddling the boundary, so the
           geometry reaches feather/2 past the box on every side) and the pulsing box (`rate` in
           Hz + `depth` 0..1, alpha breathing on pc.time in the FRAGMENT -- geometry never
           changes, so the retained slot stays valid and nothing re-tessellates while it runs;
           the frame must still be PRESENTED, see GUI_FX_TIME_WRAP).  The mode is derived at
           tessellation: rate > 0 is a PULSE, else a BOX.  Its own member rather than
           feather/rate/depth bolted onto rect: rect is the hot variant every fill goes through,
           and widening it would grow the whole command pool for fields almost nothing sets.
           `rot` (radians, screen space, about the box CENTRE) turns the whole surface: the fx
           coordinate is box-local and affine, so rotating the four corner POSITIONS preserves the
           field under interpolation -- a rotated card costs the same four quadrant quads.  0 for
           every axis-aligned caller (shadow / pulse), and 0 keeps the grid snap. */
        struct { f32 x, y, w, h; f32 rounding, feather, rate, depth, rot; u32 abgr; } fx_box;
        /* Per-corner rounded fill -- the tab / notch / asymmetric card shape.  Geometrically it is
           the SAME four quadrant quads a uniform rounded rect emits; the one thing that differs is
           that each quad carries its own packed word, because the radius is the only shape
           parameter that lives in the WORD rather than in the vertices.  A quadrant already sees
           exactly one corner, so per-corner radii cost no extra geometry -- only the four separate
           stamps (see tess_fx_box_core).
           The field order IS the quadrant order the tessellator walks (top-left, top-right,
           bottom-right, bottom-left), so the two cannot drift apart.
           Filled and solid-colour only.  The stroked form stays a perimeter polyline: GUI_FX_RING
           derives its interior hole from a single radius, and generalizing that is not worth it for
           a shape whose outline the polyline already draws correctly.
           `feather` is the falloff band exactly as fx_box carries it -- 0 gets the standard 1 px
           AA, wider makes the per-corner SOFT SHADOW (the tab / asymmetric-card drop shadow); the
           quadrants agree at any feather (tess_fx_box_core's centre-line proof). */
        struct { f32 x, y, w, h; f32 rtl, rtr, rbr, rbl; f32 feather; u32 abgr; } round_rect;
        /* Circular sector -- ONE member serving GUI_CMD_ARC and GUI_CMD_PIE, which differ only in
           the field the fragment evaluates, not in anything they carry.  Angles are radians in
           screen space (0 points +x, positive turns clockwise, matching text_xf.rot); a1 < a0 is
           normalized at tessellation and a sweep of a full turn routes to the exact ring / disc
           primitives instead.  `thickness` is the stroke width for ARC and is ignored by PIE.
           Sampled as a polyline this shape would need up to 66 points fanned or stroked -- up to
           65 separate TRIANGLE commands for a pie, ~130 vertices for a spinner.  It costs one
           quad instead, because a circular field needs no quadrant fold (see the effect band). */
        struct { f32 cx, cy, r, thickness, a0, a1;                u32 abgr; } arc;
        /* The arc under an angular dash cut.  `period` is radians per dash+gap cycle -- the emit
           side quantizes it so a WHOLE number of cycles fits the sweep, which is what keeps a
           closed dashed ring from showing a seam where the pattern meets itself; `duty` is the
           on-fraction.  Both ride the quad's flat uv word to the fragment (GUI_FX_ARC_DASH). */
        struct { f32 cx, cy, r, thickness, a0, a1; f32 period, duty; u32 abgr; } arc_dash;
        /* The arc whose colour sweeps col_a (at a0) -> col_b (at a1).  col_b rides the quad's flat
           uv word; the fragment lerps by angle/aperture (GUI_FX_ARC_GRAD) -- the one gradient a
           4-corner vertex colour could never express, because it varies by ANGLE, not position. */
        struct { f32 cx, cy, r, thickness, a0, a1; u32 col_a, col_b; } arc_grad;
        /* A textured quad under a rotation about its CENTRE -- the text_xf treatment applied to
           one quad (tess_quad_xf).  UVs resolve at emit exactly as draw_push_icon's do: an icon
           is a quad forever, and the atlas UV moves only on a bake, which reloads everything.
           Centre pivot rather than the text anchor because a marker / needle / spinner glyph
           turns about its own middle -- the one pivot every caller was computing anyway. */
        struct { f32 x, y, w, h; f32 u0, v0, u1, v1; f32 rot; u32 tex_idx; u32 abgr; } image_xf;
        /* The cell pattern: col_a fills even cells, col_b odd, anchored at the box origin.  The
           fragment tiles it in framebuffer space (GUI_FX_CHECKER), so this is ONE quad at any
           area and any cell, where a rect-pool expansion would cap at 64x64 cells. */
        struct { f32 x, y, w, h; f32 cell; u32 col_a, col_b; } checker;
        /* The line lattice: a line every `cell` px, `thickness` px wide, in abgr over NOTHING --
           the caller layers it on its own fill.  (ox, oy) is the lattice anchor in screen px:
           lines land on ox + k*cell, so a panning canvas passes its content origin and the grid
           rides along (GUI_FX_GRID). */
        struct { f32 x, y, w, h; f32 cell, thickness, ox, oy; u32 abgr; } grid;
    };
} gui_cmd_t;

/*==============================================================================================
    GUI_DRAW -- direction: a cardinal direction, the ImGuiDir analogue.  Passed to arrow_button (and any
    future directional widget) to pick which way the glyph points.
==============================================================================================*/

typedef enum
{
    GUI_DIR_LEFT,
    GUI_DIR_RIGHT,
    GUI_DIR_UP,
    GUI_DIR_DOWN,

} gui_dir_t;

/*==============================================================================================
    GUI_CHROME -- input character filter

    What a text field will ACCEPT, checked per typed or pasted character (rejected characters
    are dropped silently; a rejected char never eats the selection).  Class bits union: a
    character passes if ANY named class admits it.  NO_BLANK / UPPERCASE modify on top;
    UPPERCASE maps before the class check, so lowercase hex typed into HEX|UPPERCASE lands as
    'A'..'F'.  Installed for the next input widget via gui()->next_input_filter(); the numeric
    entry fields (Ctrl+Click on a drag/slider box, input_int/_float) and the color hex field
    install their own internally.
==============================================================================================*/

typedef enum
{
    GUI_INPUT_FILTER_NONE      = 0,
    GUI_INPUT_FILTER_DIGITS    = 1 << 0,  /* 0-9 */
    GUI_INPUT_FILTER_INT       = 1 << 1,  /* 0-9 + - */
    GUI_INPUT_FILTER_DECIMAL   = 1 << 2,  /* 0-9 . + - e E */
    GUI_INPUT_FILTER_HEX       = 1 << 3,  /* 0-9 a-f A-F # */
    GUI_INPUT_FILTER_ALPHA     = 1 << 4,  /* a-z A-Z */
    GUI_INPUT_FILTER_NO_BLANK  = 1 << 5,  /* spaces dropped */
    GUI_INPUT_FILTER_UPPERCASE = 1 << 6,  /* a-z mapped to A-Z before the class check */

} gui_input_filter_t;

/*==============================================================================================
    GUI_CHROME -- color edit flags
==============================================================================================*/

typedef enum
{
    GUI_COLOR_EDIT_NONE        = 0,
    GUI_COLOR_EDIT_NO_ALPHA    = 1 << 0,  /* ColorEdit4 with alpha ignored/hidden */
    GUI_COLOR_EDIT_DISPLAY_HSV = 1 << 1,  /* Display inputs as HSV */
    GUI_COLOR_EDIT_FLOAT       = 1 << 2,  /* Display inputs as float 0..1 instead of 0..255 */
    GUI_COLOR_EDIT_NO_PICKER   = 1 << 3,  /* color_edit: clicking the swatch does not open the picker popup */
    GUI_COLOR_EDIT_NO_INPUTS   = 1 << 4,  /* color_picker: SV square + bars only (no drag fields / hex row) */

} gui_color_edit_flags_t;

/*==============================================================================================
    GUI_CHROME -- window drag mode: how a window may be repositioned by the mouse.
    Selected globally via gui()->window_set_drag(); default is TITLEBAR.
==============================================================================================*/

typedef enum
{
    GUI_WIN_DRAG_NONE     = 0,    /* windows are fixed in place                          */
    GUI_WIN_DRAG_TITLEBAR = 1,    /* drag only by the title bar (default)                */
    GUI_WIN_DRAG_BODY     = 2,    /* drag from anywhere in the window not over a widget  */

} gui_win_drag_t;

/*==============================================================================================
    GUI_CHROME -- apply condition: when a queued window_set_next_* value takes effect on its target window.

    Passed to window_set_next_pos / window_set_next_size.  The value and the condition are two
    separate axes: the same window can be seeded once, forced every frame, or re-applied whenever
    it re-appears, by changing only the condition -- the reason geometry is a side channel rather
    than fixed window_begin parameters.  Bit values so a window can mask the conditions it still
    permits.  A 0 (unset) condition is treated as ALWAYS.
==============================================================================================*/

typedef enum
{
    GUI_COND_ONCE      = 1 << 0,   /* apply once for this window, then never again -- seeds the
                                      initial position/size on first appearance and never again
                                      (akin to Dear ImGui's FirstUseEver) */

    GUI_COND_ALWAYS    = 1 << 1,   /* apply every frame the value is queued -- forced geometry for
                                      layout managers, snapping, animation; pair with NOMOVE /
                                      NORESIZE so a user drag does not fight it */

    GUI_COND_APPEARING = 1 << 2,   /* apply each time the window appears -- on creation and again
                                      whenever it is shown after a frame of absence (e.g. re-center
                                      a reopened popup / modal) */
} gui_cond_t;

/*==============================================================================================
    GUI_CHROME -- window flags (policy bits shared down to GUI_SURFACE panes / regions / children)

    Passed as the final argument to window_begin to customize a single window's behavior.
    They mostly switch off default behavior; pass 0 (GUI_WIN_NONE) for the defaults.
==============================================================================================*/

typedef enum
{
    GUI_WIN_NONE              = 0,         /* default behavior */

    /* Title bar & chrome -- strip default window decoration. */

    GUI_WIN_NOTITLEBAR        = 1 << 0,    /* no title bar: body fills the top; no collapse, no titlebar drag */
    GUI_WIN_NOCOLLAPSE        = 1 << 1,    /* no collapse arrow; the window stays expanded */

    /* Menu bar -- reserve a one-row strip below the title bar that menu_bar_begin fills (the
       ImGuiWindowFlags_MenuBar analogue).  The strip is carved from the top of the body before the
       body scroll region opens, so it never scrolls; menu_bar_begin returns false unless set. */

    GUI_WIN_MENUBAR           = 1 << 2,    /* reserve a non-scrolling menu-bar strip (menu_bar_begin) */

    /* Move & resize -- the window is user-movable and edge-resizable by default. */

    GUI_WIN_NOMOVE            = 1 << 3,    /* disable user drag moving the window from anywhere */
    GUI_WIN_NORESIZE          = 1 << 4,    /* disable user resizing from the border edges */

    /* Placement is managed externally (docking layout, animation, scripted snap).  Bypasses both
       the per-drag margin clamp (window_clamp) and the merge-back fit-inside clamp so the system
       can position and size the window freely without gui fighting the placement.  Without this
       flag both clamps apply unconditionally; with it neither does. */

    GUI_WIN_NO_BOUNDARY_CLAMP = 1 << 5,    /* placement is externally managed; skip both clamps */

    /* Auto-resize -- size the window to its content instead of a fixed w/h.  Window-only: a
       region already autosizes per-axis on w/h <= 0 without this bit (see GUI_WIN_CHILD_RESIZE_X/_Y
       below for how a region mixes autosize and resize across its two axes); gui_region_begin
       does not read it. */

    GUI_WIN_ALWAYS_AUTOSIZE   = 1 << 6,    /* hug content every frame: no user resize, no scrollbars */
    GUI_WIN_CAN_AUTOSIZE      = 1 << 7,    /* show a corner size-grip; double-click it to fit content */

    /* Scrolling -- a dynamic vertical bar and mouse-wheel input are on by default.  The NO* flags
       switch pieces off; the ALWAYS_* flags force a bar to stay shown regardless of content. */

    GUI_WIN_NOSCROLL          = 1 << 8,    /* disable all scroll bars (keep mouse input) */
    GUI_WIN_NOMOUSESCROLL     = 1 << 9,    /* disable mouse wheel scrolling */
    GUI_WIN_HSCROLL           = 1 << 10,   /* enable dynamic horizontal scroll bar (off by default) */
    GUI_WIN_ALWAYS_VSCROLL    = 1 << 11,   /* always show vertical scroll bar -- override */
    GUI_WIN_ALWAYS_HSCROLL    = 1 << 12,   /* always show horizontal scroll bar -- override */

    /* Native-borderless: this window IS its host OS window (window kind 3).  Its titlebar stands in
       for the Win32 caption and its border for the sizing frame, so titlebar drag / double-click /
       right-click and border drags are routed to native OS window actions (app()->window_start_move
       / window_title_event / window_system_menu / window_start_resize) instead of gui's in-client
       move, tear-off, collapse, and edge-resize.  The host OS window must have been opened with
       APP_WIN_BORDERLESS.  Geometry is owned by the OS window (size follows WM_SIZE).

       Hosts normally do not pass this flag directly: gui()->viewport_shell() is the front door --
       it emits the frame-only shell (and no-ops on an OS-chrome window).  Pass NATIVE yourself
       only to build a custom shell window. */

    GUI_WIN_NATIVE            = 1 << 13,

    /* Title-bar capability -- subtract a caption control a window should not offer.  All three
       buttons show by default (opt-out, like the NO* flags above); a window drops the ones it does
       not support and the title bar reflects that, reclaiming the freed space for the title text.

       NO_MINIMIZE / NO_MAXIMIZE gate the minimize / maximize pair everywhere it appears: the OS
       caption buttons of a native window (GUI_WIN_NATIVE or a detached floater), and the gui
       title-bar buttons every movable regular floater shows by default.  On a regular floater,
       maximize pins the window to its surface's work area (below the native caption band and the
       main menu bar) and raises it over everything else; minimize parks it as a title-bar chip on
       a shelf along the surface's bottom edge -- click the chip (or its restore button) to bring
       it back.  Double-click on the title bar toggles maximize when it is offered (the collapse
       toggle then lives on the arrow alone); dragging a maximized title bar restores first, OS
       style.  ALWAYS_AUTOSIZE windows own their geometry and never maximize.  The close (main) /
       pop-in (floater) primary button is never suppressed: close is essential and pop-in is a
       floater's only route back to the main surface.

       NO_DETACH removes the pop-out path for any window -- it hides the non-native detach button and
       blocks the drag tear-off -- independent of NOMOVE (a window may move yet refuse to pop out). */

    GUI_WIN_NO_MINIMIZE       = 1 << 14,   /* no minimize button (native caption or gui title bar) */
    GUI_WIN_NO_MAXIMIZE       = 1 << 15,   /* no maximize / restore button (native or gui) */
    GUI_WIN_NO_DETACH         = 1 << 16,   /* no pop-out: hide detach button, block tear-off drag */

    /* Closeable -- add a close (X) button at the title bar's right edge.  Clicking it hides the
       window: window_begin returns false and emits nothing from then on, and the record persists
       so the window keeps its position / size while closed.  Re-opening is the caller's job --
       offer a button that calls window_set_open( title, true ).  A native window uses its OS close
       caption button instead, so this flag only adds the X to a regular (non-native) panel. */

    GUI_WIN_CLOSEABLE         = 1 << 17,   /* show a close (X) button; hidden until re-opened */

    /* Never hosts tabs: the window is not a tab-drop target (no center chip when another window is
       title-dragged over it) and window_tab refuses it as `onto_title`.  For control / instruction
       panels whose body the host gates on window_begin's return -- becoming an inactive tab would
       silently skip that body.  The window itself may still be dragged INTO groups or dockspaces. */

    GUI_WIN_NO_TAB_TARGET     = 1 << 18,   /* not a tab-drop target; window_tab refuses it as onto_title */

    /* Input passthrough -- the window is purely visual; the cursor passes through it as if it
       were not there.  hover_win is never set to this window, so no widget inside can receive
       mouse input and the window never steals clicks from content behind it.  Combine with
       GUI_WIN_OVERLAY for a completely inert window-based HUD.  region_begin (gui_region.c)
       honors this flag too -- a region is interactive by default, opt out with NO_INPUT for a
       fixed-rect HUD with no window identity at all (the perf overlay's case). */

    GUI_WIN_NO_INPUT          = 1 << 19,   /* click-through: never becomes hover_win */

    /* Child / root regions -- child_begin and gui_region_begin.

       CHILD_RESIZE_X / _Y (the ImGuiChildFlags_ResizeX / _ResizeY analogue): a draggable grip on
       the right / bottom border; the size on that axis then becomes user-owned and persisted --
       seeded once from the child_begin / region_begin w/h, thereafter set by the drag --
       overriding the passed value.  RESIZE_Y supersedes the h<=0 auto-size on that axis.  A real
       window ignores these (it owns its geometry already), as does a grid-cell child (the cell
       sizes it).  Vertical is the common case; both axes may be combined.

       Each axis picks independently -- a region may autosize x (its w argument <= 0) while
       CHILD_RESIZE_Y drives y, or any other mix; they are not mutually exclusive across axes.

       NO_CLIP: skip pushing a draw clip rect for this region.  Use when the caller knows
       content fits and wants to avoid the extra draw batch the scissor causes. */

    GUI_WIN_CHILD_RESIZE_X    = 1 << 20,   /* child/region: drag the right border to resize width  */
    GUI_WIN_CHILD_RESIZE_Y    = 1 << 21,   /* child/region: drag the bottom border to resize height */
    GUI_WIN_NO_CLIP           = 1 << 22,   /* child/region: do not push a clip rect */

    /* Arena band: routes this window's (or region's) retained geometry into the debug band of
       the shared vertex/index arena.  Debug-band windows pack AFTER every main-band slot, are
       excluded from the render stats they may themselves display, and never raise any_changed /
       frame_dirty (so a live readout cannot silently defeat idle-skip for the whole app).  For
       self-measuring diagnostic UI -- the perf overlay and the pipeline dashboard; popups and
       tooltips opened from inside a debug-band window inherit the band automatically. */

    GUI_WIN_DEBUG_BAND        = 1 << 23,   /* diagnostic UI: debug arena band + stats/dirty exempt */

    /* Docked maximize -- opt-in: while this window is its dock node's ACTIVE tab, the node's tab
       strip offers a maximize / restore button (double-click on the strip's empty band toggles
       too).  Maximizing pins the node over the WHOLE dockspace, fully obscuring the other docked
       nodes -- their windows suppress (window_begin returns false, inactive-tab semantics) until
       restore -- while the tab strip stays usable on top.  The transition eases like the floater
       maximize and obeys the same global switch (window_anim_enable).  Opt-in because it suits
       fullscreen-able content (a 3D viewport toggling between full view and its dock pane), not
       ordinary panels; dock_window_maximize is the programmatic twin and ignores the flag. */

    GUI_WIN_DOCK_MAXIMIZE     = 1 << 24,   /* docked: offer maximize-over-dockspace on the tab strip */

    /* Text selection -- opt-in: every text run this window draws becomes selectable, including
       text drawn BY widgets (button labels, tree rows).  Two gestures: click-drag ON text
       sweeps a linear web-style highlight (multi-line); click-drag on window-background dead
       space rubber-bands a marquee box that selects every character it covers, over widgets
       and all.  Ctrl+C copies the covered runs to the OS clipboard (newline between lines),
       Escape or a plain click clears.  Widgets stay untouched -- the selection is a fallback
       consumer of presses no widget claimed, so buttons / selectables / scrollbars always win
       (but the marquee does consume body dead-space drags, so it shadows GUI_WIN_DRAG_BODY).
       ONE selection exists at a time across all flagged windows; what is copied is what is
       DRAWN (an ellipsized run copies its truncated bytes; scrolled-away lines are not part
       of the selection). */

    GUI_WIN_TEXT_SELECT       = 1 << 25,   /* window text runs are selectable / copyable */

    /* Modal overlay: a top-level window pinned into the overlay z-band (above every normal
       window) that fences ALL interaction behind it -- every other window goes inert until this
       one stops being emitted.  The exclusive game-console / modal-dialog primitive, WITHOUT the
       popup layer's auto-centering and background dim (popup_modal_begin owns that look): the
       caller positions and sizes the window itself.  Keyboard is not auto-captured -- pair with
       set_keyboard_focus for a text-entry modal.  Emit it last in the build so it wins z. */
    GUI_WIN_MODAL             = 1 << 26,

    /* Bottom-anchored content -- the region justifies its whole content block to the BOTTOM of its
       view instead of the top: when the content underfills the view the slack falls at the TOP and
       the last (newest) row hugs the bottom edge; when it overflows, the block is top-anchored and
       the scroll offset is pinned to the tail (the newest row), following it as content grows until
       the user scrolls up -- scrolling back to the bottom re-arms the follow.  The caller emits rows
       in natural order (oldest first); no bottom-up pen math.  A console / log / chat transcript.
       Works on any region (window body, child_begin, region_begin); combine with NOSCROLL for a bar-
       less console (the wheel and scroll_by still drive it). */

    GUI_WIN_ANCHOR_BOTTOM     = 1 << 27,

    /* Convenience composites -- common flag bundles named for intent (the ImGuiWindowFlags_NoXxx
       shorthands).  Plain ORs of the bits above, so they compose with extra flags as usual
       ( GUI_WIN_OVERLAY | GUI_WIN_NOMOUSESCROLL ) and a window's resolved behavior is identical
       to spelling the members out.

       NODECORATION -- strip all chrome: no title bar, no border resize, no scrollbars, no collapse.
                       A bare content panel you still position / move yourself.
       OVERLAY      -- a passive, non-interactive window-based HUD: undecorated, fixed in place,
                       hugging its content every frame, and non-detachable.  Pin it with
                       window_set_next_pos.  For a HUD with no window identity at all (no pool
                       record, no dock/native/z-order path), use region_begin instead. */

    GUI_WIN_NODECORATION = GUI_WIN_NOTITLEBAR | GUI_WIN_NORESIZE |
                           GUI_WIN_NOSCROLL   | GUI_WIN_NOCOLLAPSE,

    GUI_WIN_OVERLAY      = GUI_WIN_NODECORATION    | GUI_WIN_NOMOVE |
                           GUI_WIN_ALWAYS_AUTOSIZE | GUI_WIN_NO_DETACH | GUI_WIN_NO_INPUT,

} gui_win_flags_t;

/*==============================================================================================
    GUI_SURFACE -- region z tier: where a root region sits in the one z contest windows and
    popups compete in (whichever z wins, wins draw order and hover that frame).  A three-way
    choice, so it is a parameter rather than flag bits.

    MID -- the default: a fixed band above every ordinary window and below every popup, so a HUD
           element draws over normal windows but under a menu / combo / modal.
    BG  -- lowest tier (ties the docked/base window floor).  A background element that only
           receives hover when nothing else (no raised window) covers it -- e.g. a desk-level
           widget that must not steal clicks from anything on top of it.
    FG  -- highest tier (above every popup depth).  Always wins draw order and hover, even over
           an open menu/combo/modal -- e.g. an always-on-top HUD button that must remain
           clickable no matter what else is open.
==============================================================================================*/

typedef enum
{
    GUI_REGION_MID = 0,     /* default band: over windows, under popups */
    GUI_REGION_BG,          /* background: loses to any raised window   */
    GUI_REGION_FG,          /* foreground: wins over every popup        */

} gui_region_tier_t;

/*==============================================================================================
    GUI_SURFACE -- pane: the MINIMAL top-level surface occupant: the block every "window" is built from.

    Both servers already run on this cross-section: the interaction server's occlusion contest
    keys on (id, rect, z, viewport), and the render backend's per-window unit is a command
    span tagged (id, z, viewport).  A pane is exactly that shared tag plus the io-side rect:
    identity for hover/active attribution and the retained-cache key, a rect for hit test and
    base clip, one z that decides BOTH paint order and the hover contest, the hosting OS
    surface, and whether it competes for input at all.

    Everything called a window is a pane plus policy: + scroll link = scrolling region
    (region_begin); + persisted rect and chrome = the stock window (window_begin); + overlay
    z band = popup.  pane_begin (gui_api.h GUI_CORE) opens the raw block for callers building
    their own chrome; the returned struct is a same-frame value, not a persistent record.
==============================================================================================*/

/* Edge bits for edge-resize -- the mask a caller hands feat_resize (and the vocabulary the
   internal edge-resize service, its highlight painter, and the stock window share).  Combine
   for a corner (GUI_RESIZE_R | GUI_RESIZE_B). */
#define GUI_RESIZE_L  ( 1u << 0 )
#define GUI_RESIZE_R  ( 1u << 1 )
#define GUI_RESIZE_T  ( 1u << 2 )
#define GUI_RESIZE_B  ( 1u << 3 )

typedef struct gui_pane_t
{
    gui_id_t   id;      // identity: hover attribution, state pool key, draw segment tag
    gui_rect_t rect;    // where it is; hit test + base clip derive from it
    u32        z;       // one number, two consumers: occlusion contest + paint order
    u8         vp;      // hosting OS surface (viewport index)

} gui_pane_t;

/*==============================================================================================
    GUI_CHROME -- dockspace flags

    Passed to dockspace_over_viewport.  0 (GUI_DOCKSPACE_NONE) is the default dockspace that fills
    the viewport behind the free-floating windows.  Bit values so future policy bits (e.g. hide the
    single-tab strip, no central-node auto-hide) can be ORed in without changing the call sites.
==============================================================================================*/

typedef enum
{
    /* default: fill the viewport, draw splitters + tab bars */
    GUI_DOCKSPACE_NONE = 0,

    /* Tab docking only: windows may tab into leaves (center drop) but never split them -- no side
       or edge drop chips are offered and the programmatic split verbs refuse, so no splitters can
       ever form.  The dockspace degenerates to one full-area tab group.  Sticky per viewport:
       re-published every dockspace_over_viewport call. */
    GUI_DOCKSPACE_NO_SPLIT = 1 << 0,

} gui_dockspace_flags_t;

/*==============================================================================================
    GUI_CHROME -- combo flags

    Passed to combo_begin to tune the dropdown.  The HEIGHT_* group caps the dropdown to a fixed
    number of visible rows (then it scrolls) -- the ImGuiComboFlags_Height* analogue; they are
    mutually exclusive, so set exactly one (an unset height defaults to REGULAR / 8 rows).  0
    (GUI_COMBO_NONE) is the default no-tweak set.
==============================================================================================*/

typedef enum
{
    GUI_COMBO_NONE            = 0,         /* default behavior (REGULAR height) */

    GUI_COMBO_HEIGHT_SMALL    = 1 << 0,    /* cap the dropdown to ~4 rows, then scroll   */
    GUI_COMBO_HEIGHT_REGULAR  = 1 << 1,    /* cap to ~8 rows (the default), then scroll  */
    GUI_COMBO_HEIGHT_LARGE    = 1 << 2,    /* cap to ~20 rows, then scroll               */
    GUI_COMBO_HEIGHT_LARGEST  = 1 << 3,    /* no cap: as many rows as fit on screen      */

    /* Mask of the height bits, to clear the group before setting one (the demo idiom). */
    GUI_COMBO_HEIGHT_MASK     = GUI_COMBO_HEIGHT_SMALL | GUI_COMBO_HEIGHT_REGULAR
                              | GUI_COMBO_HEIGHT_LARGE | GUI_COMBO_HEIGHT_LARGEST,

} gui_combo_flags_t;

/*==============================================================================================
    GUI_CHROME -- tab bar

    tab_bar_begin / tab_item_begin open an in-window tabbed content switcher (the ImGuiTabBar
    analogue): a strip of clickable chips above the body, with only the selected tab's widgets
    emitted below it.  Distinct from the docking tab strips, which tab whole windows into a dock
    node -- this tabs SECTIONS of one window's body.  See gui_api.h for the usage contract.

    0 (GUI_TAB_BAR_NONE / GUI_TAB_ITEM_NONE) is the default no-tweak set; the flag params exist so
    behavior can grow (reordering, fitting policy) without changing the call sites.
==============================================================================================*/

typedef enum
{
    GUI_TAB_BAR_NONE = 0,     /* default behavior */

    /* Room to grow -- e.g. a future GUI_TAB_BAR_REORDERABLE (drag to reorder tabs),
       GUI_TAB_BAR_FILL (chips expand to fill the strip width). */

} gui_tab_bar_flags_t;

typedef enum
{
    GUI_TAB_ITEM_NONE = 0,    /* default behavior */

    /* Room to grow -- e.g. a future GUI_TAB_ITEM_SET_SELECTED (force this tab active this frame). */

} gui_tab_item_flags_t;

/*==============================================================================================
    GUI_CHROME -- table support

    begin_table / end_table open a multi-column layout with independent cell clipping.
    Use table_setup_column before any row to name and size columns, then iterate with
    table_next_row + table_next_column.  See gui_api.h for the full ergonomic contract.

    Column count limit is GUI_TABLE_COLS_MAX.  Column sizes use the same overloaded f32 as
    the layout engine: >1 = fixed pixels, 1 = stretch / fill, (0,1) = fraction.

    Columns have a LOGICAL index (setup order -- what every table_*_column call takes and
    returns) and a DISPLAY position (left-to-right on screen, which GUI_TABLE_REORDERABLE lets
    the user drag around).  Hidden columns (GUI_TABLE_HIDEABLE) keep their logical index and
    are simply skipped by table_next_column.  Widths, display order, visibility, sort choice,
    and scroll are all persisted per table id.
==============================================================================================*/

#define GUI_TABLE_COLS_MAX 16

typedef enum
{
    GUI_TABLE_NONE            = 0,
    GUI_TABLE_BORDERS_H       = 1 << 0,   // horizontal row dividers (between rows)
    GUI_TABLE_BORDERS_V       = 1 << 1,   // vertical column dividers (between columns)
    GUI_TABLE_BORDERS_OUTER   = 1 << 2,   // outer frame border around the whole table
    GUI_TABLE_BORDERS         = GUI_TABLE_BORDERS_H | GUI_TABLE_BORDERS_V | GUI_TABLE_BORDERS_OUTER,
    GUI_TABLE_SCROLL_Y        = 1 << 3,   // table body scrolls vertically
    GUI_TABLE_SCROLL_X        = 1 << 4,   // table body scrolls horizontally
    GUI_TABLE_SORTABLE        = 1 << 5,   // clicking a header column header sorts
    GUI_TABLE_ROW_STRIPES     = 1 << 6,   // alternating even/odd row background tint
    GUI_TABLE_RESIZABLE       = 1 << 7,   // drag column borders to resize
    GUI_TABLE_NO_HEADER       = 1 << 8,   // skip table_headers_row entirely
    GUI_TABLE_REORDERABLE     = 1 << 9,   // drag a header sideways to reorder columns
    GUI_TABLE_HIDEABLE        = 1 << 10,  // columns can be hidden (see the context menu below)
    GUI_TABLE_SORT_TRISTATE   = 1 << 11,  // sort cycle gains an unsorted step: asc -> desc -> none
    GUI_TABLE_HIGHLIGHT_COL   = 1 << 12,  // tint the whole column under the cursor
    GUI_TABLE_NO_CONTEXT_MENU = 1 << 13,  // suppress the built-in right-click header menu

} gui_table_flags_t;

typedef enum
{
    GUI_TABLE_COL_NONE         = 0,
    GUI_TABLE_COL_FIXED        = 1 << 0,  // fixed pixel width -- does not stretch
    GUI_TABLE_COL_STRETCH      = 1 << 1,  // fill remaining space (default when width==0)
    GUI_TABLE_COL_NO_RESIZE    = 1 << 2,  // pins this column's right boundary (no drag)
    GUI_TABLE_COL_NO_SORT      = 1 << 3,  // not clickable for sort
    GUI_TABLE_COL_ALIGN_RIGHT  = 1 << 4,  // right-align cell content (and the header label)
    GUI_TABLE_COL_ALIGN_CENTER = 1 << 5,  // center cell content (and the header label)
    GUI_TABLE_COL_WIDTH_AUTO   = 1 << 6,  // width tracks the widest content measured (fit-to-content)
    GUI_TABLE_COL_DEFAULT_HIDE = 1 << 7,  // starts hidden (GUI_TABLE_HIDEABLE tables)
    GUI_TABLE_COL_NO_HIDE      = 1 << 8,  // cannot be hidden -- omitted from the context menu
    GUI_TABLE_COL_NO_REORDER   = 1 << 9,  // cannot be dragged, and no column may cross it
    GUI_TABLE_COL_DEFAULT_SORT = 1 << 10, // table opens sorted on this column
    GUI_TABLE_COL_PREFER_DESC  = 1 << 11, // first sort click on it sorts descending

} gui_table_col_flags_t;

/* Background color override target for table_set_bg_color. */
typedef enum
{
    GUI_TABLE_BG_NONE = 0,
    GUI_TABLE_BG_ROW,     // tint the current entire row
    GUI_TABLE_BG_CELL,    // tint the current cell only

} gui_table_bg_target_t;

/* Sort specification returned by table_get_sort_specs (always filled, whether or not the sort
   changed this frame -- the return value is the "it changed" signal). */
typedef struct
{
    i32  col;          // sorted column index (logical); -1 = unsorted
    bool descending;   // false = ascending

} gui_table_sort_specs_t;

/* Sort key for one cell, filled by the value callback below.  Set num + is_num for a numeric
   compare; otherwise set str for an alphabetical (strcmp) compare.  A row that leaves both unset
   sorts as an empty string / zero. */
typedef struct
{
    const char* str;      // alphabetical key (used when is_num is false)
    f64         num;      // numeric key (used when is_num is true)
    bool        is_num;   // true = compare num; false = compare str

} gui_table_sort_value_t;

/* Built-in sort: supply the sort key for one cell.  row is the user data index, col the column
   being sorted.  Let the table handle alphabetical / numeric ordering and the sort direction. */
typedef void ( *gui_table_sort_value_fn )( i32 row, i32 col, gui_table_sort_value_t* out,
                                             void* user );

/* Custom sort: full-control comparator -- return <0 / 0 / >0 like strcmp.  a and b are user data
   indices, col the sorted column, descending the requested direction (apply or ignore it as you
   wish; the table does NOT negate the result for you). */
typedef i32 ( *gui_table_sort_cmp_fn )( i32 a, i32 b, i32 col, bool descending, void* user );

// clang-format on
/*==============================================================================================
    GUI_FRAME -- font configuration
==============================================================================================*/
/* Built-in font presets for init() -- pre-baked .orb_font assets (FreeType-rasterized offline by
   font_tool, not an stb runtime bake) shipped under assets/font/.  GUI_FONT_NONE loads nothing;
   the caller is then responsible for its own font_load() before the first frame renders. */

typedef enum
{
    GUI_FONT_NONE = 0,        // load nothing; caller loads its own font(s) via font_load()
    GUI_FONT_JETBRAINS_12,
    GUI_FONT_JETBRAINS_16,
    GUI_FONT_JETBRAINS_20,
    GUI_FONT_JETBRAINS_24,
    GUI_FONT_ROBOTO_12,
    GUI_FONT_ROBOTO_16,
    GUI_FONT_ROBOTO_20,
    GUI_FONT_ROBOTO_24,
    GUI_FONT_CASCADIA_MONO_12,
    GUI_FONT_CASCADIA_MONO_16,
    GUI_FONT_CASCADIA_MONO_20,
    GUI_FONT_CASCADIA_MONO_24,
    GUI_FONT_CASCADIA_MONO_32,
    GUI_FONT_CASCADIA_CODE_16,

    GUI_FONT_BUILTIN_COUNT

} gui_builtin_font_t;

/*==============================================================================================
    GUI_FRAME -- DPI response mode

    How gui reacts to monitor scale (app()->window_dpi_scale; the process is per-monitor DPI
    aware, so all engine coordinates are physical pixels).  gui scales by retargeting the
    ACTIVE FONT within the init() preset's family -- a bigger bake raises em, and every layout
    metric already rescales from em -- so the response granularity is the set of baked sizes
    that family ships.  Each surface (viewport) resolves against ITS OWN hosting window's
    scale, so floaters on differently-scaled monitors each get the right bake (mixed DPI).
    See dpi_set() in gui_api.h.
==============================================================================================*/

typedef enum
{
    GUI_DPI_OFF = 0,    // ignore monitor scale -- UI stays at the authored bake, 1:1 pixels
    GUI_DPI_AUTO,       // follow each surface's own monitor scale (default)
    GUI_DPI_MANUAL      // apply the explicit factor passed to dpi_set()

} gui_dpi_mode_t;

/*==============================================================================================
    GUI_FRAME -- boot descriptor

    A simple one-call host setup (gui()->boot). The gui owns the main OS window and its render 
    context end to end -- the same lifecycle its tear-off floaters already use -- instead of the
    host assembling window_open / context_open / init / viewport_open by hand.  
    
    Everything here is optional in the sense that a field left zero keeps today's default; 
    the struct is designed to be built as a compound literal at the call site.  
    See boot() in gui_api.h for the full contract.
==============================================================================================*/

typedef struct
{
    const char*        title;       // OS window title; doubles as the chrome shell caption
    i32                x, y;        // window position; 0,0 = OS centers
    i32                w, h;        // client size; 0,0 = 50% of the desktop work area
    bool               os_chrome;   // true = stock OS frame; false (default) = borderless window
                                    // with the gui chrome shell auto-emitted

    bool               debug;       // arm the debug hotkey driver (debug_enable)
    gui_builtin_font_t font;        // built-in preset; GUI_FONT_NONE = caller font_load()s

    gui_clock_fn       clock;       // system clock function callback
    gui_sleep_fn       sleep;       // system sleep function callback
    gui_wait_events_fn wait;        // system wait-for-events function callback

    f32                clear[ 4 ];  // boot_present_begin clear color; alpha 0 = dark
    
} gui_boot_desc_t;

/*==============================================================================================
    GUI_FRAME -- limits
==============================================================================================*/

/* 32K verts covers the busiest measured frame (all sb_gui demo windows + the pipeline
   dashboard peak ~9K) several times over, and stays well inside u16 vertex indices (64K
   would sit right at the 65535 ceiling).  The 3x index ratio clears both mixes with room
   to spare -- quads run 6 idx per 4 verts (1.5:1) and AA paths / arcs stay under 2:1 --
   so geometry that would exceed it overflows a frame's tessellation, not the buffer
   sizing.  The per-frame region sizes that fall out of these (VB 896 KB at the packed
   28-byte vertex, IB 192 KB) are both 256-byte aligned, so each frame-in-flight region
   stays independently addressable -- note that this only matters if the VB/IB are ever
   moved off HOST_COHERENT memory, in which case regions would need rounding up to
   nonCoherentAtomSize to flush apart. */

#ifdef GUI_STRESS_TEST

/* Stress-bench build (the gui_stress lib variant, sb_gui_stress): the per-frame pools scale
   ~4x so the bench can push past shipping load without tripping caps.  Two are ceiling-bound,
   not 4x: verts stop just under 64K (u16 indices) and the clip table at 256 (u8 index). */

#define GUI_MAX_VERTS        ( 60 * 1024 )   /* per-frame tessellated vertices                   */
#define GUI_MAX_CMDS         8192            /* per-frame semantic draw commands                 */
#define GUI_MAX_PATH_PTS     32768           /* per-frame total polyline / path point pool       */
#define GUI_MAX_RECT_ENTRIES 16384           /* per-frame total draw_rects batch pool            */
#define GUI_MAX_TEXT_POOL    ( 64 * 1024 )   /* per-frame flat string copy pool for text cmds    */
#define GUI_MAX_CLIP_RECTS   256             /* per-frame clip table entries; u8 index caps at 256 */

#else

#define GUI_MAX_VERTS        ( 32 * 1024 )   /* per-frame tessellated vertices                   */
#define GUI_MAX_CMDS         1024            /* per-frame semantic draw commands                 */
#define GUI_MAX_PATH_PTS     2048            /* per-frame total polyline / path point pool       */
#define GUI_MAX_RECT_ENTRIES 4096            /* per-frame total draw_rects batch pool            */
#define GUI_MAX_TEXT_POOL    ( 16 * 1024 )   /* per-frame flat string copy pool for text cmds    */
#define GUI_MAX_CLIP_RECTS   64              /* per-frame clip table entries; u8 index caps at 256 */

#endif

#define GUI_MAX_IDX          ( GUI_MAX_VERTS * 3 )   /* 3x clears quads (1.5:1) and AA strips     */
#define GUI_CLIP_DEPTH       32                      /* push_clip / pop_clip nesting depth       */

/* Command segments: one contiguous span of the command list per (win, z, vp, band) the emit path
   stamps, cut wherever a window seam, draw_set_sort_key, draw_set_viewport or draw_set_band
   changes the tag (draw_seg_retag).  The render backend orders these spans instead of re-scanning
   the whole command buffer.  Worst case each command sits in its own segment, plus the open one,
   so the cap is the command cap + 1. */

#define GUI_MAX_SEGS       ( GUI_MAX_CMDS + 1 )

/*==============================================================================================
    GUI_FRAME -- memory usage breakdown (bytes), reported by gui()->mem_stats().

    A full accounting of what the gui system holds, split by WHERE it lives:

      - GPU       : device memory -- per-viewport geometry buffers, atlas textures, and (Debug)
                    the debug overlay's own buffers.  Dynamic: created at init / viewport_open,
                    released at shutdown / viewport_close.
      - CPU static: EVERY fixed backend buffer baked into the image (.bss/.rdata) -- the draw
                    list, tessellation staging, retained cache, font/atlas/icon registries,
                    render state + embedded shaders, capture buffers, debug tooling.  Present
                    for the whole run whether one window is open or fifty; summed exhaustively
                    in render/gui_render_mem.c (the accounting contract lives there).
      - CPU heap  : one malloc block per live context (header + state / popup / window /
                    viewport / dock pools).  Dynamic: grows only when a secondary context is
                    created.

    Every bucket is exact (a sizeof of the backing array, a summed malloc size, or a live-count
    multiply of a fixed region), so the grand total is the true resident footprint -- not a
    high-water estimate.  print_mem_stats() dumps the same breakdown as a sectioned table.
==============================================================================================*/

typedef struct
{
    /* --- GPU device memory (dynamic). --- */

    u32 gpu_vertex_bytes;       // per-viewport VB regions, summed over live surfaces x frames-in-flight
    u32 gpu_index_bytes;        // per-viewport IB regions, summed over live surfaces x frames-in-flight
    u32 gpu_texture_bytes;      // the three resource atlases (coverage incl. assist rows, sprite, SDF)
    u32 gpu_debug_bytes;        // debug-overlay VB/IB (Debug builds; 0 when compiled out / not created)
    u32 gpu_total;              // sum of the section above
    u32 viewport_count;         // live GPU surfaces contributing to gpu_vertex/index_bytes

    /* --- CPU static memory (.bss + .rdata; fixed backend buffers, resident the whole run). --- */

    u32 cpu_drawlist_bytes;     // EMIT: s_draw (cmds + hashes + point/rect/text/clip pools) + path stroker
    u32 cpu_tess_bytes;         // BUILD: s_tess CPU vertex / index / GPU-command staging
    u32 cpu_cache_bytes;        // retained cache: slot tables, stable cmd cache, diff records, seg chains,
                                //   permutation scratch, volatile registry, stats
    u32 cpu_draw_bytes;         // DRAW unit statics: icon + sprite registries (the font registry
                                //   lives under font/ and counts under cpu_frontend_bytes instead)
    u32 cpu_res_bytes;          // the three atlas instance records (packer nodes + tenant bookkeeping)
    u32 cpu_render_bytes;       // RENDER: pipeline/sampler state + embedded SPIR-V bytecode
    u32 cpu_select_bytes;       // text-selection run capture buffer (always compiled; a product feature)
    u32 cpu_debug_bytes;        // debug overlay + name registry + dashboard snapshot + command stepper
                                //   (each 0 when its feature is compiled out -- Release builds)
    u32 cpu_frontend_bytes;     // frontend statics: io snapshot, style/theme state, layout/id stacks,
                                //   undo buffers, gesture latches (gui_ui_mem.c) -- plus the font
                                //   registry, whose per-font resident bitmaps + ext records make this
                                //   the one frontend bucket that scales with loaded content
    u32 cpu_static_total;       // sum of the section above

    /* --- CPU dynamic memory (heap). --- */

    u32 cpu_context_bytes;      // sum over live contexts of the single ctx block (header + all pools)
    u32 context_count;          // live contexts contributing to cpu_context_bytes
    u32 cpu_atlas_bytes;        // atlas-owned heap: resident staging mirror + every tenant's retained
                                //   source copy, summed over all three atlases
    u32 cpu_dynamic_total;      // heap total: cpu_context_bytes + cpu_atlas_bytes

    /* --- Grand total: everything the gui system holds right now. --- */

    u32 total_bytes;            // gpu_total + cpu_static_total + cpu_dynamic_total

} gui_mem_stats_t;

/*==============================================================================================
    GUI_FRAME -- per-frame render statistics, reported by gui()->render_stats().

    A direct read on render density: the geometry the last completed frame tessellated and how
    many GPU indexed draw calls (batches) it cost to paint it across every surface.  Published at
    frame_begin, so a read during the build returns the PREVIOUS frame's totals -- the standard
    one-frame-lag metric (the build that reads it is also the one being measured).
==============================================================================================*/

typedef struct
{
    u32 cmd_count;          // semantic draw commands the UI emitted
    u32 clip_count;         // clip table entries referenced by those commands (debug band excluded)
    u32 seg_count;          // command segments cut this frame (per-(win,z,vp,band) spans)
    u32 text_pool_used;     // bytes of the per-frame text pool consumed (cap: GUI_MAX_TEXT_POOL)
    u32 vert_count;         // tessellated vertices (total, including retained)
    u32 tri_count;          // tessellated triangles (total, including retained)
    u32 draw_calls;         // GPU indexed draw calls (batches), summed over surfaces

    u32 win_total;          // windows tracked this frame
    u32 win_retained;       // windows whose geometry was reused (no re-tessellation)
    u32 vert_retained;      // vertices that came from prev-frame copy, not re-tessellated
    u32 tri_retained;       // triangles retained from prev-frame copy

    u32 upload_batches;     // number of buffer write calls per frame
    u32 upload_bytes;       // total bytes uploaded to GPU vertex and index buffers

    u32 volatile_patched;   // volatile_cb rows whose geometry was patched in place this frame
                           // (idle replay or a live real-frame reuse-patch) -- a separate signal
                           // from win_retained: a window with an animating volatile widget still
                           // counts as fully retained; this is what actually moved.
} gui_render_stats_t;

/*==============================================================================================
    GUI_DEBUG -- overlay layers

    Bitmask passed to gui()->debug_set_layers().  Each bit enables one bolt-on debug
    visualization, emitted into a separate draw list and painted last, on top of the UI.
    The overlay is compiled in for Debug builds only (GUI_DEBUG_OVERLAY); in a Release
    build set_layers is a no-op and get_layers returns 0.  These constants stay defined in
    every build so call sites compile unchanged.
==============================================================================================*/

typedef enum
{
    GUI_DBG_NONE     = 0,         // overlay off
    GUI_DBG_WINDOW   = 1 << 0,    // window outer frames; the hover window stands out
    GUI_DBG_INTERACT = 1 << 1,    // per-widget interaction rects (hover/active tinted)
    GUI_DBG_RESIZE   = 1 << 2,    // window edge-resize grab bands; hot when armed
    GUI_DBG_CLIP     = 1 << 3,    // clip (scissor) rectangle stack, colored by depth
    GUI_DBG_LAYOUT   = 1 << 4,    // layout allocated space per widget
    GUI_DBG_CONTENT  = 1 << 5,    // measured content rect per scrollable region -- drawn
                                  //   in the MAIN list so it scrolls with its content
    GUI_DBG_REGION   = 1 << 6,    // per-region screen geometry: view rect, reserved
                                  //   scrollbar gutters, and the body's interaction clip

    GUI_DBG_ALL      = GUI_DBG_WINDOW | GUI_DBG_INTERACT | GUI_DBG_RESIZE | GUI_DBG_CLIP | GUI_DBG_LAYOUT
                     | GUI_DBG_CONTENT | GUI_DBG_REGION,

} gui_dbg_layer_t;

/*==============================================================================================
    GUI_DEBUG -- render mode

    How the main UI draw list is rasterized, selected via gui()->debug_set_render_mode().  Unlike
    the debug overlay layers (a separate draw list painted on top), this changes the rasterization
    of the UI itself, so the two are independent and compose.  Available in every build (it is just
    a pipeline + push-constant switch, cheap enough to leave in Release).

      NORMAL    -- textured, blended UI (the default).
      WIREFRAME -- the geometry's triangle edges (VK_POLYGON_MODE_LINE), each window keeping its own
                   color: a direct read on how many triangles a shape costs.
      BATCH     -- every GPU draw call (one indexed draw == one batch) is tinted a distinct color, so
                   a color change marks a batch split -- count the colors to count the batches.
==============================================================================================*/

typedef enum
{
    GUI_RENDER_NORMAL    = 0,   // normal textured / blended UI
    GUI_RENDER_WIREFRAME = 1,   // triangle edges only (wireframe)
    GUI_RENDER_BATCH     = 2,   // per-draw-call color tint (batch boundary view)

    GUI_RENDER_MODE_COUNT,      // mode count -- not a mode

} gui_render_mode_t;

/*============================================================================================*/
#endif    // GUI_H
