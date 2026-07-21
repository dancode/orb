# GUI Stack Plan -- from one grab bag to a stack of small libraries

Status: DRAFT (2026-07-21).  Companion to the rect-first campaign proven by sb_gui_diablo.
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

## 5. Public faces

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

## 6. Migration increments (each builds + runs)

  1. SKELETON    -- write the library headers as curated declaration groupings (no code
                    motion); resection gui_api.h to match.  Pure reorganization.
  2. SEAMS       -- add flow_begin/flow_cell/flow_end over push_layout_overlay/empty;
                    depth-verify with a sandbox check running the canonical nesting above.
  3. ELEMENT     -- stand up gui_element: lift sb_gui_diablo ui.c cores + the internal
                    cell_/item_/draw_ widget seams; add gui_el_style_t; chrome installs it
                    from the active theme.  sb_gui_diablo/ui.h shrinks to a game kit.
  4. TU SPLIT    -- 2 unity units -> per-library units; include order = dependency graph.
  5. STYLE PURGE -- stock widgets read element-shaped values through the installed
                    gui_el_style_t; per-widget theme slots retire into chrome tokens.
  6. KITS        -- editor kit and sample game kit move to their own homes as the reference
                    consumers; optional: split real build targets.

Vtable note: additions/reorders change gui_api_t layout -- hot-reload hosts need a restart
at each increment that touches it (func_api_size discipline unchanged).

/*============================================================================================*/
