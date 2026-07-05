# GUI Doc/Comment Audit -- Campaign Plan & Progress

A verification-driven audit of every comment in `gui`. NOT a blind rewrite: reconcile each
comment claim against the code it describes, fix drift, fill gaps, apply COMMENT_CONTRACT.md,
and leave correct comments alone. Correct comments encode hard-won mental model -- do not churn
them.

Why an audit and not a rewrite: the survey (2026) found the prose is high quality; the problem is
DRIFT (comments accurate when written, features landed, comments never moved) plus a few gaps.
46 forward-looking comments, several confirmed stale (docking + multi-context both landed), one
flat factual error (the `_alloc` "static default context" line). See ARCHITECTURE.md sec 1-8.

## The load-bearing discipline

Verification is the expensive, non-skippable step. Trace each claim to implementing code, not to a
neighboring comment. A chunk rushed into paraphrase-mode makes things worse than doing nothing.

## Phases

- [x] **Phase 0 -- Standard.** COMMENT_CONTRACT.md written.
- [x] **Phase 1 -- Verify the anchor.** DONE 2026-07-04. Every load-bearing ARCHITECTURE.md claim
      traced to implementing code (not comments); all pass, no corrections to the anchor:
      - s_frame_dirty = s_force_redraw || io_dirty() || s_retained.wants_redraw ||
        gui_build_any_changed()  (gui_frame.c:457) -- exact; doc notes the core formula, code also
        force-trues on deferred font flush + active debug overlay (both documented elsewhere).
      - GUI_STATE_CAP=24 (gui_internal.h:57); gui_region_t = scroll_link(16)+user_w/h(8) = exactly
        24 -- "largest state struct" claim is tight and correct.
      - GUI_POPUP_Z_BASE=0x80000000u (gui_internal.h:153); GUI_MAX_VIEWPORTS=4 == APP_WIN_MAX ==
        RHI_CTX_MAX (gui_internal.h:63).
      - s_frame_built guard cleared in frame_begin (gui_build_frame_reset), BUILD lazy on first
        render() (gui_build_cache.c:719).
      - interaction_frame_reset called once in frame_begin (gui_frame.c:489), defined gui_ctx.c:645.
      - Tier-1 s_interaction/s_replay_mode in gui_ctx.c, s_io in gui_input.c; Tier-3 s_tess in the
        render-backend TU (gui_build_tess.c:88).
      - Segment key = draw_seg_retag(win, z, vp, font, band) (gui_emit_draw.c:285) -- all 5 axes.
- [x] **Phase 2 -- Cross-cutting sweeps.** DONE 2026-07-04.
      - [x] Resolved the forward-looking comments. KEY LESSON: most "seam" hits are the codebase's
            NOUN for a shared code junction (present-tense architecture) -- NOT the contract's
            `SEAM:` tag; left untouched. Actual fixes below.
      - [x] Filled the gui.h header banner: overview pointing to ARCHITECTURE.md + cleaned the
            informal "Noteable Optimizations" note (typos AND two non-ASCII em-dashes removed).
