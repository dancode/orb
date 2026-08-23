#ifndef GUI_H
#define GUI_H
/*==============================================================================================

    runtime_service/gui/gui.h -- gui module types (the public type header).

    This file has no functions, just definitions -- the shape of every "noun" the GUI uses.

    GUI is ORB's in-house immediate-mode 2D interaction renderer, built from two servers
    that never see each other:

    1. INTERACT SERVER: that tracks what the user is doing (hover, click, focus, drag).
    2. RENDER SERVER:   that turns draw commands into GPU triangles.

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

/* Shape handle -- identifies one piece of authored art baked into the SDF atlas as a signed
   DISTANCE FIELD rather than as coverage (register_shape).  What that buys is the whole effect
   cascade: a shape is a FIELD SOURCE, so the same border, glow, inset, swell, pulse and
   subtraction that reach a rounded box reach a keyhole, a gear or a badge silhouette -- the
   silhouettes no closed-form field and no single record-borne cut can describe.

   The difference from an SDF ICON, which is also a field in the same atlas, is where it resolves:
   an icon's texel is read in the colour stage as alpha, so only an outline can be drawn from it,
   where a shape's texel becomes the fragment's distance before a single op has run.

   Effects reach exactly as far as the bake padded (gui_shape_bake_t.spread) and no further -- past
   that the stored field saturates and the effect stops rather than fading.  shape_reach reports
   that limit in px for a given rect.  0 means "no shape", and every draw helper no-ops on it. */

typedef u32 gui_shape_id_t;
#define GUI_SHAPE_NONE 0u

/* Where a shape bake gets the margin its field needs.  All three run one path -- crop to the ink,
   add margin, downsample -- and differ only in what gui_shape_bake_t.out_max names. */

typedef enum
{
    GUI_SDF_PAD_GROW = 0,   /* out_max is the INK's longest edge; the tenant comes out 2 * spread
                               larger.  The default: the caller states the art size it wants and
                               the bake pays for the reach out of atlas area.                    */
    GUI_SDF_PAD_KEEP,       /* out_max is the TENANT's longest edge and the source is used
                               UNCROPPED -- the import path for art authored with its own spacing.
                               The margin is measured rather than made, and a short one caps the
                               effective spread (reported back, so shape_reach cannot overstate). */
    GUI_SDF_PAD_INSET,      /* out_max is the TENANT's longest edge; the ART shrinks inside it to
                               make room.  Costs ink resolution, spends no extra atlas.          */

} gui_sdf_pad_t;

/* How to bake one shape.  A NULL descriptor at register_shape takes every default. */

typedef struct
{
    u32           out_max;  // stored longest edge, texels (0 = GUI_SHAPE_SIZE_DEFAULT)
    f32           spread;   // texels of field either side of the outline (0 = GUI_SHAPE_SPREAD).
                            //   The reach every effect is bounded by, and the quantization it
                            //   trades against: a byte holds 127 levels over the spread, so 8
                            //   leaves ~16 across the 1 px AA band and 16 would leave 8 and band
    gui_sdf_pad_t pad;      // where the margin comes from

} gui_shape_bake_t;

#define GUI_SHAPE_SIZE_DEFAULT  128u
#define GUI_SHAPE_SPREAD        8.0f

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

/* Runtime font baker (see font_baker_set) -- resolve "this typeface at this pixel size" to a
   baked .orb_font on disk.  gui asks it for the type-ramp sizes (GUI_VAR_TYPE_SMALL / _LARGE)
   it has no shipped bake for; `family` is a source name a baker like dev_font_get resolves (a file in
   assets/font_source or an OS-installed face name), size_px is final (DPI already applied).
   Write the absolute path into out_path and return true; false = cannot bake (gui records the
   failure once per size and leaves that role at the body size). */
typedef bool ( *gui_font_bake_fn )( const char* family, u32 size_px,
                                    char* out_path, int out_path_size, void* user );

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
    instances of one schema rather than two schemas.  There is no flat color enum: ten roles
    times four phases ARE the 40 cells of gui_style_t.col below.

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

/* What the color is FOR.  Ten roles cover every surface the GUI paints.

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
   child region steps off it AT REST and still needs its own standing-based HOT/ACTIVE -- a drop
   target and a scoped focus are as real for a child as for the window it lives in, independent of
   the window's own reading (child_standing_phase, flow/gui_layout_child.c).  Folding "recessed"
   into PANEL's INERT cell, as a single early build did, cost every child region that reading
   entirely -- INERT is one cell, not a phase, so a recessed panel could never also show as hot.
   Split, PANEL's INERT cell
   goes back to being the one thing every other role's INERT cell already was -- a non-interactive
   surface's look, permanent (gui_stock_panel's decorative backdrop, an empty dock-leaf
   placeholder) or temporary (a window fenced off by an active modal -- see the phase table below)
   -- and PANEL_CHILD carries its own full IDLE/HOT/ACTIVE/INERT ramp, seeded from a nested
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
    CHILD    nested surface    valid drop target    focus inside child     behind modal fence
    TITLE    bar, inactive tab chip hov / drop      focused bar, live tab  de-emphasized bar

    BG       control face      hovered face         pressed / focused      plot backdrop
    BORDER   frame line        hovered / resize     focused window ring    subdued frame
    TEXT_PRI body text, caret  unused               unused                 disabled-text ink
    TEXT_SEC secondary text    unused               unused                 unused
    ACCENT   value fill        engaged fill         dragged fill           empty track
    MARK     check, radio dot  nav ring             captured-nav ring      unused
    GRAB     knob / thumb      hovered knob         dragged knob           unused

   PANEL/HOT, CHILD/HOT and TITLE/HOT (the band, not the chip) share one formula: a wash toward
   the GUI_EXT_DROP hue, read while a drag gesture is in flight and this surface is the computed
   landing target -- a frame-level fact, not a cursor-over-pixel one, which is why it stays IDLE
   under an ordinary mouse-over.  Two independent gestures feed it, and a surface can answer to
   either without knowing which: PANEL and the TITLE band also read window_route_is_drop_target
   (chrome/dock/gui_dock_route.c), true only for a window being title-dragged over a dockspace --
   no opt-in needed, the window genuinely IS the target there.  The other path is a generic
   drag_source_begin payload (interact/gui_drag.c) hovering a window or child opened with
   GUI_WIN_DRAG_TARGET (gui.h) -- explicit, on purpose: most windows and children are scenery a
   drag happens to pass over on its way to a widget target inside them (a specific list row, a
   colour swatch), and those widgets already ring on their own via drag_payload_accept
   (draw_drop_ring) the moment they call drag_target_begin, with or without this flag anywhere
   above them.  GUI_WIN_DRAG_TARGET is for the coarser case -- a reorderable list whose body
   accepts a drop anywhere in it, not just on an existing row -- so the CALLER decides which
   containers read as "you can drop somewhere in here" instead of every window a drag happens to
   cross lighting up.  A tab CHIP (col_tab_bg) is the one place under
   TITLE that still reads HOT as plain cursor hover, same as every other role: it is a small,
   individually-hoverable target the way a button is, unlike the band it sits on.

   PANEL/ACTIVE, CHILD/ACTIVE and TITLE/ACTIVE all read "the keyboard cursor is scoped to this
   surface", by different means: PANEL and CHILD both lift their ground a faint step so the eye
   can find the live surface without the fill competing with the content painted over it -- PANEL
   reads the window's own focus (nav.focused_win), CHILD reads whether the focused widget is
   scoped to THIS child specifically (s_interaction.focused_win, which pane_tag stamps to the
   child's id on entry, not the enclosing window's) -- while TITLE/ACTIVE is authored as the full
   lifted, accented band (the same one HOT washes further): the focused window's bar, or a live
   tab, is meant to be the vivid one that draws the eye first, with TITLE/IDLE the bare ground so
   an unfocused window or a background tab recedes instead of outshining it.  BORDER/ACTIVE
   carries the strongest version of the window-level fact (a full focus ring) -- three surfaces,
   one signal, weighted so the ring does the convincing and the other two do not have to fight it
   for attention.

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
   hover feedback (a selected row could never also show as hovered).  Selection is a live wash
   over whatever cell resolved -- see GUI_STYLE -- SELECTED below. */

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

    The grid above is what a RENDER reads. It is not what a theme WRITES. 40 literals would
    be the wrong authoring surface: many cells are structurally redundant with each other
    (TEXT_PRIMARY is one colour across most phases, a role's HOT and ACTIVE cells usually
    sit close together, an inert cell is usually its neighbour role's base) and a theme
    that hand-typed all 40 would restate the same relationships dozens of times with nothing
    to keep the restatements in sync -- one literal edited and its echoes elsewhere quietly
    drift off the ramp.

    So a theme authors SEVEN colours and SIX numbers, and gui_style_bake derives the
    full 40 cells based on those 13 values.

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
        gui()->style_bake( e );                         // 40 cells re-derive

        e->col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE ] = ember;   // one bespoke cell
    
    Alpha rides through: a seed's alpha byte is carried onto every cell derived from it, so a
    translucent panel seed yields a translucent panel in all four phases without four literals.
