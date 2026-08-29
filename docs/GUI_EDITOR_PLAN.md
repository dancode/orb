# GUI Editor Plan -- v1.0

A WYSIWYG UI layout editor built on the ORB immediate-mode GUI: what it is, why the IMGUI
model makes it easier rather than harder, which substrate already exists, what still has to be
answered, and the order to build it in.

Status: **investigation complete, implementation not started.** This document is the starting
point to return to, not a record of work done.

Substrate citations were verified against the tree on 2026-08-28. Line numbers drift; treat
the function and file names as the durable part.

---

## 1. Verdict

Building this is a clear path, not a swamp. Every mechanism the editor needs is already
present in the public `gui()` vtable. **No engine changes are required to reach a working
stage 2.**

The cost is roughly 2000-4000 lines of unglamorous code -- document model, undo,
serialization, property panel -- none of which carries research risk. For scale:
`source/runtime_service/gui` is ~71,000 lines and `sb_gui_editor` alone is ~2,300.

The risk in this project is **not** GUI integration. It is scope creep past layout. Section 11
draws that line explicitly.

---

## 2. The inversion that makes this tractable

In a retained toolkit a WYSIWYG designer is hard because widgets are objects with lifetimes.
Unreal's UMG Designer has to instantiate real Slate widgets in a preview mode and maintain a
parallel designer hierarchy with proxy wrappers, because the thing being edited and the thing
being displayed are two different object graphs that must be kept in sync.

In an immediate-mode GUI there is no such split. The designer surface and the live UI are the
same function called with a flag:

```c
void ui_emit( ui_doc_t* d, i32 node, ui_design_t* dz );   /* dz == NULL at runtime */
```

Nothing is instantiated, nothing has a lifetime, and the frame is discarded and rebuilt. The
preview cannot drift from the document because it *is* the document, re-walked.

The difficulty inverts rather than disappearing. A retained toolkit hands you widget objects,
a property system, serialization and a transaction stack for free, so its designer is mostly
glue. Here the preview is nearly free and those four things are yours to write. That is a good
trade: the free part is the part with design risk, and the written part is boring.

---

## 3. Substrate audit -- what already exists

This is the highest-value section to re-read before starting. Every row was confirmed present.

### 3.1 Load-bearing mechanisms

| Need | Mechanism | Location |
|------|-----------|----------|
| Get the rect a widget just occupied | `gui()->get_item_rect()` | `core/gui_query.c:157` |
| Dictate a widget's rect from a tree | `gui()->next_item_rect()` -- consumes no pen, grows no highwater | `flow/gui_layout_core.c:1181` |
| Hand a resolved box to immediate widgets | `flow_begin(rect)` / `push_layout_overlay` | `flow/gui_sublayout.c:86-102` |
| Paint normally, interact with nothing | `region_begin( ..., GUI_WIN_NO_INPUT )` | `flow/gui_region.c:132` |
| Paint a widget in an arbitrary phase | `draw_face_item( r, GUI_ID_NONE, st, sel )` | `stock/gui_face.c:204` |
| Stable identity across rename and reorder | `push_id_int( uid )`, or the `###key` label form | `core/gui_id.c:101-150` |
| Foreground layer above popups | `GUI_REGION_FG` + `NO_INPUT` + `NO_CLIP` | `gui.h:2642`, `flow/gui_pane.c:70-93` |
| Draw over widgets emitted later | mark-and-paint-later (`ring_mark_t` idiom) | `stock/gui_adornment.c:167-256` |
| Drag and drop protocol | `drag_source_begin` / `drag_target_begin` / `drag_payload_accept` | `gui_api.h` CORE block |
| Load, dedup, mtime-watch, hot-reload a file | `asset()->type_register` + `asset()->refresh()` | `runtime_service/asset/asset_api.h:32,54` |
| Working template for the above | `sb_asset_shader` re-cooks and picks up mid-run | `sandbox/runtime/sb_asset_shader/` |

`GUI_WIN_NO_INPUT` is the single most important entry in this table. A region carrying it
never calls `surface_hover_nominate`, so it never wins `hover_win`, so every `item_state`
inside it fails the `win_hover` gate and returns a zeroed state -- **at full opacity, with
appearance completely unchanged**. That is the design-mode fence, and it ships today.

