# The gui service, from the outside in

What you type at each rung of the gui, what sits underneath, and where the edges are.
`source/runtime_service/gui/GUI_ARCHITECTURE.md` is the inside-out companion for people
changing the service; this page is for people using it.  Code is the source of truth.

In one sentence: an immediate-mode 2D UI engine whose stock widget kit and window chrome
are ordinary clients of a strictly layered stack, so the same system is a batteries-included
editor toolkit at the top and a bring-your-own-look interaction engine underneath, with a
pure rect composer and a retained geometry cache built in.

---

## 1. The ladder

The `gui()` vtable reads in bands, one per section banner of `gui_api.h`.  Each band is
implemented purely in terms of the bands below it and never reaches up; a unit's include
list is its rung and the compiler enforces it.  `sb_gui_base` demonstrates each rung using
nothing above it.

| Band | What you type | What you get | Stop here when... |
|------|---------------|--------------|-------------------|
| CHROME | `window_begin( "Tools" )`, `button( "Save" )`, `combo`, `table_begin`, dockspaces, menus, popups | The editor toolkit: windows, docking, tabs, tables, stock widgets in the house look | You are building a tool or editor and want the house style |
| STOCK | `comp_button( id, rect )` -> state; `stock_button( rect, ... )` | Widget LOGIC (`comp_*`: hit test, drag math, snapping, focus) and one reference render (`stock_*`) over it. A user widget is the stock render's sibling over the same `comp_*` call | You own placement and look but want ready-made behavior |
| STYLE | `style_color( role, item_phase( state ) )`, `push_style_color`, `style_edit`, `style_source_set` | One role x phase color grid plus metrics; themes and kits compile down into it | You want the stock widgets wearing your palette |
| FLOW | `region_begin(...)`, `row( h )` / `cols_n( n )` / `stack()`, `flow_begin( rect )` / `flow_cell( w, h )` / `flow_end()`, `rows_clip` | A scrolling region and a layout engine that hands you cells to fill | You want automatic layout and scroll but no window chrome |
| RECT | `split`, `carve( form, area, ... )`, `anchor` | Pure math partitioning of a rect. Nothing drawn, no state | You know your sizes and just need geometry (HUD, canvas) |
| SURFACE | `pane_begin( id, rect, tier, vp, flags )`, `feat_move` / `feat_resize` / `feat_collapse` / `feat_maximize` / `feat_clamp` | A top-level occupant that competes for hover and z, plus window features as freestanding mechanisms over any id | You are building your own window manager |
| CORE | `item( id_str, rect )` -> `gui_item_state_t`, `push_id_int`, drag-and-drop, anim timers | Identity, hover/active arbitration, the interaction state machine over YOUR rect | You are building a genuinely custom widget |
| DRAW | `draw_rect`, `draw_text`, `draw_bezier_cubic`, paths, icons, sprites | A 2D batch renderer with three atlases. No ids, no layout, no style | You just want to paint into the frame |
| FRAME | `boot( desc )`, `frame_begin( dt )`, `render( vp, cmd )` | The loop, viewports, the GPU flush | You are wiring a host |

---

## 2. The one idea, seen four ways

**As a rule:** chrome is a client.  The stock widgets are written on the same substrate a
user widget uses.  If `button()` can do it, your widget can, because `button()` has no
vocabulary you lack.

**As a widget:** a leaf is four independent services, one line each.

```c
gui_rect_t       r  = gui()->flow_cell( 0, 0 );                       // placement (FLOW)
gui_item_state_t st = gui()->item( "save", r );                        // behavior  (CORE)
u32              bg = gui()->style_color( GUI_ROLE_BG,
                                          gui()->item_phase( st ) );   // look      (STYLE)
gui()->draw_rect( r.x, r.y, r.w, r.h, bg );                            // paint     (DRAW)
```

Swap any one line (your own paint, a rect from `carve`) and keep the rest.

**As a data split:** the style sorts every field by one question -- can reading it move a
rect?  Metrics (row heights, pads, gaps, borders) can, and are read by both the layout
engine and widgets that measure themselves.  Colors and shapes cannot, and are paint-only.

**As a loop property:** immediate mode with a retained twist.  Widgets re-emit every frame,
but geometry is cached per window; a frame whose command hash is unchanged with no input
skips emit entirely (`frame_begin` returns false).