==============================================================================================*/

/* The source colours.  One per surface KIND, which is a coarser axis than the role -- PANEL,
   PANEL_CHILD and TITLE are all the container surface, so all derive from SURFACE and the ramp
   separates them.  Seven seeds cover ten roles because TITLE and PANEL_CHILD have no colour of
   their own: a caption band is a lifted surface and a child region a nested one --
   derivations, not decisions.  The severity hues used to live here too
   (a severity ladder is a set of independent editorial choices no derivation can guess from an
   accent) but they were roles wearing a ramp they never used -- see GUI_ROLE_COUNT above and
   gui_style_ext_t below, where they live now as flat, unramped colours instead. */

typedef enum
{
    GUI_SEED_SURFACE = 0,   // container base: window body, panel, and the band over it
    GUI_SEED_CONTROL,       // control face base: button, input field, check box, track
    GUI_SEED_INK,           // text base -- every glyph and the caret
    GUI_SEED_LINE,          // frame line base: borders, rules, resize edges
    GUI_SEED_ACCENT,        // THE hue: value fills, selection wash, press wash, focus ring, nav highlight
    GUI_SEED_MARK,          // the affirmative indicator hue: check, radio dot
    GUI_SEED_GRAB,          // the contrast anchor: knobs and thumbs, opposite the theme

    GUI_SEED_COUNT

} gui_style_seed_t;

/* HOW FAR a derived cell travels -- the theme's personality, in seven numbers, and the index into
   gui_palette_t.ramp.  Authored per theme rather than fixed, because a step that reads as one
   notch on a near-black surface reads as four on a near-white one: the light and dark built-ins
   carry visibly different sink values for exactly that reason.  A ramp of all zeroes bakes a
   flat, unreactive UI -- a legitimate look, and a useful debugging one.

   All are 0..1 except NEST, the one SIGNED entry, which also carries a direction -- see its
   comment below and bake_nest in style/gui_bake.c.

   An array rather than named fields, for the same reason gui_style_t.var is one: the enum IS
   the field list, so a style editor walks the ramp with no table of its own. */

typedef enum
{
    GUI_RAMP_HOVER = 0,   // accent tinge on a hovered control face -- a whisper: hover is a lift, not a hue change
    GUI_RAMP_PRESS,       // how far a pressed face washes toward the accent -- the deeper, engaged wash
    GUI_RAMP_FADE,        // how far an inert cell fades toward the surface (the INERT phase)
    GUI_RAMP_RECESS,      // how deep a HOLE cuts below its base: the empty track, the modal-fenced panel
    GUI_RAMP_NEST,        // SIGNED, -1..1: one rung of the surface ladder for a nested region --
                          // positive sinks it toward black, negative lifts it toward the pole
    GUI_RAMP_STEP,        // one lift notch for the accent, border and anchor ramps
    GUI_RAMP_SELECT,      // how far a CHOSEN surface washes toward the accent (style_wash_selected)
    GUI_RAMP_COUNT

} gui_style_ramp_t;

/* GUI_STYLE -- the EXTENDED palette: flat, unramped colours a theme authors and a caller reaches
   by name, for a signal that is a standing fact rather than an interaction state -- no phase, no
   bake, no per-set derivation cost.  The reserved slots below are the severity ladder that used
   to live in GUI_ROLE_INFO/OK/WARN/ERROR, plus the instrument colours; a kit registers its own
   beyond them at runtime with gui_style_ext_add (style/gui_style.h), mirroring
   gui_style_brush_add's per-set pool exactly -- a handle only means something inside the set
   that issued it, and costs that set alone.

   style_ext( id ) is the resolved read; push_style_ext / pop_style_ext override a slot for a
   scope exactly like push_style_var does, since there is no ramp to re-derive on push -- a flat
   value swap is the whole operation. */

typedef enum
{
    GUI_EXT_INFO = 0,   // status hue: a neutral notice
    GUI_EXT_OK,         // status hue: healthy / passing
    GUI_EXT_WARN,       // status hue: near a limit
    GUI_EXT_ERROR,      // status hue: failed / over a limit
    GUI_EXT_DROP,       // instrument hue: "a drop can land here" -- every drag-and-drop cue
                        // (dock overlay, drop hint / ring, PANEL / CHILD / TITLE HOT wash)
    GUI_EXT_SHADOW,     // instrument hue: the elevation shadow under floating chrome -- black at
                        // the theme's chosen alpha (alpha 0 = a theme with no shadows at all)

    GUI_EXT_RESERVED_COUNT   // the engine-authored slots -- part of every theme's palette

} gui_style_ext_t;

/* Slots one style set can hold, reserved included.  Small on purpose, like the brush pool: a
   kit needing more named colours than this is describing per-widget tokens, not a theme. */
#define GUI_STYLE_EXT_MAX 16u

/* The authored half of a style, in full: seven colours, six numbers and six extended colours, 76
   bytes.  Small enough that a theme is worth having dozens of, or deriving live from a single
   accent the user picked. */