- [x] **Phase 3 -- Per-subsystem audit (bottom-up, one reviewable chunk each).** COMPLETE 2026-07-04.
      Per-chunk checklist: inventory -> classify (correct/drift/stale-forward/vague/redundant/
      missing-invariant) -> verify each claim by tracing code -> fix -> build-verify unity TU ->
      stop for review.
      - [x] gui_internal.h  DONE 2026-07-04.  The headline flat error fixed: `_alloc` line 685
            "NULL for the static default context (slot 0)" -> corrected.  Traced both ends:
            ctx_alloc_slot sets `ctx->_alloc = blk` unconditionally (gui_ctx.c:467), slot 0 goes
            through it (gui_ctx.c:481), and BOTH teardown paths free it -- gui_frame.c:121 frees
            s_ctx_pool[i]->_alloc for every slot at shutdown, gui_ctx_destroy frees c->_alloc
            (gui_frame.c:1217).  `_alloc` is never NULL.  Also tightened one residual "no nesting
            yet" (line 700) to present tense (it duplicated the FUTURE: nested-tables note already
            added above it in Phase 2).  Rest of the header verified clean -- it was heavily audited
            in Phase 2 (dock_root, col.label, "stay global", nested-tables FUTURE: tag all already
            present-tense/tagged).
      - [x] core/  DONE 2026-07-04.  (PILOT core/gui_ctx*.c done earlier; rest done now.)
            - [x] gui_ctx.c    (4 drift fixes: gui_window_t location, multi-context tense,
                                 gui_retained_t "will become" tense, slot-0 free contradiction)
            - [x] gui_ctx_id.c (clean; reclamation gate + salt verified)
            - [x] gui_ctx_io.c (clean; all accessors match)
            - [x] gui_symbol.c      (2: draw_shadow "still...a future" -> FUTURE: tag [verified rings
                                      at :474]; "a future style lands once" -> present rationale)
            - [x] gui_layout_core.c (2: stale "future one-shot size overrides" -> the one-shot fit_next
                                      [landed as cell_fit_resolve, live from grid+flow]; ORPHANED
                                      widget_split_label header comment [separated from its fn by an
                                      inserted fn] relocated + its widget list "future combo/drag/color"
                                      -> present [all three land via widget_split_label callers])
            - [x] gui_resize.c      (4: internal CONTRADICTION -- header says s_resize_* "lives here"
                                      [true, defined :50-52], section comment said "lives in gui_window.c"
                                      -> fixed; 3x child_begin attributed to gui_layout.c -> gui_layout_child.c
                                      [child_begin + all resize calls live there, confirmed vs gui.c manifest])
            - [x] gui_style.c       (1: PHANTOM symbol k_col_default -- defined NOWHERE, refactored into
                                      the k_themes registry [dark/light]; base is s_style.colors from the
                                      active theme, not a constant)
            - [x] gui_widget_core.c (1: same PHANTOM k_col_default -> active theme k_themes/gui_theme.c)
            - [x] gui_layout.c      (1: header typo "gui_begin/child_end" -> gui_child_begin/child_end)
            - [x] gui_anim.c gui_stacks.c gui_region.c gui_theme.c gui_input.c gui_layout_region.c
                  gui_layout_child.c  (all CLEAN -- headers/cross-refs/claims verified vs code; gui_region.c
                  was Phase-2 done)
      - [x] backend/pipeline/  DONE 2026-07-04.  5 comment fixes, all one drift class: the command
            SEGMENT key gained `font` then `band` axes over time (band from the arena-bands work),
            but several descriptive comments still named the old 3-axis (win,z,vp) key.  Verified
            the live key is 5 axes: draw_seg_retag tests win/z/vp/font/band (gui_emit_draw.c:287-288).
            - [x] gui_emit_draw.c   (4: segs[] header block "per-(win,z,vp,font)" -> +band; segs[]
                                      field inline "per-(z,vp)" -> per-(win,z,vp,font,band); seg_count
                                      inline "segs[0] is z=0,vp=0" -> "the bg span"; draw_seg_retag fn
                                      header "(win,z,vp)" -> "(win,z,vp,font,band)" + note each setter
                                      passes the other four axes through)
            - [x] gui_build_cache.c (1: pipeline-map header EMIT line "per-(win,z,vp) segments" ->
                                      per-(win,z,vp,font,band); rest of the 989-line file verified
                                      clean -- the stats/idle-skip/band-major/set_stable prose all
                                      traces exactly, best-maintained file in the chunk)
            - [x] gui_emit_path.c gui_build_tess.c gui_build_volatile.c gui_render.c gui_shader.h
                  (all CLEAN -- traced headers/cross-refs/fn attributions vs code.  Checked:
                  tess_stroke_poly_aa/tess_axis_line/tess_dashed_line exist (gui_build_tess.c);
                  gui_font_internal.c owns font_init (referenced by gui_render.c:267); gui_shader.h
                  GLSL paraphrase matches shaders/gui.frag exactly -- dbg_tint decode, sRGB-linear,
                  s.r coverage.  gui_build_volatile.c's long design header is exemplary + accurate.)
      - [x] backend/resource/  DONE 2026-07-04.  ZERO fixes -- first fully-clean chunk.  All 6 files
            (gui_atlas.h/.c, gui_font.h/.c, gui_font_internal.c, gui_icon.c) traced clean: headers,
            cross-refs, and fn attributions all match code.  Verified: draw_push_icon (pipeline/
            gui_emit_draw.c:629) reads icon_get/icon_atlas_idx as the header claims; the "asset
            pipeline later" notes (gui_atlas.h:14, gui_icon.c:13) are legit scoped non-goals, not
            stale promises -- left as-is; font_init's no-op-not-placeholder rationale is accurate.
            SIDE FIND (not a code fix): my own memory project_imgui_font_system was stale -- it
            described a bit/bmp/ttf runtime split + _bmp.c/_ttf.c files that do not exist; the real
            design is a single .orb_font baked-atlas loader (font_tool/FreeType offline).  Memory
            corrected; NO code comment claimed this, so nothing in-tree to fix.
      - [x] widgets/  DONE 2026-07-04.  1 fix / 6 files (5 fully clean).  The lone drift was a
            NAVIGATION GAP, not a false claim: gui_widget_slider.c's header presents a structured
            per-widget breakdown of the file that stops at drag_int -- but the file also holds the
            whole drag_float / drag_float2/3/4 family AND color_edit3/4 (a ~230-line color picker
            with HSV/RGB working copy, hover tooltip, and click-to-open picker popup).  Header
            extended to cover both families + retitled "Slider, drag, and color-edit widgets".
            - [x] gui_widget.c        (clean; header uses an illustrative "such as" list, not an
                                        exhaustive enumeration, so unlisted members are not drift;
                                        cross-refs to gui_widget_window.c [window/] + gui_widget_core.c
                                        [core/] name the right files; the checkbox/radio one-frame-late
                                        wants_redraw rationale traces exactly)
            - [x] gui_widget_slider.c (1: header enumeration missing drag_float* + color_edit* -- fixed)
            - [x] gui_widget_numeric.c (clean; input_int/float/double/float2-4 all present + in order;
                                         the last_item save/restore-around-step-buttons rationale holds)
            - [x] gui_text_edit.c     (clean; exemplary -- undo-ring contract, mouse-capture-via-active_id
                                        rationale, glyph-level clip note all trace.  Verified: struct is
                                        exactly 16 bytes [fits GUI_STATE_CAP=24]; gui_clipboard_set in
                                        gui_input.c; is_item_deactivated_after_edit accessor in gui_ctx_io.c)
            - [x] gui_widget_draw.c gui_volatile.c  (clean; canvas/draw_text/icon placement + the
                                        volatile-widget UI-half seam, verified vs gui_backend.h + the
                                        FUTURE: tag on gui_volatile_end already placed in Phase 2)
      - [x] window/  DONE 2026-07-04.  2 fixes / 2 files -- both the stale-forward-marker class,
            both files otherwise exemplary (gui_widget_window.c is the best-documented file in gui:
            the drag-threshold-vs-double-click rationale, merge-back hysteresis latch, popup-z-band
            exception, and native-floater screen-coord derivation are all deep WHY prose that traces).
            - [x] gui_window.c        (1: header cast collapse/scroll/saved-layout as "(later)" but
                                        win->collapsed + win->scroll are LIVE record fields [used in
                                        this file].  Verified gui_window_t holds x/y/w/h/z/viewport/
                                        scroll/collapsed/closed/home_/restore_ etc.  Rewrote to present;
                                        kept saved-layout-to-disk as a real FUTURE: -- confirmed only
                                        docked windows serialize [dock/gui_dock_serialize.c], free
                                        windows persist in-memory only.  s_resize_* attribution to
                                        core/gui_resize.c + window_nominate_hover->gui_hover_nominate
                                        [gui_region.c] alias both re-confirmed clean.)
            - [x] gui_widget_window.c (1: "Phase 1: docked windows reserve no menu bar" -- stale
                                        phase-numbered marker [docking is complete through phase 4],
                                        same class as the table Phase-stub fixes in Phase 2.  Converted
                                        to FUTURE: stating the real invariant -- window_begin_docked
                                        hard-zeros menubar_rect so GUI_WIN_MENUBAR is ignored on the
                                        docked path, while the free-float path honors it via mb_h.
                                        Verified header's "declares no long-lived state of its own":
                                        all drag/grip/titlebar-threshold statics live in gui_window.c.)
      - [x] popup/  (2 fixes / 4 files -- both the SAME wrong-file cross-ref, no-keyword drift.
                     gui_popup.c + gui_nav.c "included by gui.c ... before gui_api.c (so gui_ctx_begin
                     can call popup_close_check / popup_apply_modal / nav_new_frame)" -- but gui_ctx_begin
                     was MOVED into gui_frame.c during the frame_begin/ctx_begin split (defined
                     gui_frame.c:508, the calls at :530-533); gui_api.c holds only the vtable entry
                     .ctx_begin = gui_ctx_begin.  The binding include constraint is gui_frame.c, not
                     gui_api.c -> fixed both.  Verified include order (popup files 178-181 precede
                     gui_frame.c:188 and gui_api.c:192) and every load-bearing cross-ref: s_popups_open /
                     s_popup_open_count are per-context members via g_ctx (gui_ctx.c:344), nav_item_register
                     defined gui_widget_core.c:326 + called from widget_behavior:483, GUI_POPUP_Z_BASE =
                     0x80000000 in gui_internal.h, s_fwd_caps.keyboard_nav gate real.  gui_widget_combo.c +
                     gui_widget_menu.c fully clean -- exemplary WHY prose (combo was-open toggle guard,
                     menu_close_chain modal floor, menu_bar clip-widen for hit-test, band inheritance,
                     off-screen premeasure) all traced.)
      - [x] dock/  (4 fixes / 3 files -- all the phase-numbered stale-forward-marker class; docking is
                    COMPLETE through phase 4, so the dev-plan phase numbers are unresolvable jargon.
                    gui_dock_drag.c "(Phase 2 mouse gestures)" -> "(the mouse gestures)" and "reusing the
                    Phase-1 tree edits" -> named the real source (gui_dock_core.c edits via the gui_dock.c
                    public verbs).  gui_dock.c "Phase 1 is programmatic only ... the later phases built on
                    top" -> "This programmatic path is the foundation ... build on the same node tree";
                    VERIFICATION GUARD: kept the drag path as "drives these very verbs" (dock_drag_commit
                    calls gui_dock_split / gui_dock_window, confirmed) but described persistence separately
                    as "rebuilds straight from the node pool" -- the loader uses dock_node_alloc directly,
                    NOT the public verbs, so a naive "both build on these verbs" would have been a NEW false
                    claim.  gui_dock_serialize.c title "(Phase 3)" dropped.  gui_dock_core.c fully clean.
                    Verified every include-order justification: dock_find_window_node / dock_window_chrome /
                    dock_drag_detect / dock_drag_commit ARE forward-declared in gui_internal.h (773-779) and
                    called from gui_widget_window.c (585/706/708/987); dock_node_alloc / dock_collapse /
                    dock_leaf_remove_tab are NOT forward-declared, matching the core header's distinction.)
      - [x] table/  (4 fixes / 1 file [gui_table.c] + 1 descriptor fix in gui.c; THREE drift classes in
                     the one focused file the Phase-2 broad sweep had only partly reached.
                     (1) STALE PHASE JARGON: the header's "Phase 1/2/3/4 --" feature enumeration (tables
                         are complete through phase 4) -> relabeled to feature layers, content preserved
                         verbatim; same class as dock/ and the window/ Phase-1 marker.
                     (2) FALSE CLAIM / CONTRADICTION: gui.c:47 descriptor called the file "multi-column
                         rows with cell clipping" -- but the file header (:9) and table_next_column (:604)
                         both emphatically state "NO per-cell clip" (self-fit over clips design); fixed the
                         descriptor to "multi-column rows, self-fitting cells, one table clip".  Same
                         species as the Phase-2 gui_api.h:450 NO_INPUT contradiction.
                     (3) INTERNAL NEAR-CONTRADICTION: the BG_CELL fill comment (:779) said "the active cell
                         clip keeps it in bounds" while its sibling ROW-fill comment (:773) correctly says
                         "auto-clipped to the body region"; there is no per-cell rect clip (the cell fill is
                         a rect, unaffected by the per-cell TEXT glyph-clip) -> "the one table clip keeps it
                         in bounds (there is no per-cell clip)".
                     Plus tightened the "Nested tables are not yet supported" guard note into a greppable
                     FUTURE: tag consistent with the canonical one at gui_internal.h:696-697 (which already
                     points back here: "gui_table.c rejects nesting").  Verified include order
                     (gui_layout_child.c:151 precedes gui_table.c:165), the s_tpool = g_ctx->table_pool
                     alias (gui_internal.h:682), and the deep one-clip / chrome-drawn-last / pair-resize /
                     stable-sort prose all trace exactly.)
      - [x] gui_frame.c + gui_api.*  DONE 2026-07-04.  7 fixes / 2 files (gui_frame.c 3, gui_api.h 4);
             gui_api.c + gui_host.h clean.  Two drift classes, and the chunk's headline lesson is
             CROSS-HEADER PARALLEL DRIFT: every claim I fixed in the .c files during earlier chunks
             had an un-reconciled TWIN in the public gui_api.h (the popup/ lesson at header scale).
             (1) STALE-FORWARD / FALSE-FUTURE:
                 - gui_frame.c:681 owned-floater section "(Phase 3) drives spawn/close" -> dropped
                   "(Phase 3)"; the twin gui_api.h:148 "the lifecycle the tear-off gesture WILL drive"
                   -> "drives".
                 - gui_frame.c:798 gui_viewport_update header "Today it destroys surfaces the user
                   closed; Phase 3 WILL ALSO service tear-off / merge-back" -- FALSE-FUTURE: both are
                   implemented directly below (step (1) tear-off/merge-back :804-936, step (2) closed/
                   abandoned teardown :938-984).  Rewrote to the real two-step contract; twin at
                   gui_api.h:154 (viewport_update "destroys those the user closed") widened to
                   "apply tear-off / merge-back and tear down closed or abandoned surfaces".
                 - gui_api.h:265 Docking "Phase 1 is programmatic ... the later phases built on top"
                   -> "The programmatic path ... Mouse drag-to-dock and layout persistence build on
                   the same tree" (public-header twin of the dock/ chunk's gui_dock.c fix).
             (2) FALSE CLAIM / CONTRADICTION (twin of the table/ chunk):
                 - gui_api.h:1148 Tables "with independent cell clipping" -> "self-fitting cells (one
                   table clip, no per-cell clip)"; same false claim as gui.c:47 + gui_table.c:779.
                 - gui_api.h:1163 table_next_column "clips draw + hit-test to the cell" -- traced
                   FALSE: gui_table_next_column only sets a text glyph-clamp (draw_set_text_clip_x,
                   gui_table.c:635) so labels ellipsize to the column; no draw/hit clip (:604-605 "no
                   per-cell clip is pushed").  Rewrote to "the cell sizes the widget and long text
                   ellipsizes to the column (self-fit, no per-cell clip)".
             Also expanded gui_frame.c's file-header enumeration (nav gap: the file had grown to hold
             the perf/state overlays, memory stats, animation-state query, multi-context lifecycle,
             and owned-floater surfaces beyond its original bracket-a-frame list).  gui_api.c is a
             mechanical vtable with accurate prose; gui_host.h's direct-call decls are a curated
             subset with no completeness claim (not drift).  Comment-only; no build run.
             ===> PHASE 3 COMPLETE.