Do not reach for `disabled_begin` instead. It forces the IDLE phase and multiplies the draw
alpha by `GUI_VAR_DISABLED_ALPHA` (0.5 by default, `style/gui_theme.c:298`), so the canvas
would render dimmed and could never show a hover or pressed state.

### 3.2 Useful precedents to copy

| Pattern | Where | Why it matters here |
|---------|-------|---------------------|
| Layout-tree text serializer | `chrome/dock/gui_dock_serialize.c` (235 lines) | Pre-order line stream, explicit stable ids, positional shape, corrupt-blob healing, between-frames safe-point rule. The closest existing thing to what the `.orb_ui` format must be. |
| Whole-document snapshot and restore | `sb_gui_editor` play mode, `ed.h:136` | The memento shape that undo generalizes. |
| Hand-written inspector by kind | `sb_gui_editor/ed_panels.c:94-181` | The `switch (kind)` property panel shape. |
| Property row idiom | `editor/ed_kit.c:17-52` | The emit target for inspector rows. `ED_PROP_LABEL_FRAC` 0.38. |
| Hierarchy with context menu + drop target | `sb_gui_editor/ed_panels.c:36-85` | `push_id_int(i)` + `selectable` + `popup_context_item_begin`. |
| Hash the document to detect change | `sb_gui_editor/ed_viewport.c:227-260` | One FNV over a small pool beats instrumenting every mutation site. |
| Input-agnostic gesture controller | `sb_gui_editor/ed_viewcam.h:7-11,92-101` | Click-vs-drag arbitration with suppression flags, never touching `gui()` or editor state. The right shape for a 2D manipulation controller. |

### 3.3 What does NOT exist

| Missing | Consequence |
|---------|-------------|
| Any point-to-item query | Build your own rect table and reverse-walk it. This is what you want anyway -- see section 6. |
| A reflection-to-gui bridge | Zero `ref_` calls anywhere in `gui/` or `editor/`. A generic property grid is unwritten. See section 9. |
| Document-level undo | The only undo in the repo is one global text-widget ring (`interact/gui_edit.c:155`). |
| Any gizmo or manipulator | Zero hits repo-wide. The 2D handle set is written from scratch. |
| A text serializer over `ref_` | `ref_serialize.c` is raw-memcpy binary that rejects on any schema change; `ref_print.c` is write-only diagnostics. |
| Any JSON / INI / key-value parser | Nearest reusable model is the `orb.targets` reader, `tools/build_tool/build_tool_03_registry.c`. |
| A panel registry in `source/editor/` | Acknowledged as pending in `editor/editor_api.c:21-23` -- registry, selection and undo "land with the world". |

---

## 4. The document model

**The document is ours, and it is a flat POD array.**

```c
typedef struct ui_node_t
{
    u32  uid;              // stable identity; never reused within a document
    u16  kind;             // ui_kind_t -- button, label, row, cols, grid, pack, ...
    u16  parent;           // index, UI_NODE_NONE for root
    u16  first_child;      // index
    u16  next_sibling;     // index
    f32  unit_w, unit_h;   // the overloaded unit; see section 7
    char name[32];         // author-facing label
    ui_props_t props;      // per-kind payload, a union keyed on kind
} ui_node_t;

typedef struct ui_doc_t
{
    ui_node_t nodes[ UI_DOC_MAX_NODES ];
    u16       count;
    u32       next_uid;
    u16       root;
} ui_doc_t;
```

Children are addressed by **index, not pointer**. That single decision buys the whole undo
system: the document is a contiguous POD block, so

```c
memcpy( &ring[ slot ], doc, sizeof( ui_doc_t ) );
```

is a complete, correct undo step. No command pattern, no inverse operations, no partial-apply
bugs, no action objects to keep in sync with new node kinds.

This is the right call specifically because UI documents are small. At 512 nodes a snapshot is
in the tens of kilobytes and a 64-deep ring is a couple of megabytes -- cheaper than the bug
surface of a command log. Revisit only if a document ever needs thousands of nodes.

`uid` is separate from array index on purpose. Indices move when nodes are spliced; uids do
not, and uids are what widget identity and the serialized file key on.

---

## 5. The interpreter

One recursive function serves both runtime and designer:

```c
static void ui_emit_node( ui_doc_t* d, u16 n, ui_design_t* dz )
{
    ui_node_t* node = &d->nodes[ n ];

    gui()->push_id_int( (i32)node->uid );

    switch ( node->kind )
    {
        case UI_KIND_BUTTON: emit_button( node ); break;
        case UI_KIND_ROW:    emit_row   ( d, n, dz ); break;
        /* ... */
    }

    if ( dz ) dz->rect[ n ] = gui()->get_item_rect();

    gui()->pop_id();
}
```

That is the entire renderer. Container kinds open a flow template and recurse over children;
leaf kinds emit a stock widget.

`dz == NULL` is the shipping path. It costs one predictable branch per node and no design-time
state whatsoever, which is what makes shipping the interpreter acceptable if C export is never
built (section 8).

---

## 6. Design mode is three deltas, never a fork

The discipline that keeps this from becoming a second widget library: design mode adds three
things to the interpreter above and changes nothing else. There is no parallel set of
"designer widgets".

**Delta 1 -- stable ids.** `push_id_int( node->uid )` around every node.

This is not optional. `item_id()` hashes the entire label string (`core/gui_id.c:145`), so a
user renaming a node from "Save" to "Save As" silently produces a different widget id and
discards that widget's hover, focus and keyed state. Deriving ids from author-visible text is
the classic failure here. Key on `uid`, or use the `"%s###%u"` form where the `###` re-roots
the hash at the stable suffix.

Note also that keying on **sibling index** is wrong for the same reason in the other
direction: reordering siblings renames every one of them. `sb_gui_editor/ed_panels.c:36` uses
`push_id_int(i)` because its entity list never reorders; a layout tree does.

**Delta 2 -- capture rects.** `dz->rect[n] = gui()->get_item_rect()` after each emit.

Do not try to use the engine's nav item list (`core/gui_nav_item.c`) as a ready-made item
table. It is gated three ways -- only the nav window registers, only when `nav.reg_all` is
set, and `nav.skip` items opt out entirely -- so it is not a complete per-frame table. One
`get_item_rect` call per node is cheaper and has none of those caveats.

**Delta 3 -- fence the canvas.** Wrap the preview in `region_begin( id, x, y, w, h, tier,
GUI_WIN_NO_INPUT )`.

Everything inside paints exactly as it would at runtime and responds to nothing. The editor's
own chrome -- selection handles, hierarchy, inspector -- lives in separate regions with input
on.

Where the designer must *show* a node as hovered, pressed or focused without it actually being
so (a state preview toggle in the inspector), `draw_face_item( r, GUI_ID_NONE, synthetic, sel
)` paints any phase with zero interaction. Passing `GUI_ID_NONE` also opts out of the
animation damper, so the preview is static rather than easing.

### Picking is ours, not the engine's

There is no point-to-item query in the GUI, and the editor should not want one. Reverse-walk
the captured rect table -- last emitted wins, which matches the engine's own within-window
resolution order -- and take the deepest node whose rect contains the point.

Doing it ourselves is strictly better than an engine query would be, because the editor needs
semantics the engine has no reason to offer: alt-click walking up the ancestry chain, picking
a container rather than its child, ignoring nodes the current filter hides, and marking
scrolled-out nodes unpickable.

---

## 7. Manipulation must speak the layout model's vocabulary

**This is the trap that sinks in-house UI editors.** In a flow-layout system a widget cannot
be dragged to an arbitrary (x, y). Building an editor that pretends otherwise --
free-positioning that then has to be reverse-fitted into flow -- is where the complexity
becomes unbounded.

The rule: every gesture edits a value the layout model already has a name for.

| Gesture | Edits | Existing machinery to reuse |
|---------|-------|-----------------------------|
| Drag node onto another | `parent` / sibling order; splice the array | `drag_source_begin` / `drag_target_begin`; insertion index computed from the rect table, drawn as a caret |
| Drag a widget edge | one `f32` unit | Same math as table column pair-resize, `flow/gui_table_engine.c` |
| Drag a track boundary in COLUMNS | the two adjacent units | As above; this gesture *is* a splitter drag |
| Drag inside an anchor container | `gui_anchor_t` fields (min, max, pivot, off) | `gui()->anchor()`, `gui_api.h:1670-1677` |
| Resize the root | document canvas size | Editor-owned |

Free positioning exists only in anchor mode, and that is honest rather than a limitation: it
is the one layout family whose model actually expresses arbitrary placement.