typedef struct gui_palette_s
{
    u32 seed[ GUI_SEED_COUNT ];             // the source colours
    f32 ramp[ GUI_RAMP_COUNT ];             // how far each derivation travels
    u32 ext [ GUI_EXT_RESERVED_COUNT ];     // the standard extended-palette colours (severity + instruments)

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
    GUI_VAR_SHADOW,         // elevation-shadow feather under floating chrome (px; overlays widen it); 0 = flat
    GUI_VAR_FOCUS_RING,     // keyboard ring stroke, drawn inward from the item edge (px); 0 = none
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
    GUI_VAR_CORNER_SMOOTH,  // how much a rounded corner's curvature RAMPS instead of starting at
                            //   the arc: 0 = a circular arc (the default), 1 = the corner fills
                            //   out toward the square it is inset from.  Applies wherever a radius
                            //   already does -- it changes the profile, never the radius.

    /* 4. RATE -- how fast an item travels between cells, in Hz-like damper speed (10 ~ 250 ms to
       95%, 20 ~ 150 ms).  These are the whole motion budget of the widget set: every surface,
       border and ink that animates reads its speed from one of the three, so a theme sets the
       FEEL of the entire UI in three numbers -- and setting them to 0 makes the whole library
       snap, which is the accessibility answer and the "I hate animation" answer at once. */

    GUI_VAR_ANIM_HOT,       // rate the hover / nav highlight fades in and out
    GUI_VAR_ANIM_ACTIVE,    // rate the pressed state fades -- faster: a press must feel immediate
    GUI_VAR_ANIM_SELECT,    // rate a selection / toggle crosses to the SELECT plane
    GUI_VAR_ANIM_SIZE,      // rate a MEASURED extent eases to a new size (natural track, box height)

    /* 5. TYPE -- the type ramp's role sizes, authored ABSOLUTE like the scale ramp's rows
       ("small IS 10 at em 12"), not as offsets from the body.  Em-scaled but never
       lattice-snapped: a font size must track the body type, and a lattice snap would
       double a small size difference on the default quantum.  Each role is its own opt-in:
       0 (the default) = that role off, chrome and type_push fall back to the body font. */

    GUI_VAR_TYPE_SMALL,     // px (at em=12) the SMALL type role renders at; 0 = role off
    GUI_VAR_TYPE_LARGE,     // px (at em=12) the LARGE type role renders at; 0 = role off

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
    TYPE     yes          no                 a font-size delta -- tracks the body em, never snaps
    
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
    GUI_CLASS_TYPE,
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
       col[][] below from the seeds (the extended colours are copied straight through -- there
       is nothing to derive).  First in the struct because it is first in the pipeline, and
       because push_style_seed addresses a seed by slot exactly as push_style_var addresses a
       var. */
    gui_palette_t palette;

    /* SKIN: the 10x4 color grid -- THE color vocabulary (gui_style_role_t x gui_style_phase_t,
       above), and the DERIVED half: gui_style_bake writes all 40 cells from the palette, then a
       kit may overwrite any of them.  GUI_COLOR packs R,G,B,A bytes; a cell is read with
       style_col( role, phase ).  There is no SELECTED plane here -- style_col_selected washes a
       resolved cell toward the accent live, rather than reading a second stored one; see the
       SELECTED section above. */
    u32 col[ GUI_ROLE_COUNT ][ GUI_PHASE_COUNT ];

    /* SKIN: the FACE plane -- the same 10x4 grid again, but a cell here holds a HANDLE, not a
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

/* The TYPE RAMP roles -- the official size variations UI is authored against, the type
   analogue of the scale ramp's density steps.  Each role's size is authored in the style
   (GUI_VAR_TYPE_SMALL / _LARGE, absolute px at em=12) and is its OWN opt-in: 0 leaves that
   role off.  NORMAL is the body em itself -- never authored, never off, always the fallback.
   Chrome consumes the roles automatically when they are on (SMALL for hints / shortcuts /
   table headers, LARGE for window titles / section headers).

   gui_type_push / gui_type_pop bracket ONE SCOPE's measure + draw with a role: both the
   measurement readers (font_text_w, text_center_y) and the TEXT command stamp switch to the
   role's font inside; layout metrics and the style never move, so cells stay body-sized.  A
   role that is off or could not resolve (no baker installed, bake failed) falls through to
   the body font -- authoring against a role is always safe.  To pair a role with a density
   step in one declaration, see scale_push_font (the scale ramp itself stays whitespace-only). */

typedef enum
{
    GUI_TYPE_NORMAL = 0,   // the body font -- push is a saved no-op
    GUI_TYPE_SMALL,        // the authored small size (floored at readability)
    GUI_TYPE_LARGE,        // the authored large size

} gui_type_role_t;

void         gui_type_push( gui_type_role_t role );
void         gui_type_pop ( void );

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
    GUI_DRAW -- the record layer (gui_prim.h)

    The types the renderer moves across the CPU/GPU boundary -- the effect-band field enum
    (gui_fx_mode_t), the GUI_OP_* modifier bits, the prim record (gui_prim_t), the quad and
    instance records (gui_quad_t, gui_fx_t) and their packing helpers -- live in gui_prim.h.
    They are the contract between the tessellator and the shaders, not drawing API: callers
    draw through gui_api.h's draw_* verbs and never build a record.
==============================================================================================*/

#include "runtime_service/gui/gui_prim.h"

/*==============================================================================================
    GUI_DRAW -- animation curves + gradient kinds (the caller-facing spellings)

    The two enums a CALLER passes for what the record layer carries as bits: a curve shapes
    every animating draw verb (draw_set_anim_curve), a gradient kind picks a ramp's geometry
    (draw_round_rect_gradient).
==============================================================================================*/

/* The SHAPING stage of the animation clock (gui_prim.h, the record's anim lanes): what a
   normalized phase 0..1 does
   between its endpoints.  It sits between the timebase and the effect, so one curve bends whatever
   the record animates -- a spin, the marching ants, a pulse -- and any two of them driven by one
   phase stay in step.  Every curve rises from 0 at phase 0 except DECAY, which is the flash.

   `param` belongs to the curve, the way anim_rate's unit belongs to its op; the curves that name
   no param ignore it. */
typedef enum
{
    GUI_CURVE_LINEAR = 0,   // k = phase.  The sawtooth: ramp, snap back.  Ambient default.
    GUI_CURVE_SINE,         // raised cosine, 0 -> 1 -> 0.  The breath every pulse had before
                            //   curves existed, and what a pulse still gets when none is named.
    GUI_CURVE_TRIANGLE,     // linear out and back -- the sine's cheap, sharper twin
    GUI_CURVE_SMOOTH,       // smoothstep: ease in AND out, still ending where it started+1
    GUI_CURVE_EASE,         // pow( phase, param ).  param > 1 eases in, < 1 eases out, 1 = linear
    GUI_CURVE_STAIR,        // param steps of equal height -- the mechanical clock-hand spinner
    GUI_CURVE_SQUARE,       // holds 0 for the first `param` of the cycle, then 1: the blink
    GUI_CURVE_DECAY,        // exp( -param * phase ).  The one curve that starts at 1 and falls.
                            //   Under PULSE, which SUBTRACTS k from coverage, that reads as
                            //   fading IN; pair it with a target k drives upward, or use EASE
                            //   with param < 1 for a pulse that flashes and settles.
} gui_curve_t;

/* Which way a ramp runs, as a draw parameter.  The record carries it as GUI_OP_GRAD_* bits
   (gui_prim.h); this is the spelling a caller uses, where "at most one" is a property of the
   type rather than a rule. */
typedef enum
{
    GUI_GRAD_LINEAR = 0,   // along `angle`, spanning the shape's extent on that axis
    GUI_GRAD_RADIAL,       // centre -> rim, against the shape's own half-extent

    /* Angular and MIRRORED about `angle`: col_b sits on that axis, the fill's own colour at the
       far side, and the ramp is the same going either way around.  Not a wrapping sweep -- that
       meets itself at the axis as a hard light/dark edge.  This is the angular SHEEN across a
       knob, a badge, a dial face.  For a sweep that measures a VALUE, reach for a gradient arc
       (draw_arc_gradient), whose shape is the sweep. */

    GUI_GRAD_CONIC,

} gui_grad_t;

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
    (gui_render.c) tessellates each command into quad records at flush time.  This separates
    the UI logic from any graphics API knowledge.

    GPU draw commands (tess_gpu_cmd_t) are a backend-private type defined in
    gui_build_tess_state.c; they carry a quad range and diagnostic state for one GPU draw call.
==============================================================================================*/

typedef enum
{
    GUI_CMD_RECT_FILL,       // solid-colour filled rectangle; rounding > 0 makes it an SDF
                             //   surface -- a filled DISC is this command at radius ==
                             //   half-extent (draw_push_circle_filled), not a type of its own.
                             //   HOT: inline in gui_cmd_t, the most common command in the list
    GUI_CMD_RECT_TEX,        // textured quad: an image, an icon, a glyph's SDF sprite -- same
                             //   shape as RECT_FILL but never solid, so its uv/tex_idx fields
                             //   would sit unused on every plain fill.  COLD: gui_cmd_ext_t
    GUI_CMD_RECT_OUTLINE,    // hollow rectangle: four edge quads (a BOX under GUI_OP_BAND
                             //   when rounded)
    GUI_CMD_TRIANGLE,        // solid triangle
    GUI_CMD_BEZIER,          // stroked quadratic bezier, round caps: one GUI_FX_BEZIER quad
    GUI_CMD_TEXT,            // glyph run from the font atlas.  HOT: inline in gui_cmd_t, the
                             //   other high-volume command alongside RECT_FILL
    GUI_CMD_TEXT_XF,         // glyph run under a uniform scale + a rotation about its origin
    GUI_CMD_TEXT_SHADOW,     // glyph run plus an offset copy, both emitted from one string walk
    GUI_CMD_LINE,            // single stroke segment
    GUI_CMD_POLYLINE,        // multi-segment antialiased polyline
    GUI_CMD_DASHED_LINE,     // patterned line: one textured quad, atlas dash row, tiled by U
    GUI_CMD_RECT_GRADIENT,   // filled rect, col_a->col_b ramped in the fragment (GUI_OP_GRAD)
    GUI_CMD_RECT_LIST,       // batch of solid rects from the per-frame rect pool (one cmd, N quads)
    GUI_CMD_SPRITE,          // RGBA sprite quad; nine-slice expanded at tessellation when the
                             //   sprite carries slice insets (1, 3 or 9 quads from one command)
    GUI_CMD_FX_BOX,          // the parameterized GUI_FX_BOX surface: a soft shadow (wide feather)
                             //   or a shader-clock pulse (rate/depth) -- one member, mode derived
    GUI_CMD_ROUND_RECT_EX,   // filled box with a PER-CORNER radius: one GUI_FX_BOX quad carrying
                             //   all four radii, the fragment picking one by the sign of its position
    GUI_CMD_ARC,             // stroked circular arc, round caps: one GUI_FX_ARC quad
    GUI_CMD_PIE,             // filled wedge, sharp radial edges: one GUI_FX_PIE quad
    GUI_CMD_ARC_DASH,        // arc cut by an angular dash pattern (GUI_FX_ARC + GUI_OP_DASH): dotted rings,
                             //   marching ants, tick dials -- still one quad
    GUI_CMD_ARC_GRAD,        // arc whose colour sweeps col_a -> col_b along the sector
                             //   (GUI_FX_ARC + GUI_OP_GRAD_ALONG): the hot/cold value arc --
                             //   still one quad
    GUI_CMD_IMAGE_XF,        // textured quad under a rotation about its centre: rotated icons,
                             //   images, markers -- the text_xf treatment for one quad
    GUI_CMD_CHECKER,         // two-colour cell pattern resolved in the FRAGMENT (GUI_OP_CHECKER):
                             //   the transparency backdrop as ONE quad, versus the 64 commands /
                             //   4096 quads a rect-pool expansion would cost at its 64x64 clamp
    GUI_CMD_GRID,            // line lattice (GUI_OP_GRID): graph-paper / node-graph backdrop in
                             //   one quad; the lattice anchors to (ox, oy) so it pans with content
    GUI_CMD_NGON,            // regular polygon (GUI_FX_NGON): filled or stroked, corners rounded
                             //   by the ambient rounding -- one quad, exact at any size
    GUI_CMD_BOX_DASH,        // rounded-box outline cut by a perimeter dash (GUI_OP_DASH): the
                             //   dashed border, and at a non-zero scroll rate the marching ants
    GUI_CMD_FRAME,           // filled body + border band composited in the FRAGMENT
                             //   (GUI_OP_FRAME): the widget bezel as ONE quad
    GUI_CMD_REPEAT,          // nx by ny copies of one rounded cell (GUI_OP_REPEAT): dot grids,
                             //   tick strips, segmented bars -- one quad whatever the count
    GUI_CMD_REPEAT_POLAR,    // n copies on a circle (GUI_OP_REPEAT_POLAR): dot rings, dial faces,
                             //   and with SPIN a rotating spinner that re-tessellates nothing
    GUI_CMD_BOX_CUT,         // rounded box minus a SECOND rounded box (GUI_OP_CUT_SHAPE): the
                             //   notched avatar, the ticket silhouette -- subtraction, one quad

    GUI_CMD_COUNT,           // sentinel: number of command types, not a type itself

} gui_cmd_type_t;

/* Sentinel half-extent for an unclipped text command: any real glyph sits well inside this, so
   the tessellator's clip test never triggers and the whole-run fast path is taken. */

#define GUI_TEXT_NO_CLIP 1e30f

/* THE SAMPLING MODEL -- the top 4 bits of a tex_idx.  What a texel MEANS to the fragment: 
   the one axis the shader branches on, and the axis the two atlases are already split along
   (render/resource/gui_res_atlas.h).

   It rides the tex_idx rather than taking a field of its own because it is a property of the
   TEXTURE, not of the shape drawn with it -- so wherever the slot goes, the model goes with it
   for free.  
   
   The SAMPLER is DERIVED from the mode in the fragment and never carried: coverage must stay
   point-sampled or glyphs stop being crisp, colour must filter or it blocks up the moment it
   is stretched.

   That derivation is the whole reason this is a MODE rather than a bool: a third sampling 
   model (SDF) is one more value, not a format change.  Three of the sixteen are spent; the rest stay
   unnamed until something emits them.

   The word travels from the emit site into the primitive record's `tex` member (gui_prim_t), and
   the model's shift is the one bit-layout contract the shaders still share with this header --
   TEX_MODE_SHIFT in gui_fx.hlsli.  The clip band,
   the self bit and the op band that used to sit under it are gone: all three are plain members of
   the record now, so the low 28 bits are the bindless index and nothing else. */

#define GUI_TEX_MODE_SHIFT  28u
#define GUI_TEX_MODE_MASK   ( 0xFu << GUI_TEX_MODE_SHIFT )
#define GUI_TEX_MODE( m )   ( (u32)( m ) << GUI_TEX_MODE_SHIFT )

typedef enum
{
    GUI_TEX_COVERAGE = 0,   /* R8: the texel's R is alpha and the vertex colour supplies RGB --
                               glyphs, icons, the drawing assists; a solid fill sets GUI_OP_SELF
                               and skips the fetch entirely, so it carries no real texel here */
    GUI_TEX_RGBA     = 1,   /* RGBA: the texel IS the colour and the vertex colour tints it --
                               sprite art, and a caller's own texture via draw_texture_in        */
    GUI_TEX_SDF      = 2,   /* R8 SIGNED DISTANCE: 128 is the outline, and the fragment recovers
                               coverage from the screen-space derivative rather than the texel.
                               That is what lets distance-field text scale and rotate cleanly --
                               and why it must filter, which is why it could not be a COVERAGE
                               font wearing a different flag (orb_font.h, sdf_range)             */
} gui_tex_mode_t;

/* Split a tex_idx into its parts: the model, and the bindless slot to sample. */

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

/*==============================================================================================
   Command Structure

   One semantic draw command is an 8-byte index record (gui_cmd_t): the command type, the index
   of the active clip rect in the per-frame clip table (assigned at clip-push time), the target
   viewport, whether that clip rect is empty (baked at the same time, so build passes can skip
   a command without touching clip_table), and a byte offset into s_draw.cmd_pool where the
   command's actual payload lives.

   The payload pool holds every command at its own natural size (see gui_cmd_ext_t).

   - A `tri` costs what a tri costs, an `fx_box` costs what an fx_box costs.
   - The k_cmd_hash_len[] is the single source of truth for each type's payload size.
   - Used both to claim pool space at emit time and to fold the retained-cache hash.
   - A type missing from that table asserts rather than silently under-hashing.

   * The z lives in gui_cmd_seg_t (per-segment, constant within a window).
   * tex_idx == 0 in rect means solid color: tessellation rewrites it to res_atlas_idx() for
     batching and sets GUI_OP_SELF, which tells the fragment shader to skip the texture fetch
     and fill white in code -- no atlas texel is ever sampled for this case.

   rounding (rect / rect_outline) is the corner radius baked from the ambient draw rounding at emit
   time, already clamped to the rect; 0 tessellates as a plain square shape.  corner_pow rides
   beside it on every command a radius can reach, baked from the ambient profile the same way
   (0 = circular); it is the exponent the record carries, not the 0..1 amount a caller authors.
   text.off is a byte offset into the frame's text pool (s_draw.text_pool), not a pointer: the
   string lives in the pool until the next frame_begin, so the command is valid through flush.
   Storing an offset instead of a const char* keeps every payload shape 4-byte aligned.

==============================================================================================*/

/* The fx_box command's `variant`: which fill the one soft-box geometry resolves as.  The
   tessellator maps each to the op it stamps (gui_build_tess_dispatch.c). */

#define GUI_FX_BOX_FILL   0u   // the filled surface -- shadow / glow meant to show through
#define GUI_FX_BOX_SKIRT  1u   // interior cut away (GUI_OP_CUT) -- the drop shadow
#define GUI_FX_BOX_INSET  2u   // falloff turned inward (GUI_OP_INSET) -- the inner shadow
#define GUI_FX_BOX_GLOW   3u   // exponential falloff (GUI_OP_GLOW) -- light, not blur
#define GUI_FX_BOX_RING   4u   // bent into a border band (GUI_OP_BAND) -- the ripple's ring

/* Every command's payload lives in one pool (s_draw.cmd_pool), addressed by byte offset -- see
   gui_cmd_payload().  No type is inlined here: the envelope is the same 8 bytes whether the
   command is a plain fill or an fx_box, and the pool holds each payload at its own natural size
   instead of every command paying for the widest type in the union. */

typedef struct
{
    u8  type;        // gui_cmd_type_t, fits u8 (GUI_CMD_COUNT values)
    u8  clip_idx;    // index into per-frame s_draw.clip_table (set at push time)
    u8  vp;          // target viewport (GUI_MAX_VIEWPORTS = 4)
    u8  clip_empty;  // baked at push time: s_draw.clip_table[clip_idx] bounds no pixels, so this
                     // command paints nothing -- build passes skip it without touching clip_table
    u32 offset;      // byte offset into s_draw.cmd_pool, see draw_cmd_ext_slot()

} gui_cmd_t;

/* Every command type's payload shape, addressed by byte offset from its command's `offset`
   field -- see draw_cmd_ext_slot().  One pool holds every type at its own natural size: a `tri`
   costs what a tri costs, an `fx_box` costs what an fx_box costs, and neither pays for the
   other's width. */

typedef union
{
    struct { f32 x, y, w, h; f32 rounding, corner_pow; u32 abgr; } rect_fill;

    /* clip_x0/clip_x1 are the horizontal pixel window for glyph-level clipping: the first and
       last straddling glyphs are cut and their U remapped; interior glyphs emit whole.  The
       sentinel (clip_x0 = -GUI_TEXT_NO_CLIP, clip_x1 = +GUI_TEXT_NO_CLIP) means unclipped
       and takes the original whole-run fast path. */

    /* edge_w / edge_col are the ambient TEXT_EDGE at emit time (width 0 = none): a second
       colour painted outside the glyph boundary, resolved by the fragment from the SAME quad,
       so an outlined or shadowed run costs no extra geometry, no second pass, and no batch
       split.  They ride the command rather than being re-read at tessellation because the
       ambient can have moved on by then -- a retained window re-tessellates long after its
       emit. */

    /* font is the registry id whose glyph metrics and atlas UVs this run resolves from -- the
       ONLY thing the font decides.  It rides the command rather than the command SEGMENT
       because a segment is the backend's batch-dispatch unit and the font is not a batch key:
       every font packs into one shared atlas, so a font change moves no texture, and even
       when it does the texture rides the vertex and cannot cut a draw call either.  Tagging a
       batch unit with a per-command property would force a segment split just for a lookup --
       see draw_set_font. */

    struct { f32 x, y; u32 off; u32 len; f32 clip_x0, clip_x1; u32 abgr;
             f32 edge_w; u32 edge_col; u16 font; } text;

    /* The textured/image case of what RECT_FILL covers for a solid fill -- an icon, a picture,
       a glyph's SDF sprite.  Split out because its uv/tex_idx fields would sit unused on every
       plain fill, the hot path every chrome widget goes through. */
    struct { f32 x, y, w, h, u0, v0, u1, v1; f32 rounding, corner_pow; u32 tex_idx; u32 abgr; } rect_tex;
    struct { f32 x, y, w, h, t;              f32 rounding, corner_pow;              u32 abgr; } rect_outline;

    /* Widget bezel: the filled body and its border band in one command, resolved by the
       fragment as a single quad (GUI_OP_FRAME).  `t` is the band's width, lying inside the
       boundary -- the emit site falls back to the fill + outline pair when the ambient
       border alignment pushes the band outward, so this member never carries an align. */
    struct { f32 x, y, w, h, t;              f32 rounding, corner_pow;  u32 abgr, col_border; } frame;
    struct { f32 ax, ay, bx, by, cx, cy;                     u32 abgr; } tri;
    struct { f32 ax, ay, cx, cy, bx, by, thickness;          u32 abgr; } bezier;

    /* A drop shadow rides its own command rather than a dx/dy/shadow_abgr feather on `text`,
       for the same reason `text_xf` does: those fields would sit unused on every plain label,
       the hot path every chrome widget goes through. Unlike text_xf, the run stays 1:1 and
       axis-aligned -- only the glyph loop changes, emitting the shadow copy (dx, dy offset,
       shadow_abgr) and the main copy per glyph from one decode + one atlas lookup, instead of
       draw_text_shadow's two full commands walking the string twice. */
    struct { f32 x, y;  u32 off; u32 len;  f32 clip_x0, clip_x1;  u32 abgr, shadow_abgr;
             f32 dx, dy;  u16 font; } text_shadow;

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
    struct { f32 x, y;  u32 off; u32 len;  f32 scale, rot;        u32 abgr;
             f32 edge_w; u32 edge_col; u16 font; } text_xf;

    /* A stroke segment, resolved as a CAPSULE by the fragment (round caps, exact at any
       angle).  `border` > 0 hollows it into a tube of that width lying inside the capsule
       boundary -- GUI_OP_BAND, the same op that turns a filled box into a rounded outline,
       which reaches this shape for free because an op modifies whatever field arrived.
       gui_draw_line pushes border 0 and only for diagonals; gui_draw_capsule* pushes at any
       angle, since a snapped rect has square caps and is not the same shape. */
    struct { f32 x0, y0, x1, y1, thickness, border;          u32 abgr; } line;
    struct { u32 pt_offset; u32 pt_count; f32 thickness;
             gui_stroke_align_t align; bool closed;         u32 abgr; } polyline;

    /* Dashed line tessellates to one oriented textured quad: U spans 0..len/period so the
       atlas dash row tiles along the line; duty (on-fraction) picks the nearest baked row. */
    struct { f32 x0, y0, x1, y1, thickness, period, duty;     u32 abgr; } dash;

    /* Gradient rect: one quad, col_a -> col_b ramped in the fragment off the prim record
       (GUI_OP_GRAD).  horizontal != 0 = left->right, else top->bottom.  Always
       square.  u32, not bool, for the same no-tail-padding rule as sprite below -- the
       retained-cache hash folds these bytes raw. */
    struct { f32 x, y, w, h; u32 col_a, col_b; u32 horizontal; } gradient;

    /* Rect list: `count` solid fills from the per-frame rect pool (s_draw.rect_pool), one
       command for the whole batch -- the dense-shape escape valve (timeline bars, graph
       columns).  Always square, GUI_OP_SELF solid fill, per-entry color; entries share the clip. */
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
       the frame must still be PRESENTED, see GUI_FX_TIME_WRAP).  Every one of these is the
       same GUI_FX_BOX mode; what differs is the op the tessellator stamps.
       `rot` (radians, screen space, about the box CENTRE) turns the whole surface: the fx
       coordinate is box-local and affine, so rotating the four corner POSITIONS preserves the
       field under interpolation -- a rotated card costs the same one quad.  0 for
       every axis-aligned caller (shadow / pulse), and 0 keeps the grid snap.
       `variant` picks which of the five fills this is (GUI_FX_BOX_*), all sharing the one
       geometry:
         FILL   -- the filled surface: a glow or halo MEANT to be seen through its subject.
         SKIRT  -- the interior cut away (GUI_OP_CUT), same outward falloff, painting
                   nothing inside the boundary.  What a DROP shadow wants: the core of a
                   filled one is only ever seen through whatever it sits behind, which is a
                   translucent panel dimming itself.
         INSET  -- the falloff turned INWARD (GUI_OP_INSET), painting from the boundary
                   `feather` px in and nothing outside.  The inner shadow / pressed well.
         GLOW   -- the outward falloff resolved EXPONENTIALLY (GUI_OP_GLOW): light,
                   not blur.
         RING   -- the field bent into a border band of `border` px (GUI_OP_BAND): the
                   hollow ring the ripple wants, since a swelling band travels outward
                   with nothing in its middle.
       A non-zero `rate` adds GUI_OP_PULSE on top of whichever variant is set, so a cut or
       inset surface can breathe as readily as a filled one.
       A non-zero `swell` adds GUI_OP_SWELL: the boundary itself travels `swell * k` px
       (negative shrinks), riding the same clock -- the pop, the breathing outline, and with
       variant 4 plus a full-depth pulse the sonar ripple, all in one quad whose bytes never
       change while it runs.
       `cut_dx`/`cut_dy` move the SKIRT's cut off the shape it is drawn on: x,y,w,h is where
       the shadow lies and the cut offset says where the caster sits relative to it, so the
       falloff is measured from one outline while the hole is taken against another.  That is
       what makes a cast DIRECTIONAL, and 0 is the even one every caller had before.  The
       offset is in the shape's LOCAL frame -- the same as screen for the axis-aligned boxes
       that are the only things which cast.
       `phase` offsets the pulse wave in CYCLES, so same-rate pulses can stagger instead of
       beating in lockstep; 0 for every non-pulsing variant.  `curve`/`curve_param` shape the
       wave (gui_curve_t), baked from the ambient at push time.
       `shape` names a baked SDF shape (register_shape) to take the place of the rounded box,
       0 being the analytic box every caller had before.  Every variant above is unchanged by
       it -- the ops read a distance and do not care which field produced one -- so a glow, a
       ring or a swell reaches authored art through the same command that reaches a rect.
       x,y,w,h is then the PADDED box, inflated at emit from the caller's ink rect so the quad
       covers the field's margin; the ID travels rather than the UVs, since an SDF-atlas repack
       moves the tenant under geometry the retained cache may hold for many frames. */
    struct { f32 x, y, w, h; 
             f32 rounding, corner_pow, feather, rate, depth, rot;
             u32 abgr, variant; f32 cut_dx, cut_dy; f32 phase;
             u32 curve; f32 curve_param; f32 swell, border; u32 shape; } fx_box;

    /* Per-corner rounded fill -- the tab / notch / asymmetric card shape.  Geometrically it is
       the SAME one quad a uniform rounded rect emits; the one thing that differs is
       that each quad carries its own packed word, because the radius is the only shape
       parameter that lives in the WORD rather than in the vertices.  A quadrant already sees
       exactly one corner, so per-corner radii cost no extra geometry -- only the four separate
       stamps (see tess_fx_box_core).
       The field order IS the quadrant order the tessellator walks (top-left, top-right,
       bottom-right, bottom-left), so the two cannot drift apart.
       Filled and solid-colour only.  The stroked form stays a perimeter polyline:
       GUI_OP_BAND derives its interior hole from a single radius, and generalizing that is
       not worth it for a shape whose outline the polyline already draws correctly.
       `feather` is the falloff band exactly as fx_box carries it -- 0 gets the standard 1 px
       AA, wider makes the per-corner SOFT SHADOW (the tab / asymmetric-card drop shadow); the
       quadrants agree at any feather (tess_fx_box_core's centre-line proof).
       `col_b` makes it a GRADIENT fill: the ramp runs abgr -> col_b, shaped by `grad_kind`
       (gui_grad_t) and oriented by `grad_ang` (radians, box-local, 0 points +x -- the axis for
       a linear ramp, the direction a conic one peaks toward, ignored by a radial one).  A
       linear ramp spans the box exactly and holds its end colours across the AA skirt.
       col_b == abgr is a flat fill, which is not a special case but the honest degenerate one:
       the op is simply left off.
       The ramp reaches the fragment through the RECORD (GUI_OP_GRAD), which is why radial and
       conic exist at all -- neither can be described by colours at a rectangle's corners.
       `grad_mid` is the ramp's midpoint bend, stored as the EXPONENT the record carries
       (mapped once at push time, ln 0.5 / ln mid); 0 is the linear default. */
    struct { f32 x, y, w, h; f32 rtl, rtr, rbr, rbl; f32 feather, corner_pow; u32 abgr;
             u32 col_b; f32 grad_ang; u32 grad_kind; f32 grad_mid; } round_rect;

    /* Circular sector -- ONE member serving GUI_CMD_ARC and GUI_CMD_PIE, which differ only in
       the field the fragment evaluates, not in anything they carry.  Angles are radians in
       screen space (0 points +x, positive turns clockwise, matching text_xf.rot); a1 < a0 is
       normalized at tessellation and a sweep of a full turn routes to the exact ring / disc
       primitives instead.  `thickness` is the stroke width for ARC and is ignored by PIE.
       Sampled as a polyline this shape would need up to 66 points fanned or stroked -- up to
       65 separate TRIANGLE commands for a pie, ~130 vertices for a spinner.  It costs one
       quad instead, because a circular field needs no quadrant fold (see the effect band).
       spin_rate (turns/sec) + spin_phase (turns) put the sector under GUI_OP_SPIN: the whole
       frame rotates on pc.time in the FRAGMENT, so a spinner's bytes are identical every
       frame and it re-tessellates nothing -- the pulse contract for rotation.  0 = static.
       `curve`/`curve_param` shape the revolution (gui_curve_t): STAIR is the clock-hand
       spinner that ticks between positions rather than sweeping. */
    struct { f32 cx, cy, r, thickness, a0, a1; f32 spin_rate, spin_phase; u32 abgr;
             u32 curve; f32 curve_param; } arc;

    /* The arc under an angular dash cut.  `period` is radians per dash+gap cycle -- the emit
       side quantizes it so a WHOLE number of cycles fits the sweep, which is what keeps a
       closed dashed ring from showing a seam where the pattern meets itself; `duty` is the
       on-fraction.  Both reach the fragment through the record (GUI_OP_DASH). */
    struct { f32 cx, cy, r, thickness, a0, a1; f32 period, duty; u32 abgr; } arc_dash;

    /* The arc whose colour sweeps col_a (at a0) -> col_b (at a1).  col_b reaches the fragment
       through the record; GUI_OP_GRAD_ALONG lerps by the arc-length coordinate over the sweep
       -- the one gradient a 4-corner vertex colour could never express, because it varies by
       ANGLE, not position. */
    struct { f32 cx, cy, r, thickness, a0, a1; u32 col_a, col_b; } arc_grad;

    /* A textured quad under a rotation about its CENTRE -- the text_xf treatment applied to
       one quad (tess_quad_xf).  UVs resolve at emit exactly as draw_push_icon's do: an icon
       is a quad forever, and the atlas UV moves only on a bake, which reloads everything.
       Centre pivot rather than the text anchor because a marker / needle / spinner glyph
       turns about its own middle -- the one pivot every caller was computing anyway. */
    struct { f32 x, y, w, h; f32 u0, v0, u1, v1; f32 rot; u32 tex_idx; u32 abgr; } image_xf;

    /* The cell pattern: col_a fills even cells, col_b odd, anchored at the box origin.  The
       fragment tiles it in framebuffer space (GUI_OP_CHECKER), so this is ONE quad at any
       area and any cell, where a rect-pool expansion would cap at 64x64 cells.
       `rounding` is the ambient radius, folded in like any other fill's: the pattern is an OP
       now, so it lands inside the shape's own boundary instead of being the shape.  That is
       what makes the transparency chequerboard behind a colour swatch the SWATCH, one quad. */
    struct { f32 x, y, w, h; f32 cell; u32 col_a, col_b;
             f32 rounding, corner_pow; } checker;

    /* The line lattice: a line every `cell` px, `thickness` px wide, in abgr over NOTHING --
       the caller layers it on its own fill.  (ox, oy) is the lattice anchor in screen px:
       lines land on ox + k*cell, so a panning canvas passes its content origin and the grid
       rides along (GUI_OP_GRID).  `rounding` is the ambient radius: the lattice ends at the
       panel's rounded boundary rather than at a rectangle over it. */
    struct { f32 x, y, w, h; f32 cell, thickness, ox, oy; f32 angle; u32 stripes;
             u32 abgr; f32 rounding, corner_pow; } grid;

    /* Regular polygon (GUI_FX_NGON): `sides` flat edges inscribed in circumradius r, rotated
       by `rot` (radians, 0 = a vertex pointing up), corners rounded by `rounding` px.
       thickness > 0 strokes it (GUI_OP_BAND); 0 fills.  star > 0 pulls each edge midpoint in
       to star * r, drawing a `sides`-pointed star from the same field; 0 = regular. */
    struct { f32 cx, cy, r, rounding, rot, thickness, star; u32 sides; u32 abgr; } ngon;

    /* Rounded-box outline cut by a perimeter dash (GUI_OP_BAND + GUI_OP_DASH).  dash/gap are
       arc-length px (the draw_dashed_line vocabulary); the tessellator snaps the period so
       whole cycles fit the perimeter.  `rate` scrolls the pattern in px/sec on pc.time --
       the marching ants -- and `phase` is a static px offset; both animate in the FRAGMENT,
       so the ants re-tessellate nothing.  `curve`/`curve_param` shape how the pattern crosses
       one period (gui_curve_t): STAIR is the ants that jump a dash at a time. */
    struct { f32 x, y, w, h; f32 rounding, t; f32 dash, gap; f32 rate, phase;
             u32 abgr; u32 curve; f32 curve_param; f32 anim_phase; } box_dash;

    /* A LATTICE of one rounded cell (GUI_OP_REPEAT): nx by ny copies, `pitch` apart
       centre-to-centre, centred on (cx, cy).  The cell's size is stated rather than the set's
       box, because the set's box is derived from the two and the fragment recovers the count
       back out of it (gui_build_tess.c, tess_repeat_box).  ONE quad however many copies.
       `col_b` != abgr ramps the copies toward it along the strip (GUI_OP_GRAD_CELL); `fill`
       >= 0 lights only that fraction of them (GUI_OP_CELL_FILL) -- the segmented meter. */
    struct { f32 cx, cy; f32 pitch_x, pitch_y; f32 cell_w, cell_h; f32 rounding;
             u32 nx, ny; u32 abgr; u32 col_b; f32 fill; } repeat;

    /* A RING of one rounded cell (GUI_OP_REPEAT_POLAR): `n` copies on a circle of radius
       `orbit` about (cx, cy).  `rate` animates on the shader clock in revolutions/sec -- and
       because that animates in the FRAGMENT, these bytes are identical every frame and a
       spinner re-tessellates nothing.  What the rate DRIVES depends on `col_b`: equal to
       abgr, the whole ring turns rigidly (GUI_OP_SPIN); different, the dots stay put and a
       colour ramp toward col_b marches around them, trailing the bright head -- the tail
       spinner (GUI_OP_GRAD_CELL).  `curve`/`curve_param` shape either motion (gui_curve_t):
       STAIR at `n` steps is the clock-hand tick. */
    struct { f32 cx, cy; f32 orbit; f32 cell_w, cell_h; f32 rounding;
             f32 rate, phase, curve_param; u32 n, curve, abgr; u32 col_b; } repeat_polar;

    /* A rounded box with a SECOND rounded box carved out of it (GUI_OP_CUT_SHAPE) -- the
       subtraction no number of extra quads can paint.  The cut's centre is an OFFSET from
       the shape's centre (the record's cut vector); `cut_aa` is the carved edge's own AA
       band in px. */
    struct { f32 x, y, w, h; f32 rounding; f32 cut_dx, cut_dy, cut_w, cut_h;
             f32 cut_r, cut_aa; u32 abgr; } box_cut;

} gui_cmd_ext_t;