- [ ] **Phase 4 -- Drift prevention.** Contract + ARCHITECTURE.md in-tree; SEAM:/FUTURE: tags make
      future drift greppable; optional CLAUDE.md note (touch a subsystem -> reconcile its comments).

## Confirmed drift found so far (fix during the relevant chunk)

- gui_internal.h:683 -- `_alloc` "NULL for the static default context (slot 0)": WRONG. slot 0 is
  malloc'd in ctx_alloc_slot like every context; `_alloc` is non-NULL. (Phase 3 / gui_internal.h)
- gui_internal.h:558 -- "Inert until docking lands -- no machinery yet": STALE, docking landed.
- core/gui_ctx.c:30 -- "when the multi-context model lands this stays a single global": STALE,
  multi-context landed (ctx_create, id_salt, the pool).  FIXED 2026-07-04.
- backend/pipeline/gui_emit_draw.c:41-43 -- file-header comment says "per-(win,z,vp,font)
  segments": STALE, the `band` axis was added later (draw_seg_retag keys on 5 axes incl band).
  Found during Phase 1. (Fix during backend/pipeline chunk.)

## Phase 2 fixes applied (2026-07-04)

Stale (feature landed -- rewritten to present):
- gui_internal.h:556 dock_root "inert until docking lands, no machinery yet" -> present; dock_root
  is fully wired (dock.c/dock_drag.c/dock_serialize.c/dock_core.c + gui_render.c all read/write it).