**ORB is a better editor target than Slate on exactly this point.** Because of the overloaded
unit (`gui.h:1131-1159`) every one of those drags edits a single `f32` whose meaning is
uniform across every container type, every axis and every consumer. The equivalent gesture in
Slate edits some combination of `SSizeParam`, padding, `HAlign` and `VAlign` -- four property
types needing four different inspector treatments and four different drag behaviours.

The inspector must therefore ship a **unit editor**, not a float field: a four-way mode toggle
(pixels / fill / fraction / natural) plus a value spinner that is disabled in fill and natural
mode. Getting this one control right is worth more to the tool's feel than any other single
widget.

### Selection chrome

Two options, both existing:

1. **Mark-and-paint-later**, copying `ring_mark_t` (`stock/gui_adornment.c:180-256`). Collect
   handle rects during the tree walk, paint them after the body flush, re-pushing each mark's
   saved clip so a handle on a node inside a scrolled child stays bounded by that child. The
   header comment at `:167-178` describes this exact problem.
2. **A foreground overlay region** at `GUI_REGION_FG` with `NO_INPUT | NO_CLIP`, which paints
   above every popup and clips to nothing. Correct for handles that must escape the canvas
   clip.

Expect to use (1) for in-canvas decoration and (2) for drag ghosts and rubber-band selection.

---

## 8. Persistence

**Data file first. Generated C is the graduation path, not the starting point.**

### The runtime path exists today

`asset()->type_register( "orb_ui", exts, load_fn, unload_fn, ud )` registers the format from a
DLL without touching the asset service; `asset()->acquire( vpath )` gives a refcounted,
deduped id; `asset()->refresh()` polls mtimes and reloads what changed, preserving id and
refcount. `sb_asset_shader` is a line-for-line working template for the whole loop.

`fs_blob_t` carries a hidden trailing NUL with `size` excluding it, so a text payload is
usable as a C string with no extra copy. DIR mounts override ZIP mounts by priority, so a
loose `layout.orb_ui` on disk shadows the shipped bundle -- exactly the dev workflow the
editor wants.

Note `refresh()` is currently called only from sandboxes; a production host must wire it, a
few times a second.

### The format

Copy the dock serializer's schema shape. It is 235 lines and it solves this exact problem
well:

- Pre-order line stream, one record per line, no lookahead in the parser.
- **Ids stored explicitly, never re-hashed from the name.** The dock format's comment at
  `gui_dock_serialize.c:16-17` explains why, and the reason applies identically to node uids.
- Tree shape carried positionally; runtime handles reassigned on load.
- Corrupt-blob healing rather than hard failure -- a malformed node collapses and the
  survivor takes its slot, preserving the container invariants.
- An explicit **safe-point rule**: load frees and rebuilds the tree, so it runs between
  frames or at the top of the build, never inside a body.
  `sb_gui_editor/ed_shell.c:23,200-207` shows the deferred-flag discipline that enforces it.

Do **not** build on `ref_serialize.c`. It is a raw `memcpy` of `sizeof(T)` gated on an exact
`schema_hash` match, so adding one field to `ui_node_t` invalidates every saved document.

### C export

Emitting a C function per document is attractive: it removes the interpreter from shipped
builds and lets hand-written code take over a screen once its layout settles.

**One-way only. Never round-trip.** Parsing generated C back into the document is a
compiler-grade problem and it is where this feature would consume the project. UMG does not
round-trip either. The `.orb_ui` file stays the single source of truth; the `.c` is a build
artifact.

---

## 9. Property panel: hand-written first, reflection later

The reflex is to point `ref_` at `ui_node_t` and get a generic property grid. Resist it for
v1.

Three reasons:

1. **The bridge does not exist.** There are zero `ref_` calls in `gui/` or `editor/`. Someone
   has to write the `ref_field_t` to widget dispatch from nothing, and that is a subsystem,
   not a panel.
2. **A generic field is worse UX here.** Reflection over `f32 unit_w` yields a float slider.
   What the tool needs is the four-mode unit editor from section 7, which no amount of field
   metadata will produce.
3. **The string pool is a real ceiling.** `REF_STRING_POOL_SIZE` is 16 KB (`ref.c:28`) and
   overflow is a hard FATAL (`ref.c:50-55`). It holds every type name, field name, attribute
   name and string attribute value in the process. A widget schema with hundreds of named
   fields plus `display_name` and `tooltip` strings is a plausible way to hit it.