/*==============================================================================================
    GUI_DRAW -- direction: a cardinal direction, the ImGuiDir analogue.  Passed to 
    arrow_button (and any future directional widget) to pick which way the glyph points.
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

    /* This surface's whole body is a drag-and-drop landing zone (PANEL/PANEL_CHILD's HOT --
       gui_bake.c), for a generic drag_source_begin payload (interact/gui_drag.c): a dragged row
       that reorders anywhere inside a list, a folder that accepts a drop anywhere in its body.
       Explicit opt-in on purpose -- without it, a payload dragged over a window or child never
       lights up that surface, no matter how many acceptable targets sit inside it: it is the
       CALLER's call which containers read as "you can drop somewhere in here" and which are just
       scenery the drag happens to pass over (individual widget targets still ring on their own
       via drag_payload_accept / draw_drop_ring regardless of this flag).  A window carrying it
       lights up for this OR for a title-drag over a dockspace (window_route_is_drop_target); a
       child has no dock equivalent, so this flag is its only HOT source. */
    GUI_WIN_DRAG_TARGET       = 1 << 28,

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
/* Curated font families -- the "it just works" font selection.  A family names a typeface, not
   a file: the resolver finds a bake at any requested size (a shipped .orb_font, a cached bake,
   or the host-installed runtime baker), so no size matrix exists here.  A face outside this
   list is reached by source name through font_get() with identical behavior.  GUI_FONT_NONE
   boots no managed font; the caller is then responsible for its own font_load() before the
   first frame renders. */

