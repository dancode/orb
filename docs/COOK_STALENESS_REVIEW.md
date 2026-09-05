
# Cook Staleness -- Review of COOK_STALENESS_PLAN.md, and a Smaller Plan

Status: DONE (2026-09-05).  Plan B below is implemented and verified; see the Tasks section
for what each proof showed.  An independent pass over COOK_STALENESS_PLAN.md ("Plan A"
below): every finding re-checked against the code, every mechanism asked whether something
smaller buys the same thing.  The result is Plan B at the end -- same benefits, about a
third of the surface.

---

## Verdict

Plan A's diagnosis is right in every particular, and its one non-obvious call -- cooker
identity must come from the cooker's SOURCES, never from any build product -- is not just
right but the only design that survives this build.  Its remedy is over-built.  The key,
the digest, the per-output record, the unification with tree mode, the argv plumbing and
the format-version input are each machinery for a problem the source-based cooker identity
already solves on its own.  Strip them and what remains is make-style staleness with one
more input, which is what Cook-E does today plus about fifty lines.

---

## Part 1 -- The findings, re-checked

  #1  .cook_format versions the container, not the cook.       CONFIRMED.
      vk_shader_load.c:156 rejects an OSHD_VERSION mismatch outright, so the format axis
      is already covered loudly.  A dxc-flag or reflection change in shader_tool leaves
      OSHD_VERSION correctly untouched and recooks nothing.  Both halves hold.

  #2  A hand-maintained integer nothing checks.                 CONFIRMED.
      No note at rhi_shader_format.h:66 or orb_font.h; no build step verifies a bump.

  #3  The global stamp is written from a partial job set.       CONFIRMED, DEMONSTRATED.
      Re-ran the two cases: a shader-only cook writes `font 7`; a run whose manifest read
      FAILED (exit 1, zero jobs) writes the full stamp too.  man_format_write() is called
      unconditionally at the end of the cook branch (asset_tool.c:1288), before the
      `!inputs_ok` check at :1296.  Structural, as Plan A says.

  #4  Neither mechanism tracks the cooker.                      CONFIRMED.
      08898356's diff shows cooker mtimes folded into src_mtime; Cook-E has no equivalent.
      Cook-B's .cook_cache rows are "<src_mtime> <src_rel>" only (asset_tool.c:706).

  F1  List-file race.                                           CONFIRMED.
      fopen/fclose at 09_content.c:426-447, lock at :461.  12_gen_nmake.c:87 emits
      `-no-deps -target <t>` per project; MSBuild runs those concurrently.

  F2  bad_rows counted once per manifest.                       CONFIRMED.
      recipe_parse() at :1078 precedes the name dedupe at :1089.  recipe_parse() prints to
      stderr on every failure (:489-508), so the duplicate lines are real, not just the count.

  F3  cmd buffer short by ~45 bytes.                            CONFIRMED (arithmetic).
      PATH_MAX=512.  exe 512 + " -list " 7 + list 512 + roots 2*(7+512) + " -out " 6 +
      build_dir 512 + "/content" 8 + " -check" 7 + " -f" 3 = 2605 > 2560.  Reachable only
      with pathological path lengths; still worth the one-line fix because the dropped tail
      is the mode flag.

  The rejection of exe hashing (win_toolchain.c:375 timestamps the PDB path) -- CONFIRMED,
  and it turns out to be the smaller of two reasons; see Part 2.

---

## Part 2 -- What Plan A gets right, and the fact that makes it mandatory

build_target_compile() is a FULL unity compile of every unit (07_compile.c:299-306,
:335).  There is no per-unit freshness and no relink-only path.  When freshness test B
in build_target() misses -- a linked dep's .lib is newer -- the whole target recompiles
and relinks.  So a change to sys.c does not merely relink shader_tool; it rewrites every
shader_tool .obj and the .exe.

That fact disqualifies every cooker identity that reads a BUILD PRODUCT, including the
"simpler" ones this review set out hoping to substitute:

    exe mtime            bumps on a sys change            (the rule 08898356 abandoned)
    exe content hash     bumps on a sys change, and on the PDB timestamp besides
    newest .obj mtime    bumps on a sys change            (all units recompile)
    a stamp file touched on compile   same
    __DATE__/__TIME__ reported by the tool   same         (its TUs recompile)

