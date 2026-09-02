/*  RESOURCE_ID_PLAN.md  --  design + phased build plan for the resource catalogue (res)  */

# Resource Catalogue (res) + Package Manifest -- Plan

Status: IN PROGRESS -- Phases 0 and 1 landed 2026-09-01.  Successor to the reference half
of ASSET_SYSTEM_PLAN.md.
That plan built the RUNTIME half -- fs mounts, zip bundles, the asset registry, the cook
track, packaging -- and left one question unanswered: what does an application actually
reference?  Today ship_tool answers it by copying whole directories (dev_ship.c:623-625).
This plan answers it by identity.

--------------------------------------------------------------------------------
## Goal
--------------------------------------------------------------------------------

For any target, produce the complete, minimal set of external files it can reach --
computed by the build, with no hand-authored list, and without running the program.

Three properties in tension.  Every prior approach gets two:

| Approach                     | Complete | Minimal | Automatic |
|------------------------------|----------|---------|-----------|
| Ship whole directories (now) | yes      | NO      | yes       |
| Run-and-record (Q3 style)    | NO       | yes     | yes       |
| Hand-authored manifest       | yes      | yes     | NO        |

Complete   = nothing missing; the package never breaks.
Minimal    = an unreferenced asset does not ride along.
Automatic  = nobody authors or maintains a list.

Getting all three is the whole point.  The binding constraint is that the answer must be
what CAN be loaded, never what WAS loaded -- so no mechanism in this design may depend on
live code execution.

--------------------------------------------------------------------------------
## The core idea
--------------------------------------------------------------------------------

A runtime reference is a hash of a logical name.  Resolving it requires a name table.  The
build generates that table by scanning for references.  Therefore:

    The manifest is not a byproduct of the reference system.
    The manifest IS the resolution table the runtime cannot run without.

The table cannot rot, drift, or be forgotten, because a reference whose name is absent does
not resolve.  Completeness stops being a discipline problem and becomes a load-bearing
property of a program that runs at all.  The reference does double duty: it is the engine's
real runtime handle AND the manifest entry, never a parallel annotation.

--------------------------------------------------------------------------------
## Decisions settled (and why)
--------------------------------------------------------------------------------

1. NEW ENGINE ROOT LIBRARY, LEAF.  source/engine/res/ -- no deps, inits before core, slots
   between ref and prof in the hierarchy.  Leaf-ness is not tidiness: it is what lets gui
   adopt the universal id WITHOUT depending on runtime_service/asset, preserving gui's
   documented independence (gui_icon_load.c:16-17) while putting it on the same identity as
   everything else.

2. TYPES.  rid_t (res) = a name; dead, static, build-time known, "what can be loaded".
   aid_t  (asset service, renamed from asset_id_t) = a loaded instance; refcount,
   generation, GPU handle, "what is loaded".  Both are needed; they belong to different
   layers.  rid_t says identifier, not resource -- a res_t would invite the reader to think
   they hold the thing.  sid_t / rid_t / aid_t reads as one family of engine id handles.

3. rid_t IS A PURE FUNCTION OF THE NAME (a hash, never an insertion index).  Re-registration
   is idempotent by construction, so a hot-reloaded DLL re-finds the ids it declared and ids
   held elsewhere stay valid across the swap.  Hot-reload safety is inherited, not
   engineered.

4. THE NAME POOL COPIES.  Names are string literals in a DLL image; anything hung off a
   descriptor dies with that image (mod_export.h:112-114).  Holding a pointer into a DLL is a
   use-after-unload that surfaces much later as a corrupted name in a log line.

5. CUMULATIVE REGISTRY.  Unlike ref, post_exit does NOT tear entries down.  A rid outlives
   the code that mentioned it.  This is the one deliberate divergence from the ref model.