typedef enum
{
    GUI_FONT_NONE = 0,        // no managed font; the caller loads its own via font_load()
    GUI_FONT_JETBRAINS,       // JetBrains Mono NL (OS-installed)
    GUI_FONT_ROBOTO,          // Roboto Regular (assets/font_source)
    GUI_FONT_CASCADIA_MONO,   // Cascadia Mono (ships with Windows 11)
    GUI_FONT_CASCADIA_CODE,   // Cascadia Code (ships with Windows 11)

    GUI_FONT_FAMILY_COUNT

} gui_font_family_t;

/*==============================================================================================
    GUI_FRAME -- DPI response mode

    How gui reacts to monitor scale (app()->window_dpi_scale; the process is per-monitor DPI
    aware, so all engine coordinates are physical pixels).  gui scales by resolving the
    managed family at base_size * scale -- a bigger size raises em, and every layout metric
    already rescales from em.  With a runtime font baker installed the response is EXACT (any
    size bakes on demand); without one it snaps to the nearest shipped size.  Each surface
    (viewport) resolves against ITS OWN hosting window's scale, so floaters on
    differently-scaled monitors each get the right size (mixed DPI).
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
    gui_font_family_t  font;        // managed boot family; GUI_FONT_NONE = caller font_load()s
    u32                font_size;   // requested boot size, px; 0 = 16

    gui_clock_fn       clock;       // system clock function callback
    gui_sleep_fn       sleep;       // system sleep function callback
    gui_wait_events_fn wait;        // system wait-for-events function callback

    f32                clear[ 4 ];  // boot_present_begin clear color; alpha 0 = dark
    
} gui_boot_desc_t;

/*==============================================================================================
    GUI_FRAME -- limits
==============================================================================================*/

