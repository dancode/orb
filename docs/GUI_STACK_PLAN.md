# GUI Stack Plan -- from one grab bag to a stack of small libraries

Status: ALL INCREMENTS DONE (2026-07-21).  Phase 1 (1-6: vtable TOC, rect leaf, flow seams,
element library + style strata, kits + on_hud) and phase 2 (7-10: pane, feat_* kit, the
stock-window recipe re-seat, and the physical per-library TU split) are built.  gui is six
compiler-enforced translation units; gui_window_t IS pane + feat_* + titlebar policy; the
gui() surface reads as a table of contents over the libraries.
Companion to the rect-first campaign proven by sb_gui_diablo.
Goal: break the ~200-entry gui() surface into named sub-libraries, each a small digestible
center with a public header, a one-way dependency, and utilities placed where they belong.
The imgui-style editor convenience layer becomes ONE library at the top -- not the identity
of the system.

The coordination axis (established): layout of any kind is a RECT PRODUCER; a widget is a
RECT CONSUMER; `item( id, rect ) -> state` is the axis.  Everything the server provides is
ambient and id-keyed (behavior, identity, draw list + clip/z, style, dirtiness, animation),
so producers and consumers compose freely and no layer can tell how a rect was made.

---

## 1. The library map

Seven libraries plus glue.  Each already has a physical seed in the tree (right column).

| # | Library       | What it is                                         | Seed today               |
|---|---------------|----------------------------------------------------|--------------------------|
| 1 | gui_draw      | RENDER SERVER: draw list, symbols, paths, tess,    | backend/ + present/      |
|   |               | retained cache, atlas/fonts/icons, GPU flush       | (gui_backend.c TU)       |
| 2 | gui_core      | INTERACTION SERVER: ctx, io snapshot, ids, keyed   | core/ + interact/ + nav/ |
|   |               | state pool, item() machine, nav, focus, capture,   | + surface/ + user/       |
|   |               | anim, drag-drop, surfaces (clip/scroll/z/persist)  | + compose/gui_region.c   |
| 3 | gui_rect      | RECT KIT: pure stateless carve math -- cut, place, | gui.h inlines +          |
|   |               | row/col/cell, span, inset, split, carve, anchor    | compose/gui_split.c      |
| 4 | gui_flow      | LAYOUT ENGINE: pen, templates (stack/cols/grid/    | compose/ (rest)          |
|   |               | pack/form), sizing family, avail, rows_clip        |                          |
| 5 | gui_element   | BUILDING BLOCKS: rect-consuming widget cores --    | NEW (lift sb_gui_diablo  |
|   |               | el_label, el_button, el_check, el_slider, el_meter | ui.c + internal cell_/   |
|   |               | ... over item() + draw_* + the slim element style  | item_/draw_ seams)       |
| 6 | gui_chrome    | CONVENIENCE / EDITOR UI: windows, docking, popups, | window/ dock/ popup/     |
|   |               | menus, toolbars, tabs, tables, stock flow widgets  | table/ widgets/          |
|   |               | (button(label)...), theme system, scale ramp       |                          |
| 7 | gui_debug     | overlays, dashboard, stepper (optional, caps-gated)| debug/                   |
| - | gui_frame     | GLUE: the context tying it together -- frame_begin | frame/ + gui_api.c       |
|   |               | /end, ctx scopes, viewports, boot, event routing,  |                          |
|   |               | pacing, the module vtable                          |                          |

App-side kits sit ABOVE gui, in their own homes, written the way sb_gui_diablo/ui.h was:
  - editor kit (source/editor):  property rows, asset pickers -- the editor's own idiom.
  - game kit (source/project):   how THIS game shows things (slots, globes, menus).
The stack's job is to make those kits ~200 lines each, not to ship them.

## 2. Dependency graph (one-way, enforced by include order)

        app (io)   rhi (gpu)
           ^          ^
           |          |
       gui_core    gui_draw          gui_rect (leaf, header-only, types+math)
           ^          ^                 ^
           |          |                 |
           +----+-----+--------+--------+
                |              |
           gui_element      gui_flow      (siblings; element NEVER includes flow)
                ^              ^
                +------+-------+
                       |
                  gui_chrome  (+ gui_debug)
                       |
                  gui_frame   (glue; sees everything, owns the vtable)

