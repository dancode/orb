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
- [ ] **Phase 1 -- Verify the anchor.** Check each load-bearing ARCHITECTURE.md claim against
      implementing code (not comments). Known to re-check: s_frame_dirty host-skip, s_tess as
      shared frame-scratch, GUI_STATE_CAP=24 largest-struct.
- [ ] **Phase 2 -- Cross-cutting sweeps.**
      - [ ] Resolve 46 forward-looking comments: fix stale (docking, multi-context, `_alloc`),
            tag the rest `SEAM:` / `FUTURE:`.
      - [ ] Fill the gui.h header TODO with a tight overview pointing to ARCHITECTURE.md.
- [ ] **Phase 3 -- Per-subsystem audit (bottom-up, one reviewable chunk each).**
      Per-chunk checklist: inventory -> classify (correct/drift/stale-forward/vague/redundant/
      missing-invariant) -> verify each claim by tracing code -> fix -> build-verify unity TU ->
      stop for review.
      - [ ] gui_internal.h
      - [~] core/  (PILOT core/gui_ctx*.c DONE; rest of core/ pending)
            - [x] gui_ctx.c    (4 drift fixes: gui_window_t location, multi-context tense,
                                 gui_retained_t "will become" tense, slot-0 free contradiction)
            - [x] gui_ctx_id.c (clean; reclamation gate + salt verified)
            - [x] gui_ctx_io.c (clean; all accessors match)
            - [ ] gui_anim.c gui_input.c gui_layout*.c gui_region.c gui_resize.c gui_stacks.c
                  gui_style.c gui_symbol.c gui_theme.c gui_widget_core.c
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
</content>