/* A quad record is one SHAPE -- a fill, a glyph, a capsule segment, a whole arc -- so 8K 
   covers the busiest measured frame several times over. The storage cost is what to weigh
   when moving it: the quad table holds a full set for every (frame-in-flight, viewport)
   region, so 8192 x 16 B x 8 regions = 1 MB of device memory.

   Every cap here is measured, not guessed at: gui_print_mem_stats prints each pool's lifetime
   high-water mark against its cap, and the shutdown log repeats the BUILD three.  Read those
   before moving a number -- and when content goes missing, the build's overflow warning names
   which of these it was. */

#define GUI_MAX_QUADS           ( 8192 * 2 )   // per-frame quad records (gui_quad_t)
#define GUI_MAX_PRIMS           ( 512 * 2 )    // per-frame prim records (gui_prim_t)
#define GUI_MAX_CMDS            ( 1024 * 2 )   // per-frame semantic draw commands

/* Payload byte budget for s_draw.cmd_pool, the single forward bump arena every command's
   payload is claimed from (draw_cmd_ext_slot).  32 bytes/command is a first-pass estimate, not
   a measured average: RECT_FILL and TEXT dominate real frames per the demo-library census
   elsewhere in this file, and both are under 32B (see k_cmd_hash_len, gui_emit_cmd.c), so a
   frame that is nearly all hot commands has headroom to spare; a frame heavy in wide cold
   types (fx_box et al) draws down that headroom instead of forcing every command to reserve
   it up front.  Retune from the per-type high-water marks in backend_pool_report once real
   traffic has been measured. */