A `switch (kind)` panel is roughly 300 lines, and `ed_panels.c:94-181` already shows the
shape: early-out on no selection, identity block, then a per-kind component block. `ed_kit`'s
prop row is the emit target.

Reflection earns its place later, when node kinds come from outside this file -- a game module
registering its own widget types. At that point `each_field`, `field_get_attr` and the
reserved editor attributes (`REF_AF_DISPLAY_NAME`, `REF_AF_TOOLTIP`, `REF_AF_CATEGORY`,
`REF_AF_CLAMP`, `REF_AF_STEP`, all already defined at `ref.h:218`) are exactly right. Note the
deliberate hook already named in `game/framework/world.h:179`: `comp_type_id` is commented
"for inspectors/serializers".

---

## 10. Open investigations

These are the questions to answer before or during stage 1. None looks fatal; several will
change the shape of the code.

**I1 -- Container rects.** `get_item_rect()` returns the last *item's* rect. A container spans
many emit calls, so its rect needs capturing at region begin and end. Determine whether an
existing query returns the region's own rect, or whether the interpreter must remember the
rect it passed into `flow_begin`. The latter is probably sufficient and costs nothing.

**I2 -- Nested regions under NO_INPUT.** Confirm that a child region opened *inside* a
`GUI_WIN_NO_INPUT` region inherits the deafness. If the flag does not propagate, every
container kind in the interpreter must pass it down explicitly, which is fine but must be
deliberate.

**I3 -- Scroll conflict.** The design canvas wants to pan and zoom. The document being
previewed may itself contain scroll regions. Decide which owns the wheel. Likely answer: the
canvas pans only on a modifier or middle-drag, and inner regions scroll normally so the author
can see the scrolled state. Check the sticky wheel-target behaviour before designing this.

**I4 -- Zoom.** Is there a transform seam that scales an entire emitted subtree, or must zoom
be implemented by scaling the DPI/style scale for the canvas region? `dpi_set` and
`scale_push` exist; determine whether either can be scoped to one region without disturbing
the editor chrome around it. This materially affects whether zoom is cheap or a campaign.

**I5 -- Sort key from outside the gui unit.** `draw_set_sort_key` is an internal render-server
seam, not on the public vtable. Confirm that `GUI_REGION_FG` + `NO_CLIP` is sufficient for all
overlay needs from a host-side editor, or decide to promote `draw_set_sort_key` to the vtable.

**I6 -- Natural sizing warm-up.** Content-driven track widths resolve from the previous
frame's measure. Setting `GUI_VAR_ANIM_SIZE` to 0 in design mode makes the damper snap, but
confirm that a newly added node still resolves correctly on frame two rather than needing a
hidden warm frame. Matters for "add a widget and immediately drag its edge".

**I7 -- Keyboard routing.** Read the four-tier model at `core/gui_query.c:32-75` in full
before adding any editor hotkey, and route every read through `gui()->is_key_pressed()`, never
`app()->key_pressed()`. Determine which tier delete / duplicate / undo belong in, given that a
text field in the inspector must win over them while focused.

**I8 -- Multi-viewport.** Can the design canvas live in a second OS window? The viewport pool
is global and `APP_WIN_MAX` is 4. Worth knowing early; it changes nothing structural if the
answer is yes.

**I9 -- Undo granularity.** A drag produces a value change per frame. Decide the coalescing
rule -- almost certainly "one undo step per completed gesture", pushed on mouse release,
matching the char-insert coalescing already in `gui_edit.c:667-674`.

**I10 -- Document root.** Does the preview include window chrome (title bar, close box), or is
the document always a region's contents? Decide early; it determines whether `ui_kind_t` has a
window kind and whether the canvas draws a simulated title bar.

**I11 -- uid collision with widget ids.** `push_id_int( uid )` combines against the ambient
seed, so two documents open in two canvases cannot collide. Confirm, and confirm that a
document loaded twice in one frame (a prefab preview) is safely distinguished.

---

## 11. Scope boundaries

Holding these is what keeps the estimate honest. Each is a place where a UI editor can absorb
unlimited effort.

**Behaviour and data binding -- OUT.** The editor produces layout plus *named callback slots*.
What a button does stays in C, resolved by name at load. **Do not build a visual scripting
graph.** This is the single most important boundary in the document.