Rules the graph encodes:
  - gui_rect depends on nothing but types.  Same verbs at every scale: screen -> panel ->
    inside a widget's own rect (el_cycle carving its chevron squares is rect-in-element).
  - gui_element depends on core + draw + rect ONLY.  A widget core cannot reach the flow
    engine -- that is what keeps utility from being locked behind auto-layout again.
  - gui_flow produces rects and opens regions (core surfaces); it draws nothing.
  - gui_chrome is the only tier that fuses flow + element + windows into convenience calls.
  - gui_frame is the conductor: per-frame ordering, dirty gating, flush -- no widgets.

## 3. Style: layered, not a grab bag

Today one theme system (gui_col_t slots + style vars + scale + push stacks) feeds every
tier.  Split it into strata where each higher stratum COMPILES DOWN to the one below and
lower tiers never look up:

  S0 -- draw:     NO ambient style.  Every draw_* takes explicit colors/widths.  (Already
                  true; keep it true.)

  S1 -- element:  the slim core the user asked for -- the minimum a building block needs:

                      typedef struct gui_el_style_t
                      {
                          f32 pad;        // interior pad, all sides
                          f32 gap;        // space BETWEEN slots (square-spacing rule:
                                          //   defaults equal to pad)
                          f32 border_w;   // frame line width
                          f32 line_h;     // text basis (active font line height; ui_u base)
                          u32 col[ GUI_EL_ROLE_COUNT ][ GUI_EL_STATE_COUNT ];
                      } gui_el_style_t;

                      roles:  BG, BORDER, TEXT, ACCENT            (4)
                      states: IDLE, HOT, ACTIVE, DIM              (4)

                  4 metrics + a 4x4 role/state palette = the whole element vocabulary.
                  NO per-widget slots (btn_bg_hover, slot_border_hot, globe_ring...) --
                  per-widget color is either a call parameter (el_meter fill) or a token
                  in the kit above.  That is the grab-bag killer.

  S2 -- chrome:   the existing theme system, reframed as a COMPILER: named themes, slot
                  table, style vars, scale ramp, push/pop stacks resolve into (a) chrome's
                  own window/dock/popup colors and (b) an installed gui_el_style_t, so
                  themes and scale reach elements without elements knowing themes exist.

  S3 -- app kits: editor/game tokens ("panel", "danger", "gold") compiled down to S1/S2
                  values at kit scope.  Their business, not gui's.

## 4. The seam contract -- recursive rect <-> flow

Requirement: carve a rect, auto-flow into it, take one flow element, carve from that, flow
into the remainder -- to arbitrary depth.  Both adapters exist today (push_layout_overlay,
empty); the plan blesses them as THE named pair and makes reentrancy a tested contract:

    flow_begin( rect )        -- open the layout engine inside ANY rect (a stack; nests)
    flow_cell( w, h ) -> rect -- take the next flow element AS a rect (w/h 0 = natural)
    flow_end()                -- close, resume the outer producer

Canonical nesting (the acceptance test):

    gui_rect_t panel = rect_cut_left( &screen, 300 );        // carve
    flow_begin( panel );                                     // auto-flow into it
        gui()->stack();
        gui()->text( "auto rows" );
        gui_rect_t cell = flow_cell( 0, sz );                // one flow element as a rect
        gui_rect_t half = rect_cut_left( &cell, cell.w / 2 );// carve from that
        el_button( half, "L" );
        flow_begin( cell );                                  // flow into the remainder
            gui()->cols_n( 2 );
            el_check( flow_cell( 0, 0 ), &a );               // element in a flow cell
            gui()->checkbox( "stock", &b );                  // stock widget, same cell flow
        flow_end();
    flow_end();

Scrolling is NOT flow's business: when a carved area needs scroll/clip/persistence, open a
core surface first (region_begin) and flow inside it -- "region owns scroll" stands.

## 5. The pane -- the minimal "window"

What is a window, really?  Strip the chrome and both servers already answer in their own
code.  The interaction server's whole occlusion contest runs on one signature:

    surface_hover_nominate( gui_id_t id, gui_rect_t r, u32 z, u32 viewport )

and the render backend's whole per-window unit is one 16-byte tag over a command span:

    gui_cmd_seg_t { gui_id_t win;  u32 z;  u16 lo, hi;  u16 font;  u8 vp;  u8 band; }