- gui_internal.h:619 label "for headers_row (future)" -> "drawn by table_headers_row" (col->label
  consumed at gui_table.c:507).
- gui.h:1299 / :1308 table_set_bg_color / table_get_sort_specs "(future phase)" -> dropped; both
  implemented (gui_table.c:761 / :670).
- gui_api.h:1156 table_headers_row "[Phase 1 stub -- no-op until Phase 2 lands]" -> removed; fully
  implemented incl. sort-click (gui_table.c:542).
- gui_api.h:450 region_begin "NO_INPUT only for now" -> "interactive by default" -- FLATLY
  CONTRADICTED the impl (gui_region.c:23-24 regions compete for hover, opt out via NO_INPUT). Real
  correctness fix, not just tense.
- table/gui_table.c:122 "dividers (Phase 3) will be lines" -> "are drawn as lines" (dividers exist,
  gui_table.c:259-290,584).

Corrected-but-NOT-to-present (verification caught a would-be new false claim):
- gui_resize.c:8,31 + gui_widget_window.c:44 "a future dock splitter shares this resize mechanism":
  the dock splitter EXISTS and does NOT share it (its own drag path in dock/; no resize_grab/
  resize_apply_edges call). Rewriting "future" -> "present sharing" would have injected a NEW false
  claim. Corrected to "window + child_begin share it; the dock splitter has its own drag path."