#define GUI_CMD_POOL_BYTES      ( GUI_MAX_CMDS * 32 )
#define GUI_MAX_PATH_PTS        ( 2048 )       // per-frame total polyline / path point pool
#define GUI_MAX_RECT_ENTRIES    ( 4096 )       // per-frame total draw_rects batch pool
#define GUI_MAX_TEXT_POOL       ( 16 * 1024 )  // per-frame flat string copy pool for text cmds
#define GUI_MAX_CLIP_RECTS      ( 64 )         // per-frame clip table entries; u8 index caps at 256
#define GUI_CLIP_DEPTH          ( 32 )         // push_clip / pop_clip nesting depth

/* Prim records are counted per STATE CHANGE, not per quad -- a glyph run is one, a run of flat
   fills sharing a texture and a clip is one, and identically-styled shapes dedup across
   placements -- so the cap sits far below the quad cap.  Measured UIs land in the dozens; 512 is
   the next power of two above anything sb_gui's full demo set reaches with the dashboard open.

   The GPU multiplier is what makes this cap worth being honest about: the storage buffer holds a
   full set of records for EVERY (frame-in-flight, viewport) region, so 512 records is 513 KB
   across 8 regions where 2048 was 2 MB.

   512 also happens to be the largest cap at which the quad's PACKED FIELDS can name every record
   the arena can hold -- see the static assert below, which is what pins that.  Past it the fx row
   index outgrows the field first (the GLYPH layout at 512, the SHAPED one at 1024), and a spill
   there is a wall no amount of arena is a fix for.  Going back up is a step at a time: 1024 costs
   the glyph tag its guarantee (a rotated character in a record-heavy window falls back to the
   SHAPED path, which is correct but spends a prim record), and 2048 needs the union re-planned. */

/* The arena's LAST addressable fx row -- the final row of a page opened at the last record a slot
   could reach -- against the two quad layouts that carry one (gui_quad_t).  Trips at compile time
   if GUI_MAX_PRIMS is raised past what the fields can name, rather than at runtime through
   TESS_OVF_FX_FIELD on whichever frame first gets record-heavy enough to reach it. */
#define GUI_PRIM_FX_ROW_MAX  ( ( GUI_MAX_PRIMS - 1u ) * GUI_PRIM_ROWS \
                             + ( GUI_PRIM_ROWS - GUI_FX_ROWS ) )

ORB_STATIC_ASSERT( GUI_PRIM_FX_ROW_MAX <= GUI_QUAD_FX_MASK,
                   "GUI_MAX_PRIMS outgrew the quad's fx field -- re-plan the idx union (gui_quad_t)" );
ORB_STATIC_ASSERT( GUI_MAX_PRIMS - 1u <= GUI_QUAD_PRIM_MASK,
                   "GUI_MAX_PRIMS outgrew the quad's prim field -- re-plan the idx union" );

/*==============================================================================================

    THE PRIM PALETTE -- shared records the whole frame addresses, in the prim field's dead half.

    A slot-local style index names a record inside the emitting window's own arena run, so two
    windows drawing the same button hold two copies of one record and no dedup walk can reach
    across the boundary between them.  A PALETTE entry is that record placed once, outside every
    region, named by an index the arena can never produce: the prim field is 11 bits and the arena
    caps at GUI_MAX_PRIMS, so GUI_PAL_FIRST .. GUI_PAL_FIRST + GUI_PAL_MAX - 1 is index space
    nothing else can occupy.

    Which base an index resolves against is therefore the index itself -- at or above GUI_PAL_FIRST
    it resolves absolutely against pc.pal_base, below it slot-locally against pc.prim_base exactly
    as before (shaders: prim_row, gui_common.hlsli).  Reserving LOW indices instead would have
    rebased every slot-local index, every win_geo_slot_t.prim_base and the asserts above.

    fx rows are NOT palettized: they stay slot-local against prim_base, so a palette style still
    composes with a per-instance turn, animation phase and uv rect.

    Palette CONTENT is learned, not constant: radii, feather and border widths scale with em and DPI
    (gui_style_apply), so the table is emptied and re-learned when the landed style moves.  Static in
    SLOT, live in CONTENT.

    MIXED DPI needs no second addressing scheme.  Two monitors at different scales put two style
    scales in one frame (gui_frame_dpi.c: windows build sequentially and each lands its own), and a
    scale reaches the record as SCALED LANE VALUES -- a 10.67 radius at 1x is a 21.33 radius at 2x,
    which is a different record by its bytes.  So both scales' vocabularies simply coexist in the one
    table, and the content-addressed lookup separates them for free.

    ENTRIES ARE EARNED, NOT AUTHORED.  There is one route in: a record the frame draws again in a
    later build frame takes an entry on the spot (pal_intern, render/pipeline/gui_render_intern.c).
    Nothing is registered, named or declared anywhere, which is what lets the palette reach a UI
    layer the engine has never seen -- a custom theme's own shapes are covered by being drawn twice.
    The cap therefore sizes for a whole application's working set, not for chrome's: a theme's
    vocabulary is tens of entries, an application's is hundreds.
==============================================================================================*/

#define GUI_PAL_FIRST  1024u   /* first style index that resolves against the palette */
#define GUI_PAL_MAX     256u   /* palette entries, ACROSS every live style scale       */

ORB_STATIC_ASSERT( GUI_MAX_PRIMS <= GUI_PAL_FIRST,
                   "the prim arena grew into the palette's index range -- raise GUI_PAL_FIRST" );
ORB_STATIC_ASSERT( GUI_PAL_FIRST + GUI_PAL_MAX - 1u <= GUI_QUAD_PRIM_MASK,
                   "the palette outgrew the quad's prim field -- re-plan the idx union" );

static inline bool gui_prim_is_pal( u32 style ) { return style >= GUI_PAL_FIRST; }
static inline u32  gui_prim_pal   ( u32 entry ) { return GUI_PAL_FIRST + entry;  }

/* "No entry" from a palette lookup.  Past GUI_PAL_MAX rather than a signed -1, so the one test a
   caller writes is `< GUI_PAL_MAX` and a miss can never index the table. */
#define GUI_PAL_NONE  0xFFFFFFFFu

/*  What the palette is doing -- the debug A/B lever, one axis rather than two.  The lookup and
    the learning are not independent: interning is the ONLY route into the table, so "no learning"
    from a cold start leaves the lookup with nothing to answer and is indistinguishable from off.
    Three states is the whole honest space.

    (Spelled GUI_PALETTE_* to stay visually apart from the GUI_PAL_* index-space constants above;
    these name a mode, those name a slot.) */

typedef enum
{
    GUI_PALETTE_OFF = 0,   // no lookup: every style takes a per-slot arena record, as before the
                           //   palette existed -- more records, identical pixels
    GUI_PALETTE_FROZEN,    // the table answers but earns nothing new; what a run measures the
                           //   learned set's coverage against
    GUI_PALETTE_LEARNING   // the default: answers, and a record drawn again in a later build
                           //   frame earns an entry

} gui_palette_mode_t;

/*============================================================================================*/
/* Command segments: one contiguous span of the command list per (win, z, vp, band) the emit
   path stamps, cut wherever a window seam, draw_set_sort_key, draw_set_viewport or draw_set_band
   changes the tag (draw_seg_retag).  The render backend orders these spans instead of re-scanning
   the whole command buffer.  Worst case each command sits in its own segment, plus the open one,
   so the cap is the command cap + 1. */