So the fundamental building block -- the PANE -- is a (id, z, vp) TAG shared by both
servers, plus one side-specific payload each:

    /* interaction server: the io-side window.  Everything that competes for the mouse,
       owns an id namespace, and establishes a base clip is exactly this. */
    typedef struct gui_pane_t
    {
        gui_id_t   id;      // identity: hover/active attribution, state pool, segment tag
        gui_rect_t rect;    // where it is; hit test + base clip derive from it
        u32        z;       // one number, two consumers: occlusion contest + paint order
        u8         vp;      // which OS surface
        u8         input;   // competes for hover? (the GUI_WIN_NO_INPUT bit)
    } gui_pane_t;

    /* render server: the draw-side window.  Already exists as gui_cmd_seg_t -- a pane tag
       plus a command range.  Clip stays per-command; font/band are batch context. */

The shared cross-section is the KEY (id, z, vp): interaction adds `rect` (hit), render
adds `[lo, hi)` (payload).  z is banded (background / windows / overlay / debug arena),
so "a region with a z" is exactly right -- region_begin is today's closest exposed form.

Everything called a window is a pane plus policy, a strict composition ladder:

    pane                                        -- the block above
     + gui_scroll_link_t                        -> scrolling region      (region_begin)
     + persisted rect ownership + flags         -> free window record
     + titlebar/resize/collapse/max-min chrome  -> the stock window      (gui_window_t)
     + dock membership                          -> docked pane
    pane + overlay bit + overlay-band z         -> popup / tooltip
    pane wrapping the OS window itself          -> viewport_shell

Five different window widgets with totally different chrome are five policies over one
pane: allocate the tag, nominate for hover, push the base clip, emit commands under the
tag, optionally attach a scroll link.  gui_core owns gui_pane_t; gui_chrome owns every
rung above it.  gui_window_t's remaining ~30 fields are chrome policy and stay in chrome.

## 6. The chrome feature kit -- window features as id-keyed policies

Every window feature becomes its own API section, usable alone, so a window widget is
built feature by feature -- and "anything can be a collapse, or a titlebar, or a max-min:
they just work over an id."  The tree already proves the shape twice: interact/gui_move.c
and interact/gui_resize.c are record-agnostic mechanisms (id + rect + cursor in, geometry
out) with the policy left to the caller -- windows, resizable children, and floating dock
groups already share them.  The kit generalizes that split to the rest of the chrome.

State rule (what makes a feature freestanding): IN-FLIGHT state is a service singleton
arbitrated by active_id (one drag at a time -- the existing pattern).  PERSISTENT state is
either a caller-owned pointer (mechanism form, diablo-style: you see every byte) or an
id-keyed pool blob (convenience form).  Mechanisms take pointers; wrappers persist by id.

Dependency classes -- how a feature can stand alone vs when it cannot:

  A. FREESTANDING     -- inputs are (id, rect, io) only; state is a small blob.
  B. ENVIRONMENT-DEP  -- must query the work area / viewport (clamp, maximize, tear-off).
  C. POPULATION-DEP   -- must enumerate siblings (z contest, shelf packing, tab targets).
  D. COMPOSITE        -- no state of its own; wires A-features + paint into a look.

The features, sectioned:

  | Feature      | Class | API sketch (mechanism form)                     | Today          |
  |--------------|-------|-------------------------------------------------|----------------|
  | move         |  A    | feat_move( id, handle_rect, &x, &y )            | gui_move.c OK  |
  | resize       |  A    | feat_resize( id, &rect, edge_mask, min_w/h )    | gui_resize.c OK|
  | press-defer  |  A    | click vs drag vs double-click disambiguation    | gui_move.c OK  |
  | collapse     |  A    | h = feat_collapse( id, &open, full_h ) tweened  | window fields  |
  | open latch   |  A    | caller bool (el_close_button sets it)           | by-title pair  |
  | next-channel |  A    | staged rect + cond mask over any persisted rect | window fields  |
  | scroll       |  A    | scroll_link_t attach to any pane                | 3 callers OK   |
  | max/min      |  B    | feat_maximize( id, &rect, &norm, work_area )    | window fields  |
  | clamp        |  B    | rect vs work area (viewport_content_y)          | window_clamp   |
  | tear-off     |  B    | move past threshold -> viewport_spawn           | window/frame   |
  | raise/front  |  C    | pane z via surface tier (its only writer)       | surface/ OK    |
  | shelf        |  C    | minimized chip parking (needs sibling order)    | window fields  |
  | titlebar     |  D    | a cut band + feat_move + el buttons; no state   | window_end.c   |
  | dock         | SYS   | stays a system (tree + membership), not a feat  | dock/ OK       |