Only an identity computed from the cooker's SOURCES -- its units and the headers its last
compile recorded in obj/<t>/_includes.txt -- stays put when sys.c changes, because sys.c
is neither.  Plan A landed on exactly that.  Keep it; it is the whole design.

Two supporting facts checked on the live tree:

  - obj/shader_tool/_includes.txt is 30 project headers and nothing else (system headers
    are already filtered out), so the walk is trivial.
  - rhi_shader_format.h is IN shader_tool's include set, and orb_font.h is in font_tool's.
    A format bump is therefore a source change to the cooker that writes that format, and
    the source-based identity catches it with no help.

---

## Part 3 -- What Plan A over-builds, and the fact that removes each piece

  The digest and the per-output record (Cook-G, Cook-H).
      A key exists to detect "the inputs are not what they were".  Make-style comparison --
      "an input is at least as new as the output" -- detects the same thing for every input
      that is a FILE, and every input here is a file once the cooker is identified by its
      sources.  Cook-E already IS make-style (man_input_time() >= dst_t, asset_tool.c:1250).
      The record buys only the timestamp-preserving-revert case, which build_tool's own tests
      A and D do not catch either; Plan A's Non-goals concede that "mtime is what the rest
      of build_tool already trusts".  By its own principle the record is surplus.

  Folding the format version into the key (item 5).
      Free, per Part 2: the version lives in a header the cooker compiles.  Dropping the
      item also lets asset_tool DELETE the two includes 081130db added --
      runtime_service/rhi/rhi_shader_format.h and tools/font_tool/orb_font.h -- which are a
      tool reaching into runtime_service and into a sibling tool for a constant it no
      longer needs.  Plan A keeps that coupling; Plan B removes it.

  Recording the cook command line (item 3).
      The argv is a pure function of the source path, the recipe's contents, and
      asset_tool's own code.  The first two are already inputs; the third is covered by
      treating asset_tool as one more tool in the chain (Part 4).  Plumbing argv back out of
      cook_file() and the cook_* spawners is invasive for no added coverage.

  Unifying with tree mode (Cook-G).
      Tree mode has exactly one caller, and it is a sandbox proof: sb_asset_image.c:225
      ("pack" mode) runs `-src ... -dst ...` then `pack`.  The build never touches it, and
      its .cook_cache is self-contained.  Rewriting a sandbox's bookkeeping to share a
      record the build does not need is effort in the wrong place.  Leave it.

  The hashed cache_lookup.
      Moot without the record.  (Cook-B's linear scan is a sandbox's problem, bounded by
      COOK_MAX_JOBS, and not this plan's.)

---

## Part 4 -- Plan B

### The rule

asset_tool, manifest mode, the shape it has today with one input added:

    stale  =  output missing
           || newest( source,
                      sibling inputs,                      -- *.hlsli / family.txt, as now
                      newest source of asset_tool,         -- the dispatcher
                      newest source of the kind's cooker ) -- shader_tool or font_tool
              >= output mtime

Every clause is a file mtime compared on ONE clock inside asset_tool.  No key, no record,
no version, no stamp.

### The boundary: pass a PATH, not a time

build_tool knows each tool's sources; asset_tool must not learn build_tool's obj layout.
Plan A hands a key across.  Plan B hands the path of the tool's newest source:

    bin/asset_tool.exe -list ... -tool asset_tool=<path> -tool shader_tool=<path> -tool font_tool=<path>

asset_tool stats each path itself.  This is not a style preference; it is required:
build_tool's platform_get_mtime() is _stat64 st_mtime (seconds since 1970,
build_tool_win.c:51) and sys_file_time() is a raw FILETIME (100 ns ticks since 1601,
win_file.c:76).  A number passed from one to the other is not comparable.  A path is.  It
also names the culprit for free: "cook shader/gui_quad.vs (shader_tool newer:
rhi_shader_format.h)".

An absent -tool, or a path that no longer exists, contributes 0 -- the axis is simply not
present, which is what a hand-run asset_tool gets today.

### build_tool's half

  - tool_newest_source( target, out_path ): max platform_get_mtime() over the target's
    units and every row of obj/<t>/_includes.txt.  It is freshness tests A and D's walk
    with a max instead of a compare.  Without _includes.txt (-no-include-track, or a tool
    never compiled here) it reduces to the units.
  - build_content_phase(): append ` -tool <name>=<path>` for asset_tool and each of its
    tool_deps -- the same set target_is_content_tool() already derives membership from.

  About 30 lines, all in 09_content.c.