---

## 3. Two ways to place things

**Rect composition** (RECT): single pass, known sizes, pure math, nothing drawn.

```c
static const f32 FORM[] = { GUI_CUT_X, 80.0f, 1.0f,             // sidebar | rest
                            GUI_CUT_Y, 28.0f, 1.0f, 28.0f,       //   header / body / footer
                            GUI_END, GUI_END };
gui_rect_t leaf[ 4 ];
gui()->carve( FORM, area, 6.0f, leaf, 4 );
```

**Flow layout** (FLOW): multi-pass, auto-sized, scrolls, stateful.  Flow owns the pen, the
highwater, scrollbars and the wheel claim; widgets never see the shape, they ask for the
next cell.

```c
gui()->region_begin( "list", x, y, w, h, GUI_REGION_MID, GUI_WIN_NONE );
gui()->row( 26.0f );
for ( int i = 0; i < n; ++i )
    gui()->stock_label( gui()->flow_cell( 0, 0 ), GUI_ALIGN_LEFT, rows[ i ] );
gui()->region_end();
```

One overloaded unit governs every size everywhere (tracks, splits, fits, packs): `t > 1`
pixels, `t == 1` fill, `0 < t < 1` fraction, `t == 0` natural, `t < 0` terminator.

**The seam:** `flow_begin( rect )` opens the layout engine inside any rect, including a
leaf that `carve` handed you; `flow_cell` takes one flow element back out as a rect;
`flow_end` resumes the outer producer.  It nests to any depth.  Scrolling is a region's
business, not flow's: open a region first, then flow inside it.

---

## 4. What is underneath

Two servers that never see each other, and libraries over them.  The **render server**
has no vertex or index buffer: one 16-byte quad record per shape sits in a storage buffer
the vertex stage pulls by `SV_VertexID`; rounded shapes are one quad whose fragment resolves
the boundary from a primitive record, so an effect never splits a batch; glyphs address a
stable glyph table by id, so a repacked atlas invalidates nothing.  Three atlases split by
what a texel means: R8 coverage (glyphs, icons; sampled NEAREST), RGBA sprite (authored art;
LINEAR), SDF (distance-field text).  The **interact server** owns ids, the keyed state
pool, the item protocol, the pane/z contest, nav, focus, capture, and animation.  Every
retained record lives in one static context struct so the whole thing hot-reloads.

Widget logic (`component/`) never paints and stops below the draw server; the stock set
(`stock/`) is logic plus one plain render; chrome (`chrome/`) is the product windowing
policy.  Kits sit above gui in their own homes: `editor/ed_kit.c` (the property row),
`project/sample_game/game_ui.c` and `sb_gui_diablo/ui.h` (game kits that install a palette
and cut rects).

---

## 5. Boundaries

These are the shape of the design, not bugs.

- **Text is Latin, one font per run.**  No shaping, bidi, RTL, or IME.  Extended
  characters and UTF-8 editing exist (`sb_gui_utf`); a text-shaping subsystem does not.
- **No sub-pixel text motion.**  The coverage atlas samples NEAREST; glyph positions snap
  to pixels.  Widgets animate; text does not slide.
- **No accessibility tree.**  The id namespace and debug-name registry are the only seed.
- **No declarative or visual authoring.**  Layout is C.  The layout editor is a future
  item (README.md section 3).
- **Windows-first.**  Win32 + Vulkan; no web or mobile backend.
- **Chrome's widgets are bespoke.**  `chrome/` makes no `comp_*` calls yet; the
  components its widgets would need (`comp_drag`, `comp_scrollbar`, `comp_tab`) do not
  exist.  Below chrome, a rect-taking combo or tree is composed by hand from `item` +
  `draw_*`.

---

## 6. Where to look

    sb_gui_example    feature explorer: end-to-end demos of every feature group
    sb_gui_base       one rung per number key, each using nothing above it
    sb_gui_diablo     a game HUD/menu shell with no chrome: rect cuts + stock renders
    sb_gui_editor     an editor shell: dockspace, panels, viewport, play mode
    sb_gui_test       headless assertions (packing, rect kit, caret math, font lookup)
    sb_gui_bench / sb_gui_stress / sb_gui_render    performance and the render matrix
