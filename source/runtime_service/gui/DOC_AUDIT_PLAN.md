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
- [ ] **Phase 3 -- Per-subsystem audit (bottom-up, one reviewable chunk each).**
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
      - [ ] backend/pipeline/
      - [ ] backend/resource/
      - [ ] widgets/
      - [ ] window/
      - [ ] popup/
      - [ ] dock/
      - [ ] table/
      - [ ] gui_frame.c + gui_api.*
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
</content>