6. SEGREGATED INTERNER.  res owns its own name pool; it does not share core/sid.  core/sid
   is for LIVE named things (entities, objects) whose names arrive at runtime.  res is a
   dictionary of DEAD things that could be loaded.  The decisive asymmetry: res holds the
   complete name set at build time and can PROVE uniqueness, so a collision is a build
   error; core/sid can never prove it.  Merging forfeits that guarantee for the half that
   could have had it -- and a rid collision silently ships the wrong asset.  Layering
   agrees: sharing core's interner would drag res above core and cost gui its adoption path.

7. NAMES ARE LOGICAL AND HIERARCHICAL, NOT PATHS.  "ui/icon/save", "font/CascadiaMono/16".
   No extension, no directory coupling.  Consequences: format changes are invisible to
   source (png -> tex -> bc7 never moves an id); Unreal's FSoftObjectPath/FPrimaryAssetId
   split collapses into one type; and a computed reference is naturally a SUBTREE, which
   makes the dynamic escape hatch a scannable token instead of a side-list.

8. NAME == PATH UNDER A CONTENT ROOT, MINUS THE EXTENSION.  Convention, not a mapping file,
   so there is nothing to author.  "ui/icon/save" -> content/ui/icon/save.* -> cooked
   ui/icon/save.tex.

9. RECIPES ARE SOURCE ASSETS.  Content with no source file (a font bake) gets a recipe file
   under the content root -- content/font/CascadiaMono/16.recipe -- so the convention in (8)
   holds universally and every name has exactly one source file.  config/fonts.manifest and
   config/icons.manifest dissolve into ordinary named content.

10. ONE DOOR.  RID( "..." ) is the ONLY way a literal becomes an id.  No asset_find(const
    char*).  If any function may take a name, the scanner must know every such signature;
    with one macro it has exactly one token to find, forever, including in code nobody has
    written yet.

11. TABLE IS GENERATED C, COMPILED IN.  Per target, from its dep closure.  res keeps zero
    I/O and stays a true leaf; the table cannot be missing at runtime; and uniqueness is
    proven at build time over the complete set.

12. TWO REGISTRATION FEEDS.  Generated C at init (names referenced from source) and cooked
    file headers, lazily (names referenced from content).  Mirrors ref's frame model.

13. THREE-WAY SPLIT.  res = identity (rid_t).  engine/fs = bytes (paths, mounts, priority).
    runtime_service/asset = instances (aid_t).  fs does NOT learn about rid_t: mounting and
    zip entries are genuinely about paths, and giving fs an identity concept would make it
    the second place identity lives.

14. TWO OVERRIDE AXES, RESOLVED AT DIFFERENT TIMES.
      build-time content-root layering  -- WHICH SOURCE a name comes from (a project shadows
                                           the engine, and only this axis can ADD names)
      runtime mount priority            -- WHERE THE BYTES ARE for a fixed path (loose file
                                           shadows zip; already built, fs.c:484-510)
    Doing project-override at runtime alone fails: a project adding a new name has nothing
    in the table, and an override with a different format (save.png vs save.tex) yields two
    paths that mount priority cannot compare.  Resolving name -> path WITH extension at
    build time is also the only version that works over a zip at all, since fs_glob cannot
    enumerate ZIP mounts (fs_api.h:42-43).

15. NO RUNTIME TRACE.  Deliberately not part of this design.  It answers "what was loaded",
    which is the wrong question.

16. orb.targets STAYS ABOUT COMPILING.  It gains no asset configuration.  The manifest is
    COMPUTED FROM the unit/dep graph it already carries; derived output lands in obj/.

--------------------------------------------------------------------------------
## Where it sits
--------------------------------------------------------------------------------

    source/engine/
      mod/    sys/    ref/
      res/    <-- resource catalogue: rid_t, name pool, edges, table (leaf, no deps)
      prof/   pack/   fs/    job/   net/   core/   app/

Shape mirrors ref, which solved the same problems:

  - leaf module with a mod_desc, inits before core
  - three headers: res.h / res_api.h / res_host.h
  - hosts wire mod callbacks (the ref_wire_mod_callbacks precedent, run_host.c:395);
    pre_init fires per newly-loaded module in dep order (mod.c:339-352) -- that is where a
    DLL's generated table registers itself
  - post_exit does NOT tear down (see decision 5)