Banned unqualified future-prose on permanent/present facts -> present tense:
- gui_internal.h:651 "stay global for now" -> "stay global by design (tier 1/tier 3; see
  ARCHITECTURE.md)".
- gui_frame.c:20 "today's full-feature behavior" -> "the full-feature defaults".
- gui.h:494 "FirstUseEver until a saved-layout lands" -> "FirstUseEver".

Legit forward-looking -> tagged greppable (FUTURE: / SEAM:):
- gui.h:1294-1295 table COL_ALIGN_RIGHT/CENTER -> FUTURE: (flags genuinely unconsumed in table/).
- gui_internal.h:692 nested tables -> FUTURE: (gui_table.c:309 rejects nesting).
- gui_region.c:21,113 region viewport-0-only -> FUTURE: (no multi-viewport routing yet).
- gui_layout_core.c:154 size-animate hook -> SEAM: (verified inert: every size_animate caller
  passes GUI_ID_NONE -- gui_layout_child.c:119,124,390).
- widgets/gui_volatile.c:94 + :52 volatile end reserved bookend -> FUTURE: / "reserved no-op".

Left as-is (present-tense runtime state, NOT drift): gui_api.h:1119, gui_frame.c:206/454,
gui_font_internal.c:267, gui_build_cache.c:247, gui_table.c:309 -- all describe frame-runtime state
("animation not yet at target", "clock not supplied", "geometry from last frame"), not unbuilt
features. The dozens of "seam" NOUN uses (shared code junctions) likewise left untouched.