The class tells you the cost of reuse: an A-feature moves anywhere for free; a B-feature
carries one query parameter (pass the work area IN -- do not let the feature find the
viewport); a C-feature only makes sense where a population exists (chrome keeps it); a
D-composite is a recipe, not a mechanism -- document it as one.

Building a window widget from scratch, feature by feature (the acceptance sketch):

    gui_pane_t p     = pane_begin( "tool", rect, z_band, vp );      // the block
    gui_rect_t r     = p.rect;
    gui_rect_t title = rect_cut_top( &r, ui_u( 1.5f ) );            // titlebar = a band...
    feat_move( p.id, title, &st->x, &st->y );                       // ...that drags
    if ( el_button( rect_cut_right( &title, title.h ), "x" ) )      // close = your bool
        st->open = false;
    r.h = feat_collapse( p.id, &st->collapsed, r.h );               // height tween by id
    feat_resize( p.id, &st->rect, GUI_RESIZE_R | GUI_RESIZE_B, min );// edges you choose
    /* ... body: flow_begin( r ) or carve on ... */
    pane_end();

gui_window_t then stops being a definition and becomes the stock RECIPE: pane + move +
resize + collapse + max/min + shelf + next-channel + titlebar composite -- one policy
file assembling kit features, with five-different-window-widgets equally legitimate.

Not super modular on purpose: the kit stays inside gui_core/gui_chrome as sections, not
seven micro-libraries.  The win is the CLASSIFICATION -- knowing each feature's true
inputs -- not maximal decoupling.

## 7. Public faces

One module vtable stays (gui_api_t -- the mod system needs a single func_api struct), but
it becomes a TABLE OF CONTENTS of the libraries: sections ordered draw -> core -> rect ->
flow -> element -> chrome -> debug -> frame, each section a curated header comment naming
its library.  In parallel, each library gets a real header usable directly in static
builds:

    gui_rect.h      inline math only -- usable anywhere, even outside gui
    gui_draw.h      draw_*/path_*/clip/text_size/icons/textures/volatile
    gui_core.h      item, ids, io queries, anim, drag-drop, surfaces, redraw levers
    gui_flow.h      layout verbs, sizing, avail, rows_clip, flow_begin/cell/end
    gui_element.h   el_* cores + gui_el_style_t
    gui_chrome.h    windows, dock, popups, menus, toolbar, tabs, tables, themes, stock set
    gui.h           types/enums/constants (unchanged role)
    gui_api.h       the vtable, sectioned; gui_host.h aggregates as today

Physical form: keep ONE build target (gui lib) but grow from 2 TUs to ~7 unity TUs, one
per library, each including only the headers below it -- the compiler enforces the graph.
Real build_tool targets can come later if wanted; the boundary is the header, not the .lib.