Reading the layering as a sentence: res says WHAT ui/icon/save is and what it drags in; fs
says WHERE the bytes are today; the asset service says WHAT IS LOADED and who holds it.

--------------------------------------------------------------------------------
## One graph, three edge producers
--------------------------------------------------------------------------------

Everything is an edge rid_t -> rid_t.  The packager walks it transitively from a target's
roots and never parses a content format.

  | Producer        | Who writes the edge                                        |
  |-----------------|------------------------------------------------------------|
  | Source code     | the scanner, from RID(...) / RES_TREE(...) tokens           |
  | Cooked content  | the cooker, into the file header (only it knows the format) |
  | Recipes         | the cooker, same as content                                 |

--------------------------------------------------------------------------------
## Acceptance test (the case that discriminates)
--------------------------------------------------------------------------------

sb_gui boots with a CHOSEN font, not a default: .font = GUI_FONT_CASCADIA_MONO,
.font_size = 16 (sb_gui.c:257-258).  The engine knows four families (gui_font_family.c:24-31).

A correct package for sb_gui contains CascadiaMono at 16px and NOT Roboto, JetBrains, or
CascadiaCode.  Every rejected approach fails here: grepping the app finds no string; grepping
the library finds all four; whole-directory staging ships all four.

Under this design the app names "font/CascadiaMono/16" through the one door, so the other
three are never harvested and cannot ship.

--------------------------------------------------------------------------------
## Phases
--------------------------------------------------------------------------------

Phase 0 -- res library skeleton                                          [DONE 2026-09-01]
   - source/engine/res/: res.h / res_api.h / res_host.h; res.c unity entry including
     res_registry.c (pool, table, register/lookup) and res_api.c (vtable + mod_desc).
   - rid_t = u32 FNV-1a of the canonical name (res_hash_name, header-inline so tools and
     DLLs compute identical ids with no link dep; same width and hash family as sid_t / ref).
     Zero reserved as RID_INVALID.
     Canonical form = ASCII lowercase, '\\' -> '/'; nothing else rewritten (res_canon_char).
   - Storage: three heap blocks that double as they fill, on the sid model -- an 8-byte slot
     (u32 id, u32 pool offset), a copied name pool, and a linear-probe table of u16 slot
     indices (index+1, 0 = empty) held at twice slot_cap so the load factor stays under 50%
     and a probe always terminates.  12 bytes per entry plus its name text; a catalogue is
     never larger than it needs.  All three start NULL and THAT is the empty catalogue --
     no init call is required, the first registration allocates.
     res is statically linked and never hot-reloads, so the blocks are held for the life of
     the program; the res module's exit deliberately does not release them (decision 5).
     Slots hold a pool OFFSET, not a pointer, so a pool realloc needs no fixup -- but a
     const char* from res_name is a view that a later registration can move, the same rule
     sid_cstr follows.  u16 buckets cap the catalogue at RES_MAX_ENTRIES (32768 -- the
     largest power of two whose index+1 fits a bucket, and the doubling needs powers of two
     for the probe mask).  That is far past what any project here will catalogue, and
     exceeding it is a clean registration failure, verified not to hang.
   - API: res_register / res_register_id / res_register_table, res_name / res_exists /
     res_count / res_each / res_canon, res_last_error, res_init / res_exit (reset only).
     res_register_id takes a caller-supplied id for the cooked-header feed (Phase 6) and
     is how the collision path is exercised.
   - Cumulative: no post_exit hook; mod exit leaves names in place (decision 5).
   - Landed early from Phase 1: RID( "name" ) in res.h.  The "" lit prefix makes anything
     but a string literal a compile error, which is the one-door guarantee.
   - Mod system: mod_desc_t.res_table (const res_table_t*) + MOD_RES_TABLE( name ) beside
     ref_register; pre_init / post_exit hooks are now small subscriber lists
     (mod_add_pre_init_cb / mod_add_post_exit_cb, max 4, dedupe by fn) so ref and res both
     listen.  res_wire_mod_callbacks() registers each module's table at pre_init -- first
     init and every hot-reload swap.  mod_system_init resets the lists.
   - orb.targets: target res (02_ENGINE, host_only), target sb_res, both added to the
     solutions that carry the engine floor.  "res" added to host_common's reserved names.
   - Proof: sb_res, 58 checks, 0 failed.  Covers same-name-same-id, canonical folding,
     re-registration idempotency + count stability, held-id survival, pool copy vs a
     scribbled source buffer, forced collision refused with both names in the error,
     unknown/invalid id clean miss, bad input refused, and a fake mod_desc carrying a
     res_table registered by mod_init_all's pre_init pass then surviving mod_system_exit.
     Growth is exercised with 5000 names -- several doublings of both blocks -- checking
     that every id still resolves to its own name after the rehashes and pool moves, that
     re-registering the whole set adds nothing, and that a reset releases and reallocates.
   - Host wiring (run_host.c, orb.targets dep lines) was left to Phase 1 so it landed with
     a generated table to register.