## Pilot finding (calibration takeaway)

Drift is real, same-class, and CONCENTRATED: of 3 core/gui_ctx*.c files, all 4 drift instances
were in one file and all were "comment written before a feature landed, never re-tensed" (type
lift into gui_internal.h, multi-context, gui_context_t lift).  The other two files were fully
correct.  Implication for remaining chunks: expect most comments to pass; hunt specifically for
past-written-as-future framing (the SEAM:/FUTURE: sweep in Phase 2 will catch the flagrant ones,
but per-file verification catches the ones with no future-tense keyword, like the gui_window_t
location claim).

## Log

- 2026-07-04: Survey done; ARCHITECTURE.md + COMMENT_CONTRACT.md written; plan created.
- 2026-07-04: Phase 0 done.  PILOT core/gui_ctx*.c done (4 fixes in gui_ctx.c; siblings clean).
  Comment-only edits, no build run (low-risk).
- 2026-07-04: Phase 1 done.  All 9 load-bearing ARCHITECTURE.md claims verified against code; anchor
  needs no correction.  One incidental drift logged (gui_emit_draw.c file header omits the band
  segment axis) for the backend/pipeline chunk.
- 2026-07-04: Phase 2 done.  ~17 comment fixes across 9 files (see "Phase 2 fixes applied"): stale
  docking/table "future" tags rewritten to present, region NO_INPUT contradiction fixed, banned
  "for now"/"today's" phrasing removed, legit forward-looking notes tagged FUTURE:/SEAM:, gui.h
  header banner filled (overview + ASCII em-dash cleanup).  Comment-only; no build run.  Standout
  catch: "future dock splitter shares resize" -- splitter exists but does NOT share it, so the
  naive tense-fix would have been a fresh false claim.
- 2026-07-04: Phase 3 chunk gui_internal.h done.  Fixed THE headline flat error (`_alloc` "NULL for
  the static default context" -- verified non-NULL for every slot on both the alloc and both free
  paths) + one residual "no nesting yet".  Comment-only; no build run.  Header was otherwise clean
  (mostly pre-audited in Phase 2).  Next chunk: rest of core/ (gui_layout*.c the dense payoff).
- 2026-07-04: Phase 3 chunk core/ done.  11 comment fixes across 6 files (see the core/ checklist
  above).  Standout catches -- the pilot's prediction held (drift concentrates in headers/refactored-
  file cross-refs + past-as-future framing): (1) TWO phantom-symbol refs to k_col_default, defined
  NOWHERE, refactored into the k_themes dark/light registry -- the strongest no-keyword drift; (2) a
  gui_resize.c internal contradiction (header "lives here" vs section "lives in gui_window.c"; code
  defines it here); (3) an orphaned widget_split_label header comment separated from its function AND
  a stale "future combo/drag/color" list (all three landed); (4) 3 stale child_begin file attributions
  after the gui_layout.c -> gui_layout_child.c split.  7 core/ files verified fully clean.  Comment-only;
  no build run.  Next chunk: backend/pipeline/ (start with the gui_emit_draw.c:41 band-axis drift
  logged in Phase 1).