#define GUI_MAX_SEGS       ( GUI_MAX_CMDS + 1 )

/*==============================================================================================
    GUI_FRAME -- memory usage breakdown (bytes), reported by gui()->mem_stats().

    A full accounting of what the gui system holds, split by WHERE it lives:

    - GPU:          Device memory -- per-viewport geometry buffers, atlas textures, and (Debug)
                    the debug overlay's own buffers.  Dynamic: created at init / viewport_open,
                    released at shutdown / viewport_close.

    - CPU static:   EVERY fixed backend buffer baked into the image (.bss/.rdata) -- the draw
                    list, tessellation staging, retained cache, font/atlas/icon registries,
                    render state + embedded shaders, capture buffers, debug tooling.  Present
                    for the whole run whether one window is open or fifty; summed exhaustively
                    in render/gui_render_mem.c (the accounting contract lives there).

    - CPU heap:     One malloc block per live context (header + state / popup / window /
                    viewport / dock pools).  Dynamic: grows only when a secondary context is
                    created.

    Every bucket is exact (a sizeof of the backing array, a summed malloc size, or a live-count
    multiply of a fixed region), so the grand total is the true resident footprint -- not a
    high-water estimate.  print_mem_stats() dumps the same breakdown as a sectioned table.
==============================================================================================*/

typedef struct
{
    /* --- GPU device memory (dynamic). --- */

    u32 gpu_texture_bytes;      // the three resource atlases (coverage incl. assist rows, sprite, SDF)
    u32 gpu_table_bytes;        // the storage-buffer tables the shader resolves through, summed --
                                //   sized by the pool caps, not by how many surfaces are open

    /* The table total, split by which table.  The first three are REGIONED, so a raised cap costs
       a region multiplier: quad/style hold a full set of entries per (frame-in-flight, viewport)
       since they carry per-surface geometry (gpu_regions); clip holds a full set per frame-in-
       flight only, since a window's clip slab is the same for every viewport (gpu_clip_regions).
       The glyph table is one shared copy, replaced rather than rewritten, so it pays no multiplier. */

    u32 gpu_quad_bytes;         // quad records   -- GUI_MAX_QUADS x 16 B x gpu_regions
    u32 gpu_prim_bytes;        // prim records  -- (GUI_MAX_PRIMS + 1) x 128 B x gpu_regions,
                                //   plus GUI_PAL_MAX x 128 B per frame-in-flight (the palette)
    u32 gpu_clip_bytes;         // clip entries   -- window slabs x 32 B x gpu_clip_regions
    u32 gpu_glyph_bytes;        // glyph UV table -- GUI_GLYPH_TABLE_MAX x 8 B, ONE copy
    u32 gpu_regions;            // frames-in-flight x viewport slots the quad/style tables carry
    u32 gpu_clip_regions;       // frames-in-flight only -- clip is window-keyed, not viewport-keyed
    u32 gpu_debug_bytes;        // debug-overlay quad table (Debug builds; 0 when compiled out)
    u32 gpu_total;              // sum of the section above
    u32 viewport_count;         // live GPU surfaces

    /* --- CPU static memory (.bss + .rdata; fixed backend buffers, resident the whole run). --- */

    u32 cpu_drawlist_bytes;     // EMIT: s_draw (cmds + hashes + point/rect/text/clip pools) + path stroker
    u32 cpu_tess_bytes;         // BUILD: the CPU quad / style / GPU-command arenas, the tessellation
                                //   diagnostics, and the per-region dirty-span table
    u32 cpu_cache_bytes;        // retained cache: slot tables, stable cmd cache, diff records, seg chains,
                                //   permutation scratch, volatile registry, stats
    u32 cpu_draw_bytes;         // DRAW unit statics: icon + sprite registries (the font registry
                                //   lives under font/ and counts under cpu_frontend_bytes instead)
    u32 cpu_res_bytes;          // the three atlas instance records (packer nodes + tenant bookkeeping)
                                //   plus the shared skyline scratch a repack trial runs against
    u32 cpu_render_bytes;       // RENDER: pipeline / sampler / push state (shaders load from disk)
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

/* Two families of numbers, split by intent: APPLICATION COST fields exclude the debug band (the
   perf overlay / dashboard must not count themselves in what they display), while POOL FILL
   (_all) fields are the physical count in the shared bucket, tooling included -- overflow
   pressure against a cap is physical, so netting it would hide real risk. */

typedef struct
{
    /* --- BUILD: geometry produced or reused this frame. --- */

    u32 cmd_count;          // semantic draw commands the UI emitted (debug band excluded)
    u32 clip_count;         // clip table entries referenced by those commands (debug band excluded)
    u32 cmd_count_all;      // physical command pool fill, both bands (cap: GUI_MAX_CMDS)
    u32 clip_count_all;     // physical clip table fill, both bands (cap: GUI_MAX_CLIP_RECTS)
    u32 seg_count;          // physical segment count, both bands (cap: GUI_MAX_SEGS)

    u32 text_pool_used;     // physical text pool bytes, both bands (cap: GUI_MAX_TEXT_POOL)

    u32 quad_count;         // quad records that DRAW -- one record IS one whole shape 
                            //   (a fill, a glyph, a capsule segment); debug band excluded
    /* THE STYLE ACCOUNTING -- three numbers over one population, so they reconcile.
       A prim record (gui_prim_t) lives in one of two pools, told apart by the range of the index
       a quad names (gui.h, GUI_PAL_FIRST): the STORED pool, one frame-global table shared by every
       window, and the UNIQUE pool, the per-window-slot arena rebuilt each frame.

         prim_total   every shape that wanted a prim record -- one per SHAPE, not per quad (a BAND
                      covering resolves one style and emits four; a plain glyph resolves none, so
                      this sits well below quad_count).  The denominator.
         prim_stored  how many stored entries those shapes named -- a UNION across windows, since
                      two windows naming one entry is the duplication the pool exists to remove.
         prim_unique  what fell through to a per-slot arena record: distinct styles after dedup,
                      plus one page per four fx instance records.

       So (prim_total - prim_unique) / prim_total is the fold rate, and prim_stored says how much
       of the stored pool this frame actually leans on.  All three are taken at tessellation and
       carried across retained frames -- replayed geometry is never re-walked. */

    u32 prim_total;
    u32 prim_stored;
    u32 prim_unique;

    u32 prim_stored_max;    // entries the stored pool HOLDS (cap: GUI_PAL_MAX) -- cumulative within
                            //   an epoch, so prim_stored <= this and a wide gap means a learned
                            //   vocabulary the current frame does not draw

    u32 quad_count_all;     // physical quad arena fill, both bands, slot padding included (cap: GUI_MAX_QUADS)
    u32 prim_count_all;     // physical prim arena (unique pool) fill, both bands (cap: GUI_MAX_PRIMS)

    u32 win_total;          // windows tracked this frame
    u32 win_retained;       // windows whose geometry was reused (no re-tessellation)
    u32 quad_retained;      // quad records that came from prev-frame copy, not re-tessellated

    /* diff_ms/tess_ms follow the BUILD publish rule: they hold the last REAL frame's cost across
       an idle one, since that is still what the geometry on screen cost.  Inert without a clock
       hook (gui()->frame_set_hooks) -- both read zero without one. */

    f32 diff_ms;             // BUILD step 1: hash + diff every window (cache_diff_windows)
    f32 tess_ms;             // BUILD step 2: reuse or tessellate every window, incl. a repack retry

    /* --- SUBMIT: what it cost to get that geometry on screen. --- */

    u32 draw_calls;          // GPU draw calls (batches), summed over surfaces
    u32 upload_batches;      // number of buffer write calls per frame
    u32 upload_bytes;        // total bytes written into the quad / style / clip tables

    u32 volatile_patched;    // volatile_cb rows whose geometry was patched in place this frame
                             // (idle replay or a live real-frame reuse-patch) -- a separate signal
                             // from win_retained: a window with an animating volatile widget still
                             // counts as fully retained; this is what actually moved.

    /* submit_ms is summed over this frame's surface flushes and resets every frame, because the
       GPU replays even an idle frame.  Also inert without a clock hook -- reads zero without one.

       Neither diff_ms/tess_ms nor submit_ms sum to the host's measured render time: the debug
       overlay's own flush and the per-surface setup around these zones sit outside all three. */

    f32 submit_ms;           // SUBMIT: uploads + draw-call recording, summed over surfaces

    /* What the GPU spent EXECUTING, timestamped on the GPU itself (rhi cmd_gpu_ms) and summed
       over this frame's surfaces -- the counterpart to the CPU zones above, which only ever see
       recording cost.  Each surface's sample is its context's whole command buffer (for a
       gui-only surface that is the clear + the gui pass), and it trails by the frames-in-flight
       depth.  Reads zero only on hardware without graphics-queue timestamps; unlike the zones
       above it needs no clock hook. */

    f32 gpu_ms;              // GPU execution, summed over surfaces, ~2 frames latent

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
