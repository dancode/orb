
# Cook Staleness -- One Record, One Key -- Plan

Status: SUPERSEDED (2026-09-05) by COOK_STALENESS_REVIEW.md, which re-checked every finding
here (all confirmed), kept the source-based cooker identity, and replaced the key and record
with a make-style rule.  Cook-F is implemented as written; Plan B's Cook-G stands in for
G, H and I.  Kept for the diagnosis.  Follows the content pipeline landed in c9c1af0c ->
081130db -> 5e53242d.  Reads alongside RESOURCE_ID_PLAN.md (the harvest and the content
phase) and ASSET_SYSTEM_PLAN.md (the Cook-A..E track this continues).

Scope: how asset_tool decides a cooked file is out of date, plus the three standalone
defects the review of the new pipeline turned up.  Not in scope: what a kind cooks to,
the manifest format, or the shape of the content phase itself -- all three are right.

---

## Goal

One rule, one record, one place:

    a cooked output is fresh if the cook key recorded beside it equals the key
    computed for it now.

Everything that can change the produced bytes is an input to that key.  Nothing else is a
staleness mechanism.

---

## Why: what is wrong today

The tool currently carries TWO staleness mechanisms that neither share code nor agree on
what staleness means, and which one applies depends only on which CLI mode you entered:

  .cook_cache    Cook-B (`-src`/`-dst` tree cook).  Per-output rows, "<src_mtime> <src_rel>"
                 (asset_tool.c:706-769).  Right shape.  Tracks the source and nothing else.

  .cook_format   Cook-E (`-list`/`-manifest`, the build's content phase).  One global file
                 of "<kind> <version>" rows, compared against OSHD_VERSION / ORB_FONT_VERSION
                 (asset_tool.c:1011-1035, 1288).  Wrong shape and wrong axis.

Four concrete problems:

1.  `.cook_format` versions the CONTAINER, not the COOK.  OSHD_VERSION guards the runtime's
    ability to parse an .oshd, and vk_shader_load.c:156 already rejects a mismatch loudly.
    So on the format axis the stamp only converts an obvious startup error into a silent
    recook.  The dangerous case is the other one: shader_tool changes a dxc flag, its
    reflection extraction, or the layout-hash computation.  Bytes differ, layout does not,
    OSHD_VERSION correctly stays 4, and nothing recooks.  Stale .oshd files load clean and
    behave wrong.  The stamp protects least where the risk is highest.

2.  A hand-maintained integer for a condition nothing checks decays.  Nothing at
    rhi_shader_format.h:66 says the constant is now a cook-invalidation key, and no build
    step verifies it was bumped.

3.  A single global file describing many outputs can always be written from a partial job
    set.  man_format_write() stamps every kind's current version regardless of which kinds
    this run had jobs for -- demonstrated: a shader-only cook writes `font 7`, and so does a
    run that failed its manifest read and cooked nothing.  Bump ORB_FONT_VERSION, let any
    shader-only build land first, and every existing .orb_font is marked format-current
    forever.  The per-project `-no-deps` path VS uses (12_gen_nmake.c:87) makes that routine.
    This is structural, not a slip: a per-output record cannot be partial.

4.  Neither mechanism tracks the cooker AT ALL.  Cook-E dropped the old "cooker exe is newer"
    rule (08898356) and Cook-B never had one.  Nor does either record the command line, so
    changing ASSET_TOOL_DEFAULT_FONT_SIZE, or how a stage tag maps to a dxc profile,
    invalidates nothing.

---

## The methodology

### The key

Per output, a digest over every input that determines its bytes:

    1. source identity        path + size + mtime
    2. sibling inputs         the set man_input_time() already walks: the *.hlsli beside a
                              shader, the family.txt beside a face-less recipe
    3. the cook command line  the exact argv handed to the sub-tool -- font size, dxc
                              profile, sdf/range from a recipe
    4. cooker identity        see below
    5. output format version  OSHD_VERSION / ORB_FONT_VERSION, folded in as ONE INPUT to the
                              key rather than standing as a mechanism of its own

Items 1 and 2 exist today, spread across two functions.  Items 3, 4 and 5 are the new
coverage; 5 is what lets `.cook_format` be deleted rather than merely fixed.

### Cooker identity -- what it is NOT

  - NOT the cooker's exe mtime.  This was the pre-Cook-E rule and it was abandoned for a
    good reason: shader_tool.exe relinks whenever sys.lib or pack.lib relinks, so every
    engine-wide rebuild recooked every shader at 40-150 ms each.

  - NOT a hash of the cooker's exe bytes, which is what this plan originally proposed.  It
    does not survive this build: platform_lk_fill_dynamic() stamps a fresh `time(NULL)` into
    the /PDB path on every link (build_tool_win_toolchain.c:375), and that path is written
    into the PE debug directory, so the exe bytes differ on every link.  /Brepro normalizes
    the PE header timestamp, not this.  Making exe hashing work would mean giving up the
    timestamped PDB name, which exists to dodge the locked-PDB-under-a-debugger problem --
    a worse trade than the one it buys.

### Cooker identity -- what it is

build_tool already knows what produces a cooker: the target's unit list, and
obj/<t>/_includes.txt, the flattened header set the previous compile recorded.  Freshness
test D in build_target() already replays exactly that list.  So:

  - build_tool computes a source key per cooker target -- a digest of that unit + include
    set (path, size, mtime), the same walk test D performs;
  - the content phase passes it: `-cooker shader_tool=<key> -cooker font_tool=<key>`;
  - asset_tool folds the key for the cooker a job dispatches to into that job's cook key.

This is the right semantic ("did any source that builds this cooker change?"), costs a walk
build_tool already performs, and is immune to relink churn from unrelated libraries.  When
no `-cooker` argument is given -- asset_tool run by hand, or by ship_tool -- the cooker axis
is simply absent from the key, which is today's behavior for tree mode.

### The record

One file per output directory, `.cook_cache`, rows of:

    <key> <output_rel> <source_rel>

It replaces today's `.cook_cache` rows and `.cook_format` entirely, and serves both Cook-B
and Cook-E through one loader.  Only successfully cooked outputs are recorded, which is
already Cook-B's rule and already the reason a failed cook retries.

---

## Tasks

### Cook-F -- standalone defects  [ships first; independent of the refactor]

These are review findings that do not depend on the new methodology and should not wait
behind it.

  F1. The content phase writes build/obj/_content_manifests.txt OUTSIDE the content_phase
      lock and asset_tool reads it INSIDE it (build_tool_09_content.c:419-462).  VS runs
      build_tool per project in parallel (12_gen_nmake.c:87), so a second process clobbers
      the list while the first tool is reading it: wrong cook scope, or "could not read
      manifest".  Fix: take the lock before the fopen and hold it across the spawn.

  F2. man_add_row() resolves roots and calls recipe_parse() BEFORE the name dedupe
      (asset_tool.c:1070-1092), so a malformed recipe named by N targets prints N errors and
      adds N to bad_rows -- build_tool then reports "N content cook(s) failed" for one bad
      file.  Fix: hoist the dedupe above classification, and keep a seen-name set that also
      covers rows which never become jobs, so each name is classified exactly once.

  F3. `char cmd[PATH_MAX * 5]` in build_content_phase() (09_content.c:456) is ~45 bytes
      short of the worst case; the tail it silently drops is `-check` / `-f`, so a
      truncation turns a check into a cook.  Raise it and fail loudly on a truncating
      snprintf.  Same pass: res_root_args()'s `cap - (size_t)n` underflows if a third
      content root is ever added (09_content.c:85).

  Validation: `bin\build_tool.exe -config Debug` and a parallel VS solution build; confirm
  the summary line still reports total=15 against the current tree, and that two concurrent
  build_tool runs no longer produce a manifest-read error.

### Cook-G -- one staleness record

  - Extend cache_ent_t with the key; make the row format `<key> <output_rel> <source_rel>`.
  - Move cache_load / cache_lookup / the writer into one section both modes call.  Replace
    the linear cache_lookup (asset_tool.c:757-769) with a hash lookup -- it is O(n^2) over
    up to COOK_MAX_JOBS=4096 entries today.
  - Cook-E stops reading .cook_format and starts reading .cook_cache.  Keys are not computed
    yet; this task is purely the record, so behavior is unchanged.

  Validation: cold cook into a scratch -out, then a second run reports every output fresh;
  a touched source recooks exactly one.

### Cook-H -- the cook key

  - Implement the digest over inputs 1, 2, 3 and 5 above.  Item 2 folds in man_input_time()'s
    existing sibling walk; item 3 requires cook_file() and the cook_* spawners to hand back
    the argv they built, so the key covers it.
  - DELETE .cook_format, man_format_version, man_kind_word, man_format_line, man_format_path
    and man_format_write.  OSHD_VERSION / ORB_FONT_VERSION stay exactly where they are and
    keep their loader-guard job; they become key inputs, nothing more.
  - Delete the stamp file from build/content on first run of the new tool.

  Validation: bump ORB_FONT_VERSION locally, run the phase scoped to a shader-only target
  (`-target gui`), then run a full build -- the fonts MUST recook.  That is the exact case
  that silently passes today.

### Cook-I -- cooker identity

  - build_tool: a helper that digests a target's unit + include set, reusing test D's walk.
  - build_tool_09_content.c: emit `-cooker <name>=<key>` for asset_tool's tool_deps closure.
  - asset_tool: parse it, map a job's kind to its cooker, fold the key in.

  Validation: touch a shader_tool source file, rebuild, run the phase -- shaders recook.
  Touch a sys/ source file so shader_tool merely relinks -- shaders must NOT recook.  That
  pair is the whole point of the design and neither half holds today.

### Cook-J -- dxc identity  [deferred; open]

dxcompiler.dll's version determines the SPIR-V bytes and nothing tracks it.  A rebuild of
the machine's Vulkan SDK silently leaves every .oshd stale.  Needs shader_tool to report
the loaded DLL's identity for folding into the key.  Real, but a round-trip per build --
land Cook-I first and measure before deciding.

### Cook-K -- docs and the loose ends

  - RESOURCE_ID_PLAN.md and ASSET_SYSTEM_PLAN.md: replace the .cook_format prose with the
    cook-key rule; record Cook-F..I.
  - Stale references left by the pipeline refactor: dev_ship.c:528 names build_cook_content
    (deleted; the knowledge is asset_tool's now) and res_tool.c:18 points at
    build_gen_res_manifest "in build_tool_09_exec.c" (it is in 09_content.c).
  - build_tool_12_gen_vs.c:1041 -- `target_wants_res_manifest(target)` and `has_reflect` are
    both fully subsumed by the `!target->is_build_tool` clause beside them.  Dead; drop.
  - Settle -force (below) and write the decision into the code, not just the commit.

---

## Open decisions

  -force and recooking.  build_content_phase() passes `-f` on ctx->force_rebuild
  (09_content.c:458).  Commit 08898356 had deliberately DECOUPLED these, with a comment
  explaining that honouring -force recooked every name once per image that listed it.  That
  rationale is genuinely dead -- cooking is deduped and runs once per build now -- so the
  re-coupling is defensible, but it was silent, and it means `build_tool -force` now runs
  dxc over every shader.

  Recommendation: keep them coupled (a -force that leaves stale content is a surprising
  -force) and put a comment at the call site saying why the earlier decision no longer
  applies.  The alternative -- a separate `-recook` flag -- is worth it only if full
  rebuilds turn out to be common enough for the dxc cost to bite.

---

## Non-goals

  - Content-hashing sources instead of size+mtime.  The key's shape allows it later; nothing
    here needs it, and mtime is what the rest of build_tool already trusts.
  - A shared cook cache across machines or a network cache.  Out of scope.
  - Changing what any kind cooks to, or the manifest format res_tool writes.

---

## Baseline for comparison

Measured 2026-09-05 on the current tree, so the refactor can be checked against it:

    96 resource manifests, 250 total rows, 15 unique cookable jobs
    asset_tool -check over the full manifest list: 4 ms, exit 0, total=15 fresh=15
    a cold cook of gui's two shaders: 190 ms (dxc dominates, 41 ms + 148 ms)