- 2026-07-04: Phase 3 chunk backend/pipeline/ done.  5 comment fixes across 2 files (gui_emit_draw.c
  x4, gui_build_cache.c x1); 5 files fully clean.  SINGLE drift class this chunk: the command-segment
  key gained `font` then `band` axes as features landed (band from the arena-bands work), and several
  descriptive comments still named the old 3-axis (win,z,vp) key -- textbook past-written-as-present.
  This closed the gui_emit_draw.c:41 band-axis item logged in Phase 1, plus found 3 sibling instances
  of the same drift by tracing (2 more in gui_emit_draw.c, 1 in gui_build_cache.c's pipeline-map).
  Verified live key = draw_seg_retag(win,z,vp,font,band) tests all 5 (gui_emit_draw.c:287).  The
  retained-cache/volatile-widget files (gui_build_cache/tess/volatile/render) are the best-documented
  in gui so far -- long design headers, all traced exactly, zero drift.  Comment-only; no build run.
  Next chunk: backend/resource/ (gui_font*.c / gui_atlas.c / gui_icon.c -- the font registry + atlas).
- 2026-07-04: Phase 3 chunk backend/resource/ done.  ZERO fixes -- first fully-clean chunk of the
  campaign.  All 6 files (gui_atlas.h/.c, gui_font.h/.c, gui_font_internal.c, gui_icon.c) traced clean
  against code; the resource layer's headers describe the shared-atlas helper, the id-addressed font
  registry, the deferred-reload queue, and the runtime icon packer accurately and in present tense.
  Confirmed the load-bearing cross-refs (draw_push_icon reads icon_get/icon_atlas_idx; font_init
  no-op rationale; the "asset pipeline later" scoped non-goals are not drift).  Side find: corrected
  a stale entry in my OWN memory (project_imgui_font_system named a bit/bmp/ttf split + _bmp/_ttf.c
  files that don't exist; real design is a single .orb_font baked-atlas loader) -- no in-tree comment
  claimed it, so nothing to fix in code.  Comment-only campaign; no build run.  Next chunk: widgets/.
- 2026-07-04: Phase 3 chunk widgets/ done.  1 fix / 6 files (5 clean).  The chunk's one drift was a
  NEW class for this campaign -- a navigation GAP rather than a false claim or stale tense: the
  gui_widget_slider.c header gives a structured per-widget breakdown that was never extended when
  the drag_float family + color_edit3/4 (a whole ~230-line color picker: HSV/RGB working copy, hover
  tooltip, click-to-open picker popup) landed in the same file, so a reader scanning the header would
  never learn color editing lives there.  Header extended + retitled.  Distinction that mattered:
  gui_widget.c's header uses an illustrative "such as" prose list (not exhaustive) so its unlisted
  members are NOT drift -- only a structured enumeration that presents itself as the file's contents
  is on the hook.  gui_text_edit.c is the best-documented widget file (undo ring, mouse capture,
  glyph clip all traced; struct verified 16 bytes, cross-file accessor attributions confirmed).
  Comment-only; no build run.  Next chunk: window/.
- 2026-07-04: Phase 3 chunk window/ done.  2 fixes / 2 files, both the stale-forward-marker class.
  (1) gui_window.c header cast collapse / scroll / saved-layout state as "(later)" -- but collapse
  and scroll are LIVE fields on gui_window_t used in that very file; rewrote to present tense, kept
  only saved-layout-to-disk as a genuine FUTURE: (verified only docked windows serialize, via
  dock/gui_dock_serialize.c; free windows persist in memory only).  (2) gui_widget_window.c's
  "Phase 1: docked windows reserve no menu bar" -- a stale phase-numbered marker (docking is complete
  through phase 4), the same species as the table Phase-stub markers cleaned in Phase 2; converted to
  a FUTURE: tag stating the real invariant (window_begin_docked hard-zeros menubar_rect, so
  GUI_WIN_MENUBAR is silently ignored on the docked path while the free-float path honors it).  Both
  files are otherwise exemplary -- gui_widget_window.c is the best-documented file in gui (drag-vs-
  double-click threshold, merge-back hysteresis, popup-z-band exception, native-floater screen-coord
  derivation all deep WHY prose that traces exactly).  Comment-only; no build run.  Next chunk: popup/.
- 2026-07-04: Phase 3 chunk popup/ done.  2 fixes / 4 files -- and both fixes were the SAME defect in
  two files: a wrong-file cross-ref (no-keyword drift, the class from the core/ chunk).  gui_popup.c
  and gui_nav.c both said "included by gui.c ... before gui_api.c (so gui_ctx_begin can call
  popup_close_check / popup_apply_modal / nav_new_frame)", but gui_ctx_begin was MOVED into gui_frame.c
  during the frame_begin/ctx_begin split (defined gui_frame.c:508; the four frame-top calls at :530-533).
  gui_api.c now holds only the vtable entry (.ctx_begin = gui_ctx_begin), so the binding include
  constraint is gui_frame.c, not gui_api.c -> repointed both headers.  Verified the include order
  (popup files 178-181 precede gui_frame.c:188 and gui_api.c:192) and every load-bearing cross-ref:
  s_popups_open / s_popup_open_count are per-context members reached via g_ctx (gui_ctx.c:344, NOT
  plain statics -- only s_popup_begin_count is), nav_item_register is defined gui_widget_core.c:326 and
  called from widget_behavior (:483), GUI_POPUP_Z_BASE = 0x80000000u in gui_internal.h, the
  s_fwd_caps.keyboard_nav feature gate is real.  gui_widget_combo.c + gui_widget_menu.c are fully clean,
  exemplary WHY prose (combo was-open toggle guard vs popup_close_check-at-frame-top, menu_close_chain's
  modal floor, the menu_bar clip-widen so bar entries pass rect_hit, band inheritance, off-screen
  premeasure) all traced.  Comment-only; no build run.  Next chunk: dock/.
- 2026-07-04: Phase 3 chunk dock/ done.  4 fixes / 3 files, all the phase-numbered stale-forward-marker
  class (the same species cleaned from table code in Phase 2 and window/ this session).  Docking is
  COMPLETE through phase 4, so the historical dev-plan phase numbers scattered through the headers are
  unresolvable jargon: gui_dock_drag.c "(Phase 2 mouse gestures)" + "reusing the Phase-1 tree edits",
  gui_dock.c "Phase 1 is programmatic only ... the later phases built on top", gui_dock_serialize.c
  title "(Phase 3)".  Each rewritten to describe what the code actually is.  The gui_dock.c rewrite had
  a verification guard: the drag path DOES drive the public verbs (dock_drag_commit -> gui_dock_split /
  gui_dock_window, confirmed) but the load path rebuilds the tree straight from dock_node_alloc, NOT the
  public verbs -- so I described the two separately rather than letting a tidy "both build on these
  verbs" inject a fresh false claim (the same trap the Phase-2 "future dock splitter shares resize
  mechanism" catch flagged).  gui_dock_core.c fully clean.  Verified every include-order justification
  against gui_internal.h (dock_find_window_node / dock_window_chrome / dock_drag_detect / dock_drag_commit
  forward-declared at 773-779 and called from gui_widget_window.c; the plain-static helpers are not).
  Comment-only; no build run.  Next chunk: table/.
- 2026-07-04: Phase 3 chunk table/ done.  4 fixes in gui_table.c (+1 descriptor in gui.c) -- notable as
  the first chunk to surface THREE distinct drift classes in a single file that the Phase-2 broad sweep
  had only partly reached (Phase 2 fixed table "future" TAGS; the header enumeration + descriptors it
  never touched).  (1) Stale phase jargon: the "Phase 1/2/3/4 --" feature enumeration relabeled to
  feature layers, content preserved (same class as dock/).  (2) False claim: gui.c:47 called the file
  "multi-column rows with cell clipping" but the code emphatically has NO per-cell clip (self-fit over
  clips); descriptor corrected (same species as the Phase-2 gui_api.h NO_INPUT contradiction).  (3)
  Internal near-contradiction: the BG_CELL comment said "the active cell clip keeps it in bounds" while
  the sibling ROW comment correctly names the body clip -- there is no per-cell rect clip; tightened.
  Also converted "Nested tables are not yet supported" into a greppable FUTURE: tag matching the
  canonical one at gui_internal.h:696-697 (which already cross-refs "gui_table.c rejects nesting"), so a
  FUTURE.*nest sweep now finds both the state-model note and its enforcement site.  Verified include
  order, the s_tpool = g_ctx->table_pool alias, and the deep one-clip / chrome-drawn-last / pair-resize
  prose all trace.  LESSON: a Phase-2-style cross-cutting sweep clears greppable TAGS but leaves the
  no-keyword drift (a wrong top-level DESCRIPTOR, an internal near-contradiction) for the per-file pass
  -- exactly why Phase 3 revisits files Phase 2 already touched.  Comment-only; no build run.  Next
  chunk: gui_frame.c + gui_api.* (the last Phase-3 chunk).
</content>