Phase 1 -- RID macro + build-time harvester                              [DONE 2026-09-01]
   - Engine floor: run_host.c calls res_wire_mod_callbacks() beside ref's and loads "res"
     between ref and prof; run.c includes res_host.h.  orb.targets: res added to run's
     deps and to every host / sandbox floor line ("mod sys ref res prof pack fs ...").
   - RES_TREE( "prefix" ) in res.h: same one-door literal rule.  A SUBTREE IS A NAME WITH A
     TRAILING SLASH: the macro appends it ( res_hash_name( "" lit "/" ) ) and the scanner
     records the same spelling, so the author never writes it and a stray slash on either
     macro stays a build error.  "ui/icon/" and the leaf "ui/icon" are different resources
     (a file and a directory can coexist under the content root) and hash differently, so
     they can never share an id -- a flag-based tree marker had let them merge into one
     entry.  No flag field exists; res_name() of a tree id is self-describing.  Phase 2
     expands trees against the content root.
   - res_hash_child( tree, leaf ) in res.h: FNV-1a is a running state, so a subtree's id IS
     the hash state after "prefix/", and continuing over a leaf yields RID of the joined
     name with no string built.  This is what lets a RES_TREE() handle resolve
     runtime-composed names (enum-indexed tables, directory listings); it is the runtime
     counterpart of RID() and needs no registry.  The only seam is the zero remap: a
     subtree whose raw state is 0 hands out id 1, from which continuation diverges, so
     res_tool refuses such a RES_TREE name at build time (one in 2^32; a rename).
     Directory interning falls out of this for free -- a directory's id is its hash
     prefix state -- so the catalogue stores no directory table; see "Directories" below.
   - res_entry_t = { name } only, 8 bytes.  The id is a pure function of the name, and a
     RID() site and the table hash through the same res_hash_name in the same binary, so
     storing the build's id would be derived data with a second source of truth.  (An
     iteration tried it for tool/runtime hash-drift detection; build_tool's include
     tracking already rebuilds res_tool on a res.h change, so it bought nothing.)
     res_register_id, the cooked-header feed's door, does verify that an incoming id
     hashes from its name -- that id arrives from outside the binary.
   - WHO OWNS A TABLE.  A table belongs to an IMAGE.  Every dynamic module gets
     g_<name>_res_table (its descriptor opts in with MOD_RES_TABLE( name ); none has yet).
     Every executable whose link closure contains res gets g_host_res_table -- a FIXED
     symbol, declared in res_host.h and carried by res's OWN mod_desc_t, since an exe has
     no descriptor of its own.  So loading "res" is what registers the exe's names, the
     same pre_init path a DLL's table takes, and an exe linked without the harvest fails at
     link time on the unresolved symbol rather than at runtime on a missing name.  Static
     libraries never own a table: their tokens are harvested into whichever image links
     them.  (build_tool_02_data.c: target_wants_res_table / res_table_symbol.)
   - res_tool (source/tools/res_tool/res_tool.c): a BUILTIN target like reflect_tool, not
     an orb.targets entry, so a child project that only imports the engine still resolves
     it; flag is_res_tool, find_res_tool().  Standalone C11 over stdio -- it links nothing
     (the plan said base+sys; a source scanner needs neither, and reflect_tool is the
     closer sibling).  It includes res.h for res_hash_name / res_canon_char only, so the
     ids it computes are the runtime's.
       CLI:  res_tool -list <units.txt> -out <table.c> -name <symbol> [-inc <dir>]... [-silent]
     build_tool owns the graph: build_gen_res_table (build_tool_09_exec.c) writes the
     unit list of the target plus its transitive link deps -- NOT mono_deps, which keep
     their own tables -- to obj/<t>/_res_units.txt.  res_tool owns the scan: it follows
     #include "..." from each unit (including-file dir first, then each -inc root, in
     compiler order), so unity fragments and headers are covered; <angle> and unresolved
     includes are skipped as system/SDK; both arms of a platform #ifdef are scanned.  A
     comment- and string-aware lexer finds RID / RES_TREE tokens, concatenates adjacent
     literals, decodes escapes.  On a #define line a non-literal argument is the macro's
     own definition and is skipped; anywhere else it is a build error ("argument must be
     a string literal"), because a name reaching the door through a macro or variable is
     invisible to the harvest.  Output is sorted by id and canonical, one entry per
     canonical name with an id + first-site file:line comment; an image with no
     references gets { .entries = NULL, .count = 0 }.
   - build_tool: step 6.5 in build_target (after reflect, inside the rebuild path -- a
     name added anywhere in the closure already makes the target stale through a unit,
     a header, or a dep .lib), the implicit tool dep on the serial path and as a graph
     dep in the scheduler, the generated .c appended in build_target_compile and
     generated first in -compile-only, ProjectDependencies + a "generated" filter entry
     in the NMake projects, a PreBuildEvent + ClCompile in the MSBuild projects (via the
     new `-res-table` command, which generates one target's table and nothing else),
     compile_commands.json entry, -graph / -list / -doctor awareness.  NO new orb.targets
     vocabulary.  NOTE: the old build_tool.exe rejects an orb.targets naming res_tool
     before it can -bootstrap; bootstrap_build_tool.bat compiles it directly.
   - VALIDATION IN THE SCANNER, not the registry: empty name, > RES_NAME_MAX, whitespace
     / control / non-ASCII / double quote, leading or trailing separator, empty segment.
     Backslashes are accepted and fold (sb_res proves the fold on purpose).
   - COLLISION CHECK: per image, over the complete closure, sorted-adjacent compare;
     fails the build naming both names and both file:line sites.  Verified with a real
     FNV-1a 32 pair found by brute force -- "collide/2ae/40b" and "collide/346/339" both
     hash to 0x00533a8f -- which sb_res now also uses for the runtime collision test
     (res_register_id no longer accepts a forged id, so forcing one is impossible).
   - Proof: sb_gui's generated table (build/obj/sb_gui/sb_gui_res_table.c, 393 files
     scanned across its closure) holds exactly font/cascadiamono/16 and nothing from
     Roboto, JetBrains or CascadiaCode; sb_gui logs the name as resolving at boot through
     the descriptor path.  (The "referenced icons" half of the proof waits for Phase 5,
     when gui itself spells icon names through RID.)  sb_res: 74 checks, 0 failed --
     a new harvest test shows this exe's own table holds exactly the five names spelled
     through the doors in sb_res.c (four RID() leaves and one RES_TREE() subtree, stored
     as "ui/icon/"), including one only ever queried and never registered by hand, and
     nothing a plain string mentions; res_register_id refuses a forged id.  res_tool
     over a scratch file with the collision
     pair, three malformed names and a RID( MACRO ) reports all five errors in one run,
     exits 1 and writes no table; a second scratch file confirms comments and strings
     are inert, adjacent literals concatenate, a literal RID inside a #define is
     harvested, a header reached by #include is scanned, and case / backslash spellings
     fold to one entry.  Forced rebuild of the render DLL exercises the dynamic path
     (an empty g_render_res_table).  Full modular Debug build green; -gen, -doctor,
     -graph green.  Not exercised: a -monolithic build.
   - Modules that were already up to date when this landed carry no table until their
     next rebuild; nothing references g_<name>_res_table until a module opts in with
     MOD_RES_TABLE, which is itself a source change, so this cannot bite.

Phase 2 -- Content root + name resolution                                [NOT STARTED]
   - content/ root convention; name -> source file -> cooked relative path (WITH extension),
     recorded in the generated table.
   - Build-time content-root layering: an ordered root list, project shadowing engine
     (override axis 1 of decision 14).
   - Proof: a name resolves to the expected cooked path; a project root shadows an engine
     name; a name with no backing file is a build error naming the referencing file:line.

Phase 3 -- Format header reference section                               [NOT STARTED]
   - Reserve a child-rid table section in the three cooked format contracts (orb_font.h v5,
     asset_tex.h v1, rhi_shader_format.h v2) plus a version bump each.
   - SPLIT THIS: reserving the fields is an hour's work and should be pulled forward the
     moment Phase 0 lands.  No format has an outbound edge yet, which is exactly why it is
     cheap now and expensive after levels and materials exist.
   - Writing and consuming edges follows in Phase 6.
   - Proof: all three formats round-trip with an empty reference section; readers reject a
     bad section length.

Phase 4 -- Asset service moves onto rid                                  [NOT STARTED]
   - asset_id_t -> aid_t.  acquire( rid_t ) replaces acquire( const char* vpath ).
   - The service DELETES its own path interning and case-folding -- that is res's job now.
   - fs is untouched; the service asks res for a path and hands that to fs.
   - Proof: sb_asset_test and sb_asset_image pass unchanged in behavior via rid; dedup,
     refcount, generation-stale rejection, and refresh() all still hold.

Phase 5 -- GUI adoption (the real surface)                               [NOT STARTED]
   - gui APIs take rid_t.  Convert the three fopen sites (gui_font_load.c:130,
     gui_icon_load.c:49, vk_shader_load.c:259) to a host-installed loader hook, following
     the font_baker_set precedent (gui_frame_resolve.c:667, wired at run_host.c:590).
   - DELETE s_builtin_icons_sdf[] / s_builtin_icons[] (gui_icon_load.c:258-269) -- the C
     table that config/icons.manifest duplicates by hand, drift documented at :245-251.
   - Font resolution stops globbing assets/font (gui_frame_resolve.c:311-312) and stops
     composing filenames; it names "font/<family>/<size>" directly.  s_family[] retires.
   - Fixes in passing: shaders resolve against sys_exe_dir while fonts and icons resolve
     against sys_root_dir, and ex_style.c:396-399 open-codes a third variant.  One root
     after this.
   - Proof: sb_gui renders identically; its generated table is exactly the acceptance-test
     set; gui still links no service above it.

Phase 6 -- Recipes + content-declared edges                              [NOT STARTED]
   - config/fonts.manifest and config/icons.manifest decompose into per-name recipe files
     under content/.  asset_tool cooks a recipe by dispatching to font_tool as it does now.
   - The cooker writes child rids into the Phase 3 header section; res registers them
     lazily on load (feed 2 of decision 12).
   - Proof: a font bakes from a recipe to byte-identical output; a synthetic parent asset
     with two children walks transitively through res with no C source mentioning the
     children.

Phase 7 -- ship_tool consumes the manifest                               [NOT STARTED]
   - Replace the three hardcoded trees (dev_ship.c:623-625) with the target's transitive
     rid closure resolved to files.
   - Read mono_dep for the module list, retiring the TODO at dev_ship.c:226.
   - Generalize past projects so `ship_tool -target sb_gui` works; today it resolves
     <project>_ship and fails in ship_stage_binary (dev_ship.c:487).
   - Fix the stale comment at dev_ship.c:620-622: missing cooked shaders are NOT fine for
     gui, which hard-fails (gui_render_init.c:422-427); only draw has a fallback.
   - Proof: a staged sb_gui package contains exactly the acceptance-test file set and runs
     from a clean directory with no engine tree present.

--------------------------------------------------------------------------------
## Open decisions
--------------------------------------------------------------------------------

1. rid_t width.  SETTLED: u32 (same width and hash family as sid_t).  The build-time
   collision check -- not the hash width -- is what guarantees uniqueness: the harvester
   (Phase 1) proves the complete name set is collision-free and fails the build on any clash,
   so a collision is a one-time rename at build, never a shipped bug.  Width only sets how
   often the check trips: ~0.01% at 1k names, ~1% at 10k -- fine at this engine's scale
   (u64 would only earn its keep past ~100k logical names).  Cooked headers store the id as
   a little-endian u32.  NOTE: this makes the Phase 1 collision check load-bearing, not a
   backstop -- it must land with the harvester, not after.
2. Name grammar for content-root layering: does a project name-space itself by prefix
   ("game/..." vs "engine/...") or do roots layer anonymously with priority?  Anonymous
   layering allows clean overrides; prefixes make provenance obvious in source.  Anonymous
   is assumed above.
3. RES_TREE granularity: whole subtree, or subtree plus a type filter?  Start with whole
   subtree and tighten only if a real site over-includes badly.  (Encoding is settled:
   a subtree is a name with a trailing slash, see Phase 1.)
4. Where res_tool's scan sits relative to reflect_tool.  SETTLED in Phase 1: separate
   builtin tool.  They do not even walk the same set -- reflect_tool scans a target's root
   directory, res_tool follows #include from the unit closure -- so a shared pass would be
   the union of two different questions.
5. Whether gui's loader hook is a raw byte reader or a typed one per resource class.  Raw
   is smaller; typed lets gui skip a copy for atlas uploads.
6. Transient runtime resources (render targets, procedural atlases) stay OUT of the rid
   space -- they are not packageable and conflating them is where Unreal's model gets heavy.
   Confirm nothing in rhi wants a rid.
7. Directories.  Splitting names into an interned directory table plus a leaf was weighed
   and settled as follows.  It is NOT a byte saving: decision 8 makes the cooked path
   name + extension, so the manifest never carries a path column and the split saves ~20%
   of a pool that is tens of KB at 10k names.  What a directory node buys is structure --
   containment and subtree enumeration -- and the runtime half of that is already free:
   a directory's id is its FNV prefix state (res_hash_child), and a subtree is a name
   with a trailing slash.  So the runtime catalogue stays a flat hash keyed by rid; a
   parent-rid + leaf slot layout is deferred until a consumer enumerates subtrees at
   runtime (the editor's content browser is the likely one).  The STATIC artifacts --
   the Phase 2 manifest and the Phase 7 package index -- are complete when written and
   should use a tree layout: directories in pre-order with a subtree end index, files
   sorted by (directory, leaf), so any RES_TREE closure is a contiguous range and a
   lookup is a binary search.  A directory-to-mount hint, if one is ever wanted, lives
   in the manifest or the asset service, never in fs (decision 13).  In ship, a bundle
   index keyed by rid needs no name text at all; names go to a debug sidecar.

--------------------------------------------------------------------------------
## Notes / constraints honored
--------------------------------------------------------------------------------

  - ASCII only; three-header split for res per convention.
  - res is a leaf: no deps, static storage, safe to query from any layer above.
  - base is a no-allocation stdlib replacement linked into hot-reload DLLs -- it is NOT the
    home for a stateful table, and res does not put one there.
  - Engine libraries stay statically linked into hosts, never in a DLL.
  - No new orb.targets vocabulary; the manifest is derived from the existing graph.