### asset_tool's half

  - Parse `-tool <name>=<path>` (repeatable) into a small table.
  - man_input_time(): fold in the asset_tool entry for every job and the entry named by
    the job's kind (RES_KIND_SHADER -> shader_tool, RES_KIND_FONT -> font_tool -- the two
    exe basenames cook_shader()/cook_font() already spell).  Report the file when it is the
    newest input.
  - DELETE: .cook_format, MAN_FORMAT_FILE, man_format_version, man_kind_word's format role,
    man_format_line, man_format_path, man_format_write, the recorded[] step, the two format
    includes.  Remove the stray .cook_format from build/content on first run.

  Net negative lines.

### Tasks

  Cook-F   unchanged from Plan A.  F1, F2, F3.  Ships first.   [DONE 2026-09-05]
           Verified: a malformed recipe named by two manifests reports one error line and
           failed=1 (was two lines, failed=2).  Full check over all 96 manifests: total=15.

  Cook-G   (replaces Plan A's G, H and I)  The rule, the boundary, both halves above.
           Validation is a PAIR, and both halves must hold:      [DONE 2026-09-05]
             - touch shader_tool.c: rebuild, shaders recook, fonts do not;
             - touch sys.c: shader_tool recompiles and relinks, NOTHING recooks.
           Then: bump ORB_FONT_VERSION locally -> fonts recook, shaders do not, and
           asset_tool did not need to know the number.  Then the baseline: -check reports
           total=15 fresh=15.
           Verified, through build_tool: touching shader_tool.c -> "7 cooked file(s) out of
           date (0 missing)" under -no-content, then 7 cooked as "tool newer: shader_tool.c";
           touching sys.c -> the whole chain recompiled and relinked ("[orb link]
           bin\asset_tool.exe ... performing full link") and "cooked 0 of 15"; touching
           orb_font.h (the same mechanism as a version bump: the header is in font_tool's
           recorded include set) -> font_tool recompiled, 8 fonts cooked as "tool newer:
           orb_font.h", shaders untouched, asset_tool not recompiled.  The first run after
           the change recooked all 15 as "tool newer: asset_tool.c", which is the one-time
           cost of asset_tool joining the chain.

  Cook-H   (Plan A's K)  Docs, the two stale references (dev_ship.c:528, res_tool.c:18), the
           dead clause at 12_gen_vs.c:1041, the -force comment at 09_content.c:458.
           [DONE 2026-09-05]

  Deferred, unchanged: Plan A's Cook-J (dxc identity).  Under Plan B it would be one more
  -tool entry pointing at dxcompiler.dll -- build_tool can locate it from %VULKAN_SDK% --
  so the cost of doing it later is a line, not a design.

### What Plan B gives up

  - A source reverted with its old timestamp intact (a timestamp-preserving copy; git does
    not do this) is not recooked.  Identical to build_tool's tests A and D.  If that ever
    matters it matters for C first.
  - A cook chain edit that produces byte-identical output still recooks once.  Bounded by
    the header set: asset_tool.c's 35 recorded headers include sys_host.h, so a sys_host.h
    edit recooks everything (about 1.5 s at today's 15 jobs).  The alternative -- leave
    asset_tool out of the chain and require -f after editing its dispatch -- trades a
    bounded over-cook for an unbounded silent gap.  Include it.

---

## Part 5 -- Decisions

  -force / -f coupling.   Agree with Plan A: keep, and say why at the call site.  Under
  Plan B `-f` is also the escape hatch for the revert case above, which is a second reason
  to keep it reachable from build_tool.

  asset_tool in the chain.   Include (Part 4).  Flagged because it is the one place Plan B
  chooses more recooks over a gap.

---

## Part 6 -- Outside both plans, noted so they are not lost

  - The `-no-deps` fan-out: a VS solution build runs the content phase once per project,
    serialized on the content_phase lock (~40 ms each, ~96 projects).  Neither plan
    changes it.  Gating the phase off on -no-deps would break "edit a shader, F7"; the
    honest options are to accept ~4 s per full VS build or to teach the phase to skip when
    the target was skipped AND its manifest is unchanged.  Separate decision.
  - Tree mode and `pack` (Cook-B/D) are sandbox-only.  Whether they stay is a question
    about sb_asset_image, not about staleness.