## 8. Migration increments (each builds + runs)

  1. SKELETON    -- write the library headers as curated declaration groupings (no code
                    motion); resection gui_api.h to match.  Pure reorganization.
                    DONE 2026-07-21: gui_rect.h created (geometry types gui_vec2_t/gui_rect_t/
                    gui_pad_t/gui_align_t + all rect/align/anchor inlines moved out of gui.h,
                    which now includes it first); gui_api.h vtable resectioned into the 8
                    library sections in plan order (draw/core/rect/flow/element/chrome/debug/
                    frame -- element is a reserved banner until increment 3) with the old
                    directory banners kept as sub-dividers; gui_api.c initializer regenerated
                    in matching order.  Full Debug build green.  Field set unchanged (vtable
                    LAYOUT changed: hot-reload hosts restart once).  Remaining headers
                    (gui_draw.h/gui_core.h/gui_flow.h/gui_element.h/gui_chrome.h) land with
                    their content at increments 3-4 -- no empty stubs.
  2. SEAMS       -- add flow_begin/flow_cell/flow_end over push_layout_overlay/empty;
                    depth-verify with a sandbox check running the canonical nesting above.
                    DONE 2026-07-21: three vtable entries in the GUI_FLOW section (flow_begin =
                    sublayout_open over the caller rect; flow_cell = the shared cell seam with
                    w/h <= 0 meaning natural track width / one standard row; flow_end =
                    pop_layout), implemented in compose/gui_sublayout.c, declared in gui_host.h.
                    Depth check: sb_gui_example "Layout > Flow Seams" demo runs the canonical
                    nesting live at three flow depths (canvas cut -> flow -> flow_cell -> cut ->
                    item() widget + flow -> flow_cell -> flow).  Full Debug build green.
                    func_api_size GREW: hot-reload hosts restart once.
  3. ELEMENT     -- stand up gui_element: lift sb_gui_diablo ui.c cores + the internal
                    cell_/item_/draw_ widget seams; add gui_el_style_t; chrome installs it
                    from the active theme.  sb_gui_diablo/ui.h shrinks to a game kit.
                    DONE 2026-07-21: gui_element.h (roles x states enums + gui_el_style_t,
                    included by gui.h) + element/gui_element.c (unity-included after user/).
                    Cores: el_panel / el_label / el_button / el_check / el_slider / el_meter /
                    el_cycle + el_style() kit door -- 8 vtable entries in the GUI_ELEMENT
                    section.  S2->S1 compile: gui_style_apply calls el_style_derive (reads the
                    ACTIVE font-scaled s_style), so every theme/font landing refreshes the
                    installed style; kits re-install after those calls to own the look.
                    sb_gui_diablo ui.c: button/check/slider/cycle/meter now delegate to el_*;
                    ui_kit_install() compiles the ember palette into el_style (called at boot
                    + after each font_use); slot/globe/title stay kit.  Full build green +
                    5s canary run OK.  func_api_size GREW again: hosts restart.
                    (Deferred: rewiring the internal stock-widget cell_/item_/draw_ seams
                    onto el_ cores -- that is increment 5's stock-widget pass.)
  4. TU SPLIT    -- 2 unity units -> per-library units; include order = dependency graph.
                    DONE (scoped) 2026-07-21: element/gui_element.c is a real THIRD unit
                    (orb.targets `unit` line on gui + gui_stress) -- it compiles against only
                    the public gui_* surface + one internal seam (style_active(),
                    gui_internal.h), so the element library boundary is compiler-enforced,
                    proving the per-library TU pattern the backend unit pioneered.  gui.c's
                    include list is re-bannered into LIBRARY GROUPS (order unchanged -- it
                    stays the static-visibility order).  The FULL per-library split is
                    deliberately re-staged to AFTER increment 5: the audit surfaced the real
                    cross-cuts a physical split must untangle first --
                      - present/ paint helpers read resolved style (draw-classified,
                        core-coupled; the style purge parameterizes them),
                      - nav/ reads the popup stack (core-classified, chrome-entangled),
                      - user/gui_stacks.c mixes core brackets with chrome style stacks,
                      - the ambient statics (g_ctx, s_scope, s_interaction, s_build) would
                        need extern-ing across ~100+ symbols -- only worth it per-library as
                        each library's file set becomes clean, not as one big-bang pass.
                    Full build green (incl. gui_stress variant).  No vtable change.
  5. STYLE PURGE -- stock widgets read element-shaped values through the installed
                    gui_el_style_t; per-widget theme slots retire into chrome tokens.
                    DONE 2026-07-21 (element-vocab resolver form, user-chosen over the
                    breaking literal purge): NEW style_el_col( role, state ) in
                    core/gui_style.c -- projects through g_el_slot_map (the element
                    unit's table, now shared with el_style_derive so the strata bridge's two
                    directions cannot drift), push/next_style_color overrides win, otherwise
                    the INSTALLED element style is the source.  10 COL_ macros re-pointed to
                    the roles x states vocabulary (TEXT/TEXT_DIM, WIDGET_BG/HOT/ACT,
                    CHILD_BG, BORDER, WIDGET_FG, CHECK_MARK, SLIDER_TRACK); the remaining 9
                    stay style_col as named CHROME TOKENS (window/input/nav/adornments).
                    Zero visual change by construction (no override + no kit overwrite ==
                    style_col exactly); a kit that overwrites gui()->el_style() now restyles
                    stock widget bodies with the same dial.  METRICS deliberately NOT routed
                    through el style: scale_push pushes row/pad/gap onto the var stack, which
                    the once-per-theme-land el derive cannot honor -- metrics remain
                    style_var (composer/chrome tokens).  Full build green + example canary.
                    This dissolves the "present/ reads resolved style" cross-cut for the
                    element-shaped subset (the full TU split's blocker list shrinks).
  6. KITS        -- editor kit and sample game kit move to their own homes as the reference
                    consumers; optional: split real build targets.
                    DONE 2026-07-21.  The audit surfaced a real contract gap first: a project
                    DLL had NO legal place to emit gui -- game()->tick runs in the host's
                    update phase, outside the gui frame bracket, and run_project.h had no gui
                    hook (the diablo kit only works because that sandbox owns its own loop).
                    So the increment's spine is the hook (user-approved ABI change):
                      - run_project.h: on_hud( dt, view ) -- the one phase where gui() calls
                        are legal; run_view_t v2 appends i32 gui_vp (-1 = no gui; tick-phase
                        views always carry -1).  Every project vtable slot must be non-NULL:
                        gui-less projects stub it (sb_proj_runtime does).
                      - runner: game()->hud( dt, view ) forwards on_hud while a session is
                        live (paused keeps the HUD up, same rule as draw).
                      - host_game: on_gui builds the v2 view (gui_vp = main viewport) and
                        calls game()->hud; update holds set_force_redraw( live ) so the
                        retained-cache emit skip cannot freeze a per-frame HUD (the
                        host_editor gate, same reason).
                      - host_editor deliberately does NOT forward on_hud: a project HUD over
                        the editor chrome is wrong, and HUD-inside-the-viewport-panel needs a
                        rect-scoped form of the hook -- future work.  Play Standalone shows it.
                    The kits, in their homes:
                      - GAME KIT: source/project/sample_game/game_ui.h/.c (~120 lines, own
                        unit) -- soft gui fetch (gui-less hosts stay supported), teal accent
                        install into el_style (S3 -> S1, re-installed on reload), HUD surface
                        bracket, score panel + tick meter over el_panel/el_label/el_meter;
                        sample_game.c composes them in on_hud with pure rect cuts.
                      - EDITOR KIT: source/editor/ed_kit.h/.c (~55 lines, unity in editor.c)
                        -- the PROPERTY ROW idiom: flow_cell takes the row, rect math cuts the
                        dim label column, flow_begin reopens the value zone so stock widgets
                        drop in unchanged.  editor_api.c's Deploy window is the reference form
                        (config/modular/pdbs/stage/deploy-to as prop rows); Game window
                        readouts use ed_prop_text.
                      - sb_gui_diablo/ui.h stays the standalone-loop sibling reference.
                    Build-target split SKIPPED on purpose: the full per-library TU split is
                    still staged (see inc 4) and the boundary is the header, not the .lib.
                    Full Debug build green (incl. sample_game_ship mono shape); 6s canaries:
                    host_game -module sample_game (HUD live, score ticking) and host_editor
                    -module sample_game (prop rows live) both clean.

## 9. Phase 2 increments -- the structural body (each builds + runs)

  7. PANE        -- build section 5: gui_pane_t (id, rect, z, vp, input) as the shared
                    minimal block; pane_begin/pane_end in gui_core over the existing
                    surface_hover_nominate + segment machinery (region_begin's guts,
                    re-seated).  Acceptance: a raw pane with hand-drawn chrome competes for
                    hover/z correctly beside stock windows, and region_begin/window_begin
                    are re-expressed as pane + policy internally with zero call-site change.
                    DONE 2026-07-21.  gui_pane_t public in gui.h (same-frame value, not a
                    record).  The internal core is pane_tag( id, z, vp, band ) in
                    surface/gui_surface.c -- the four draw stamps (window/sort_key/viewport/
                    band) + the ambient-viewport + s_build.win.id + s_scope.win commit that
                    every top-level occupant was doing by hand.  All THREE existing openers
                    now route through it with zero call-site change: region_begin (compose/),
                    window_begin_ex free path, and window_begin_docked (which also covers
                    popups, since the popup layer reuses window_begin_ex).  Hover nomination
                    stays the separate surface_hover_nominate verb ON PURPOSE: the nominated
                    rect is policy (resize-band padding, collapsed title-band, frame-only
                    caption), not part of the block.  Public pane_begin( id, rect, tier, vp,
                    flags ) = pane_tag + nominate + base clip (draw clip AND hit clip to the
                    rect, the docked-window pair; root clip seeded per viewport) + chrome
                    reset; honors NO_INPUT / NO_CLIP / DEBUG_BAND; root-level, never nests;
                    pane_end pops the clip and restores the main root clip like window_end.
                    Two vtable entries in GUI_CORE surfaces (func_api_size GREW: hosts
                    restart).  Acceptance demo: sb_gui_example "Windows > Raw Pane" --
                    hand-built chrome (rect cuts + el_button/el_check/el_label) over a pane
                    with a live tier cycle (MID/BG/FG) beside a draggable stock window.
                    Full Debug build green; 6s canaries sb_gui_example + host_editor clean.
  8. FEATURES    -- build section 6: carve the A-features out of gui_window_t/window_end.c
                    as freestanding feat_* mechanisms (collapse, open latch, next-channel,
                    max/min + clamp as B with the work area passed IN), each usable over any
                    id + caller-owned state, sectioned inside core/chrome -- not new libs.
                    move/resize/press-defer/scroll are already mechanism-shaped: bless their
                    names, don't rewrite them.  Acceptance: the section-6 sketch (a window
                    widget assembled feature by feature) runs in a sandbox demo.
                    DONE 2026-07-21.  NEW interact/gui_feature.c (unity after gui_anim.c):
                    5 vtable entries in a GUI_CORE "window features as mechanisms" section
                    (func_api_size GREW: hosts restart).  feat_move + feat_resize are thin
                    public forms over the EXISTING move/resize/press-defer services (blessed,
                    not rewritten -- feat_resize rides resize_item + resize_apply_edges with a
                    min floor re-anchored against the grabbed edge's pinned far side);
                    feat_collapse is the carved height tween (caller bool + keyed tween
                    scratch; same 0.2s ease_out_cubic feel and window_anim_enable preference
                    as stock chrome); feat_maximize (B) swaps rect <-> passed-in work area
                    with a caller-owned restore slot, tweened both ways, tracking a resizing
                    work area; feat_clamp (B) is the stock boundary policy over passed-in
                    bounds.  State rule held: in-flight = active_id singleton; persistent =
                    caller pointers; only tween scratch (edge latch + from-value) in the keyed
                    pool.  Hover gating reads the ambient scope, so features are called inside
                    the owning pane/window bracket.  GUI_RESIZE_L/R/T/B moved gui_internal.h
                    -> gui.h (public edge-mask vocabulary; GRIP stays internal).  Open latch
                    and scroll deliberately have NO mechanism (a caller bool / region_begin) --
                    documented in the section.  NOT yet re-seated: the stock window still runs
                    its own collapse/maximize/clamp code paths -- that swap is increment 9's
                    recipe work.  Acceptance demo: sb_gui_example "Windows > Feature Kit" --
                    the section-6 sketch live (pane + maximize/clamp shaping the rect,
                    collapse tween, dragging title band with cut-off buttons, R|B resize,
                    close as a plain static bool).  Full build green; 6s canary clean.
  9. RECIPE      -- gui_window_t becomes the stock RECIPE: one policy file assembling pane +
                    feat_* + titlebar composite; its persisted record shrinks toward the
                    policy fields chrome actually owns.  Behavior-identical by construction;
                    dock membership stays a system.
                    DONE 2026-07-21.  The stock window's duplicated mechanisms are gone; what
                    remains in window/ is policy.  (1) window_clamp wraps gui_feat_clamp (the
                    viewport + work-top resolve and the NO_BOUNDARY_CLAMP opt-out are the
                    policy; the geometry is the mechanism).  (2) The collapse height tween is
                    gui_feat_collapse, sampled EVERY begin -- pinned states discard the value
                    -- so its edge latch tracks win->collapsed; window_collapse_set slimmed to
                    flag + redraw.  (3) The max/min/restore rect channel is feat_pin, the
                    3-state GENERALIZATION of feat_maximize's core (state ordinal 0=normal /
                    1=work-area / 2=shelf-chip; public feat_maximize is now sugar over it):
                    entering a pin from normal saves *restore, pinned-to-pinned hops re-aim
                    without touching the save (= the old "minimized owns the restore" rule),
                    returning to 0 tweens back with a restoring latch, settled pins snap-track
                    live targets.  Setters slimmed to state flip + z/shelf policy + redraw.
                    Gesture gates ride feat_pin's tween-live return + feat_collapse_live (a
                    non-advancing GUI_STATE_PEEK of the timer slot -- a second gui_anim_timer
                    sample would double-step the clock).  Record SHRANK: state_anim, anim_from,
                    collapse_anim, collapse_from, shown_h deleted; norm is now gui_rect_t.
                    GUI_WIN_ANIM_SECS/window_anim_ease deleted -- FEAT_ANIM_SECS/feat_ease are
                    THE feel constants (dock's maximize ease re-pointed onto them; s_win_anim
                    preference unchanged).  FINDING -- next-channel stays policy: a staging
                    channel only exists because the RECORD owns geometry between frames;
                    caller-owned pane state needs no stage (you just write your variable), so
                    there is no mechanism to carve.  Shelf target (population/order) stayed
                    chrome per its C class; only its rect channel moved into feat_pin.
                    No public API change (vtable layout unchanged).  Full Debug build green;
                    6s canaries sb_gui_example + host_editor clean.
  10. TU SPLIT   -- finish section 7: per-library unity units in dependency order, done
                    LAST because increments 7-9 churn exactly the files (window/, interact/,
                    surface/) whose statics the split must extern.  Per-library, never
                    big-bang; the increment-4 blocker list is the checklist.
                    DONE 2026-07-21.  gui is now SIX translation units in one static lib:
                    gui.c (CORE + FRAME: core/surface/interact/present/user/frame +
                    gui_frame_overlay + gui_ui_mem + gui_api), gui_backend.c (draw),
                    element/gui_element.c, compose/gui_flow.c (FLOW), gui_chrome.c (CHROME:
                    widgets/table/window/dock/popup/nav), debug/gui_debug.c (DEBUG:
                    dashboard + stepper, severable).  Carved one library at a time (debug ->
                    flow -> chrome), each building green, seams discovered compiler-first
                    (C4013/C2065 harvest -> classify -> extern/declare).  gui_internal.h
                    gained the cross-unit sections: AMBIENT RECORDS (g_ctx, s_io, s_style,
                    s_interaction + s_build newly TYPED as gui_interaction_t / gui_build_t,
                    s_scope, s_layout_stack/sp, s_id_sp, s_replay_mode, s_popup_begin_count,
                    s_resize_* gesture scratch, s_next_win / s_vp_request newly typed,
                    s_font_size, s_fwd_caps) and ~120 SERVICE SEAMS grouped by owner (id /
                    io / keyed state + GUI_STATE macros / style resolution + the WIDGET_* +
                    ROUND_* + COL_* vocabulary macros moved from gui_style.c / lattice /
                    surface verbs + the z band map / item protocol + gate predicates /
                    paint helpers / gesture services / anim timer + gui_ease_fn / feat_pin
                    internals / flow's emit surface incl. REGION_PAD_DEFAULT / chrome's
                    frame steps).  Static-decl block gotcha: shared static forward decls in
                    gui_internal.h break any including TU that references without defining
                    them -- each converted to a real seam when first flagged.  Memory
                    accounting decentralized: gui_ui_memory now sums this unit + per-unit
                    seams (gui_flow/chrome/debug_unit_mem_bytes); style_stacks_empty()
                    added so the volatile-replay assert stops reaching into style
                    internals.  gui_host.h gained the public decls that had ridden unity
                    visibility (gui_split_*, gui_table_*, gui_volatile_*, gui_button_fill/
                    width).  orb.targets: gui AND gui_stress carry the three new unit
                    lines.  Upward calls are explicit and few: flow -> scrollbar_widget +
                    gui_anim_*; core/frame -> chrome's six frame steps (raise-on-press,
                    modal fence x2, popup close check, nav turnover, dock upkeep) + the
                    debug unit's two windows.  Full Debug build green (all targets incl.
                    gui_stress + mono ship); 6s canaries sb_gui_example + host_editor clean.
                    Docs: gui.c banner, GUI_ARCHITECTURE.md six-unit story.

Vtable note: additions/reorders change gui_api_t layout -- hot-reload hosts need a restart
at each increment that touches it (func_api_size discipline unchanged).  Increment 6 also
changed run_project_api_t and game_api_t layouts: all project DLLs and hosts rebuild together.

/*============================================================================================*/