**Prefabs and instancing -- DEFERRED.** A node that references another document is where every
UI editor gets complicated: override semantics, nested overrides, propagation on edit. Design
the file format so it does not *preclude* prefabs (a node kind with a document reference), but
build nothing.

**Styling authorship -- OUT for v1.** The editor selects an existing theme and existing style
roles. Authoring new themes belongs to the separate style editor, which already exists in some
form (`project_gui_style_editor`).

**Animation authorship -- OUT.** Node property animation is a timeline editor. Different tool.

**Localization -- OUT**, consistent with the GUI itself having none.

**Round-tripping generated C -- REJECTED.** See section 8. Do not revive this; it converts a
tool project into a compiler project.

---

## 12. Staged build

Each stage is independently useful and independently abandonable. The kill criterion for the
whole project is stage 2: if selecting a node by clicking the canvas does not feel immediate
and obvious, the interaction model is wrong and no amount of stage 3-6 work rescues it.

| Stage | Content | Outcome | Rough size |
|-------|---------|---------|-----------|
| **1** | `ui_doc_t` + `ui_emit_node` + a hardcoded tree rendered live | Concept proven end to end | 300-500 lines |
| **2** | Rect table, `NO_INPUT` fence, click-to-select, selection overlay, hierarchy panel | **The WYSIWYG moment. Kill gate.** | 400-600 |
| **3** | Inspector with the four-mode unit editor and per-kind property blocks | Editable | 400-600 |
| **4** | Drag-to-reorder with insertion carets; edge-drag units; anchor-mode free drag | Direct manipulation | 500-800 |
| **5** | `.orb_ui` save/load, asset hot-reload wiring, undo snapshot ring | A usable tool | 400-600 |
| **6** | One-way C export | Ships without the interpreter | 300-500 |

Stage 1 is worth doing as a sandbox target (`sb_gui_uiedit`, following the
`source/sandbox/gui/<target>` convention) rather than inside `source/editor/`. The real editor
has no panel registry, selection model or undo yet -- `editor/editor_api.c:21-23` flags all
three as pending -- so building against it now means building two things at once. Graduate the
tool into `source/editor/` once the registry lands with the world.

---

## 13. Risks

**R1 -- Manipulation drifts toward free positioning.** The most likely failure. Every "just
let me nudge it two pixels" request pulls toward a coordinate model the layout engine does not
have. Mitigation: section 7 is a rule, not a preference. Nudging edits the unit.

**R2 -- The unit editor is mediocre.** If the four-mode control is clumsy, the whole tool
feels clumsy, because it is the control the author touches most. Mitigation: prototype it in
stage 3 in isolation and iterate before wiring it to anything.

**R3 -- Scope creep into behaviour.** Section 11. The pressure is constant and the boundary is
arbitrary-looking from outside. Write the callback-slot mechanism early so the answer to "how
does the button do something" is a demo, not a refusal.

**R4 -- Fixed caps bite silently.** `UI_DOC_MAX_NODES` overflow must be a visible editor
error, not dropped nodes. Note the known hazard that GUI pool overflow kills a Debug process
silently (`project_gui_bench_suite`); the document pool must not inherit that behaviour.

**R5 -- The interpreter and hand-written UI diverge.** Once C export exists there are two ways
to build a screen and they can drift. Mitigation: export is one-way and the `.orb_ui` file
remains the source of truth; an exported screen is a leaf, not a thing to re-import.

---

## 14. Summary

The path is clear, and it is clear for a specific structural reason: an immediate-mode GUI
makes the preview surface free, and ORB's public API already contains every mechanism the
editor needs -- rect capture, rect dictation, an input fence that does not alter appearance,
arbitrary phase painting, a foreground overlay layer, a paint-later idiom, drag-and-drop, and
a hot-reload asset path with a working template.

What must be written is a flat POD document, a snapshot undo ring, a line-oriented serializer
modelled on the dock format, a hand-written inspector, and a manipulation controller whose
every gesture edits a value the layout model already names.

Three disciplines decide whether this stays a few weeks of work or becomes a campaign:

1. The document is a flat POD array we own, so undo is a memcpy.
2. Design mode is three deltas on one interpreter, never a forked widget path.
3. Manipulation edits layout-model values, never free pixels -- except in anchor mode, where
   free pixels are what the model means.

Break any one of the three and the project becomes a swamp. Hold all three and the remaining
work carries no research risk.
