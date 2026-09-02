/*  RESOURCE_ID_PLAN.md  --  design + phased build plan for resource names (res) and the package manifest  */

# Resource Names (res) + Package Manifest -- Plan

Status: IN PROGRESS.  Phases 0-4 landed 2026-09-01/02 as a runtime catalogue; REDUCED on
2026-09-02 to a header-only marker plus a build-time tool (see "The reduction" below).
Successor to the reference half of ASSET_SYSTEM_PLAN.md.  That plan built the RUNTIME half
-- fs mounts, zip bundles, the asset registry, the cook track, packaging -- and left one
question unanswered: what does an application actually reference?  Today ship_tool answers
it by copying whole directories (dev_ship.c:623-625).  This plan answers it by scanning.

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

A resource is named by a plain string: its path under a content root, minus the extension.
Every literal name in source passes through one marker, RID( "..." ), which evaluates to the
literal itself.  The build scans for that one token, resolves every marked name against the
content roots, fails with file:line when one has no file, and writes the image's name set to
a manifest for the packager.  Content-declared references (a material's textures) come from
cooked-file headers, written by the cooker.  The packager walks both.

Nothing about this runs at runtime.  The program loads by name, through fs or through a
plain file open, exactly as it would without the marker.  RID() is an annotation the build
reads, not a dependency the program carries.

--------------------------------------------------------------------------------
## The reduction (2026-09-02)
--------------------------------------------------------------------------------

The first cut made rid_t a 4-byte hash the runtime had to invert: a res library with a name
pool, a generated C table compiled into every image, a mod_desc hook that registered it at
init, a second interner beside sid, and an asset service that refused any id the table did
not know.  Phases 0-4 built and proved all of it.  It worked, and it made every load a
dependency on the module system and a table lookup, took away loading by plain filename,
and turned a build-time proof into a startup ritual.

What was essential turned out to be two things: the one-door marker, so the scanner can find
every reference without running the program; and the lowercase name-is-path convention, so
nothing ever translates a name into a file.  Everything else served the invertible handle.
So:

  - RID() / RES_TREE() evaluate to their literal.  Functions take names.
  - The res library, its module, the name pool, the generated table, mod_desc_t.res_table
    and MOD_RES_TABLE are deleted.  res is one header (res.h) plus two header-only format
    contracts (res_ref.h, res_cook.h) plus res_tool.
  - res_tool writes obj/<target>/<target>_res_manifest.txt -- plain text, one name per line
    -- instead of C.  Nothing is compiled from it.  Build-time resolution, subtree
    expansion, content staleness and the collision check are unchanged.
  - The asset service takes a name again (acquire( const char*, type )), dedups on core's
    sid interner, and refuses only a non-canonical spelling.  Decision 6's second interner
    dissolves; sid is the engine's one interner.
  - rid_t stays as a header-only hash for anyone who wants a key (res_hash_name); nothing
    needs to invert it.  Whoever holds a rid_t and wants the name holds the name too.
  - Startup validation is gone.  Verifying that every manifest name has a file is the
    build's job (per referenced name) and the packager's job (over the content set); a
    validation tool can re-run it on demand.  It is not a step a live program performs.

The runtime-gate argument for the table -- "a reference whose name is absent does not
resolve, so the manifest cannot rot" -- was the weakest one made for it.  The build already
proves completeness for every name spelled in source; the gate added a discipline check on
code that bypassed RID(), not a completeness guarantee.  Loading by name with a scannable
marker is also the mainstream shape (Unreal loads by path string with FName as the interner;
Unity by string key; Quake 3 by qpath).  A compiled-in hash-to-name table was the unusual
piece.

--------------------------------------------------------------------------------
## Decisions settled (and why)
--------------------------------------------------------------------------------

1. res IS A HEADER, NOT A LIBRARY.  source/engine/res/res.h: the RID() / RES_TREE() markers,
   RES_NAME_MAX, res_canon_char, res_name_ok, res_hash_name, res_path.  No .c, no module, no
   state, no deps.  Anything -- gui, a tool, a DLL -- includes it at no cost, which is what
   lets the marker appear everywhere without anything depending on anything.

2. A NAME IS A STRING.  What RID() evaluates to is the literal.  Functions that load take
   `const char* name`; the caller may hand them a marked literal, a runtime-composed string,
   or an absolute filesystem path where the function accepts one -- only the first is
   packaged.  There is no rid_t-to-name lookup because there is no need for one.  When code
   must STORE a name compactly it interns a sid_t (core), the engine's one interner, which
   gives the string back.  A cooked file's reference section (res_ref.h) may carry names or
   hashes; that is decided when the first cooker writes one (Phase 6), and names are the
   default.

3. rid_t IS A KEY, NOT AN IDENTITY.  u32 FNV-1a of the name (res_hash_name), header-only, for
   anything that indexes by name -- a dedup table, a cooked-file section.  res_tool proves the
   marked set collision-free per image and fails the build otherwise, so two marked names
   never share a key.  Nothing inverts it.

4. ONE DOOR.  RID( "..." ) is the ONLY marker a literal name passes through in source, and it
   takes a string literal only (the "" prefix makes a macro or variable a compile error; the
   scanner reports the same site as an error).  With one macro the scanner has exactly one
   token to find, forever, including in code nobody has written yet.  Names that never pass
   through it load fine and ship only if something else names them.

5. RES_TREE( "prefix" ) IS THE ESCAPE HATCH for names composed at runtime.  It evaluates to
   the prefix WITH its trailing slash, ready for a leaf to be appended by string
   concatenation, and the scanner records "prefix/" and expands every file beneath that
   directory into the manifest.  A subtree and a same-spelled leaf are two entries.

6. NAME == PATH UNDER A CONTENT ROOT, MINUS THE EXTENSION.  Convention, not a mapping file.
   "ui/icon/save" is content/ui/icon/save.png and ui/icon/save.tex in a shipped pack.
   Content-root layering (a project's content/ shadows the engine's, name by name) is what
   decides WHICH SOURCE a name comes from; runtime mount order decides WHERE THE BYTES ARE.

7. THE EXTENSION BELONGS TO THE LOADER; THE TREE BELONGS TO THE MOUNT.  A loader knows the
   extensions it accepts and asks fs for the name plus each in turn (ui/icon/save.tex, then
   .png) -- each an exact lookup, so it works over loose files and zip mounts alike.  Mount
   order then decides which tree answers: loose content/ in development, a cooked mirror for
   kinds that need baking, a shipped pack in a release -- the Quake 3 search path, with the
   same name in every tree.  res_path( out, cap, name, ext ) is the join.  The manifest
   records no extension.

8. CANONICAL LOWERCASE, ENFORCED AT BUILD, FOLDED NOWHERE.  The engine is lowercase-only.  A
   RID() literal that is not lowercase with '/' separators is a build error at its site; a
   content file or directory that is not is a build error quoting the on-disk spelling
   (res_tool matches case-insensitively so it can find and report it).  The runtime never
   folds: res_hash_name hashes bytes as written, the asset service refuses a non-canonical
   name outright (res_name_ok), and the path a name spells opens the file unchanged on a
   case-sensitive filesystem.  One spelling, everywhere.

9. RECIPES ARE SOURCE ASSETS.  Content with no source file (a font bake) gets a recipe file
   under the content root -- content/font/cascadiamono/16.recipe -- so decision 6 holds
   universally and every name has exactly one source file.  config/fonts.manifest and
   config/icons.manifest dissolve into ordinary named content (Phase 6).

10. THE MANIFEST IS DERIVED OUTPUT, PLAIN TEXT, PER IMAGE.  obj/<target>/<target>_res_manifest.txt:
    '#' comment lines, then one entry per line -- name, the source file it resolved to under
    its content root, and for an expanded entry the subtree it came from -- sorted by name so
    a subtree is a contiguous run.  The RID site is not written: it is where a name was
    spelled, not something a packager acts on, and a line number would churn the file on
    every unrelated edit; errors carry file:line.  A reader that wants only names takes the
    first token of every non-comment line.  Every
    executable and every dynamic module gets one (its own units plus every statically linked
    library); static libraries never do.  orb.targets gains no vocabulary: the manifest is
    computed from the unit/dep graph it already carries (decision 13).

11. VERIFICATION SPLITS BY WHAT IS BEING ASSEMBLED.  The build verifies what the code it just
    compiled asks for: each marked name resolves to exactly one file, marked names do not
    collide, spellings are canonical, and a bad name fails with file:line at the site.  When
    the target is already up to date that costs one stat per content directory a marked name
    lives in, plus one for res_tool.exe -- it never walks the content tree.  Package time
    (Phase 7) verifies the content set: every referenced file cooked and present, collisions
    over the complete tree, orphans.  Runtime failure -- a name with no file, a load that
    fails -- remains underneath both as an ordinary load error; nothing validates at startup.

12. TWO LOADING TIERS, ONE SHAPE.  Services that already sit on core (the asset service,
    game, editor, hosts) read bytes through fs; packs are what fs exists for and the
    dependency costs nothing there.  Self-contained libraries (gui) take a host-installed
    reader callback with a plain-file default rooted at content/, following the
    font_baker_set precedent, so they keep working alone and read from a pack the moment the
    host installs an fs-backed reader at boot.  Archive access is an opt-in the host makes
    once, never a dependency a library carries.

13. orb.targets STAYS ABOUT COMPILING.  It gains no asset configuration.

14. NO RUNTIME TRACE.  It answers "what was loaded", which is the wrong question.

15. THREE-WAY SPLIT, UNCHANGED.  res says how a name is spelled and how the build finds it.
    engine/fs says WHERE the bytes are (paths, mounts, priority) and learns nothing about
    resources.  runtime_service/asset says WHAT IS LOADED and who holds it (aid_t).

--------------------------------------------------------------------------------
## Where it sits
--------------------------------------------------------------------------------

    source/engine/res/
      res.h        -- markers, canonical form, hash, path join   (header-only)
      res_ref.h    -- the reference section of a cooked file      (header-only, formats)
      res_cook.h   -- source extension -> cooked extension        (header-only, tools)
    source/tools/res_tool/res_tool.c   -- scanner + resolver + manifest writer (builtin target)
    build/obj/<target>/<target>_res_manifest.txt, _res_units.txt, _res_deps.txt

Reading the layering as a sentence: res says how ui/icon/save is spelled and proves it has a
file; fs says where its bytes are today; the asset service says whether it is loaded and who
holds it.

--------------------------------------------------------------------------------
## One graph, three edge producers
--------------------------------------------------------------------------------

Everything is an edge name -> name.  The packager walks it transitively from a target's
manifest and never parses a content format.

  | Producer        | Who writes the edge                                        |
  |-----------------|------------------------------------------------------------|
  | Source code     | res_tool, from RID(...) / RES_TREE(...) tokens              |
  | Cooked content  | the cooker, into the file's reference section (res_ref.h)  |
  | Recipes         | the cooker, same as content                                |

--------------------------------------------------------------------------------
## Acceptance test (the case that discriminates)
--------------------------------------------------------------------------------

sb_gui boots with a CHOSEN font, not a default: .font = GUI_FONT_CASCADIA_MONO,
.font_size = 16.  The engine knows four families (gui_font_family.c).

A correct package for sb_gui contains CascadiaMono at 16px and NOT Roboto, JetBrains, or
CascadiaCode.  Every rejected approach fails here: grepping the app finds no string; grepping
the library finds all four; whole-directory staging ships all four.

Under this design the app names "font/cascadiamono/16" through the marker, so the other
three are never harvested and cannot ship.  Today: build/obj/sb_gui/sb_gui_res_manifest.txt
holds exactly that one name, resolved to font/cascadiamono/16.recipe, from 389 scanned
files.

--------------------------------------------------------------------------------
## Phases
--------------------------------------------------------------------------------

Phases 0-4 -- catalogue, harvester, resolution, format sections, asset on rid   [DONE, THEN REDUCED]
   What survives from them, as it stands after the reduction:
   - res.h: RID() / RES_TREE() as literal-only markers; res_canon_char and res_name_ok as
     the definition of canonical form; res_hash_name (FNV-1a 32, zero remapped to 1, no
     fold); res_path.  RES_NAME_MAX 255.
   - res_tool (source/tools/res_tool/res_tool.c): a BUILTIN target like reflect_tool, so a
     child project that only imports the engine resolves it.  Standalone C11 over stdio.
     Follows #include "..." from every unit in the image's link closure (including-file dir
     first, then each -inc root, in compiler order); a comment- and string-aware lexer finds
     RID / RES_TREE tokens, concatenates adjacent literals, decodes escapes; a non-literal
     argument outside a #define line is an error.  Names are validated (empty, too long,
     whitespace/control/non-ASCII, double quote, backslash, uppercase, leading/trailing/
     doubled separator).  Each name is resolved against the content roots -- each directory
     listed once and cached; segments matched case-insensitively so a misspelled file is
     found and reported with its spelling; a leaf is exactly one file whose stem matches
     (two is an error, a directory alone is an error pointing at RES_TREE); a subtree is the
     directory in every root that has it, expanded recursively, higher roots first, first
     root winning per name.  Collision check over the complete set, sorted-adjacent.  All
     errors reported per site with file:line in one run; no manifest written on error.
     Output: the manifest (decision 10).  -deps: every directory listed, '!' prefix for a
     root that did not exist.
   - build_tool: build_gen_res_manifest (build_tool_09_exec.c, step 6.5, inside the rebuild
     path) writes obj/<t>/_res_units.txt from the link closure (NOT mono_deps) and runs
     res_tool with -root <cwd>/content [-root <engine>/content] -inc source [-inc
     <engine>/source].  target_wants_res_manifest: every exe and dynamic lib except the three
     builtin tools.  Implicit tool dep on the serial path and in the scheduler; a
     PreBuildEvent in the MSBuild projects via `-res-manifest` (so a VS build still proves
     every name resolves); ProjectDependencies on res_tool in the NMake solutions; -graph /
     -list / -doctor awareness.  Up-to-date test E replays _res_deps.txt (equal timestamps
     count as stale; a newer res_tool.exe is stale).  No generated .c anywhere.
   - Cooked formats (Phase 3): one shared contract, engine/res/res_ref.h -- a ref_count
     header field and a reference section immediately after the fixed header, before the
     payload; RES_REF_MAX 4096, every reader rejects the count before any size arithmetic.
     orb_font v6, .tex v2 (36-byte header, exact length required), .oshd v3 (ref_count in
     the pad slot, section padded to 8 via oshd_ref_bytes).  Every cooker writes 0.  The
     section is reserved as u32 slots today; whether Phase 6 fills it with hashes or with
     names (a string table) is Phase 6's call -- names are the default (decision 2).
   - Asset service (Phase 4, re-cut): aid_t { index, generation }; acquire( const char*
     name, u16 type ); the caller names the TYPE (built-ins ASSET_TYPE_IMAGE = 1,
     ASSET_TYPE_SHADER = 2, fixed and asserted at init; custom types via type_register); a
     type's extensions are in PREFERENCE ORDER (cooked first: .tex, then .png ...); a record
     is keyed by the interned name (core sid) and stores only which extension loaded, so
     load, reload and the refresh stat recompose name + ext on the spot; name( aid ) reads
     the name back; a non-canonical name is refused; one resource, one type.  Deps: fs,
     core, rhi.
   - Fixtures: content/sandbox/res/ (sb_res; see its readme.md), content/sandbox/asset/
     (image.png, tri.vs.hlsl, tri.ps.hlsl), content/font/cascadiamono/16.recipe.
   - Proof after the reduction (2026-09-02): full modular Debug build green; -gen and
     -doctor green (15 ok / 0 warn / 0 fail; 67 targets carry a manifest).  sb_res 43/0:
     markers evaluate to their literals, canonical-form acceptance and every refusal,
     hashing (including the known FNV pair collide/2ae/40b vs collide/346/339), res_path
     bounds, and the manifest holding exactly 4 marked leaves + 1 subtree + 2 expanded files
     and nothing a plain string mentions.  sb_asset_test 21/0: dedup by name, name()
     readback, extension preference both ways, non-canonical / empty / NULL refused with
     nothing allocated, type conflict refused, release-to-zero with stale-handle rejection,
     fileless name FAILED but releasable, refresh no-op / in-place reload / FAILED record
     loading once its file appears.  sb_asset_image loose and tex modes load by name;
     sb_asset_shader cooks, acquires by name, renders, hot-reloads.  res_tool over a scratch
     file reports an uppercase literal, a backslash literal and a RID( MACRO ) at their
     sites and writes no manifest; the same check fired for real when a sizeof test in
     sb_res used an unresolvable literal ("no source file 'ab.*' under any content root").
     sb_gui's manifest is the acceptance-test set.  `-res-manifest` runs standalone.
   - Deleted: res.c, res_registry.c, res_api.h, res_api.c, res_host.h; target res; every
     "res" in orb.targets dep lines; mod_desc_t.res_table + MOD_RES_TABLE;
     res_wire_mod_callbacks and mod_static( res ) in every host; res_hash_child (a string
     concatenation now); the generated <t>_res_table.c and its compile/gen/json entries.

Phase 5 -- GUI adoption (the real surface)                               [NOT STARTED]
   - gui's loading functions take a name (const char*), not a path with an extension and a
     root; the caller marks literals with RID().  Convert the three fopen sites
     (gui_font_load.c, gui_icon_load.c, vk_shader_load.c) to a host-installed byte reader
     (decision 12) whose default opens name + ext beneath sys_root_dir/content; run_host
     installs an fs-backed reader so packs work.
   - DELETE s_builtin_icons_sdf[] / s_builtin_icons[] (gui_icon_load.c) -- the C table
     config/icons.manifest duplicates by hand; icon names become RID() literals or a
     RES_TREE( "ui/icon" ).
   - Font resolution stops globbing assets/font and composing filenames; it names
     "font/<family>/<size>" directly.  s_family[] retires.
   - Fixes in passing: shaders resolve against sys_exe_dir while fonts and icons resolve
     against sys_root_dir, and ex_style.c open-codes a third variant.  One root after this.
   - Proof: sb_gui renders identically; its manifest is exactly the acceptance-test set
     (font + the icons it draws); gui still links no service above it.

Phase 6 -- Recipes + content-declared edges                              [NOT STARTED]
   - config/fonts.manifest and config/icons.manifest decompose into per-name recipe files
     under content/.  asset_tool cooks a recipe by dispatching to font_tool as it does now.
   - The cooker writes the names its content references into the res_ref.h section (decide
     the encoding here: a string table is the default).  Loaders step over it; the packager
     reads it.
   - Proof: a font bakes from a recipe to byte-identical output; a synthetic parent asset
     with two children is walked transitively by the packager with no C source mentioning
     the children.

Phase 7 -- ship_tool consumes the manifest                               [NOT STARTED]
   - Replace the three hardcoded trees (dev_ship.c:623-625) with the target's manifest,
     walked transitively through cooked-file references, resolved to files.
   - A `-verify` mode (or a small manifest_tool) re-runs the content-set check on demand:
     every manifest name cooked and present, collisions over the complete tree, orphans.
   - Read mono_dep for the module list, retiring the TODO at dev_ship.c:226.
   - Generalize past projects so `ship_tool -target sb_gui` works.
   - Fix the stale comment at dev_ship.c:620-622: missing cooked shaders are NOT fine for
     gui, which hard-fails; only draw has a fallback.
   - Proof: a staged sb_gui package contains exactly the acceptance-test file set and runs
     from a clean directory with no engine tree present.

--------------------------------------------------------------------------------
## Open decisions
--------------------------------------------------------------------------------

1. Reference-section encoding (Phase 6): names via a string table (default) or u32 hashes.
   Names keep the packager format-free and need no inversion; hashes are smaller.  Decide
   when the first cooker writes one; every reader today only steps over the section.
2. RES_TREE granularity: whole subtree, or subtree plus a type filter?  Start with whole
   subtree and tighten only if a real site over-includes badly.
3. gui's reader hook: raw bytes, or typed per resource class?  Raw is smaller; typed lets
   gui skip a copy for atlas uploads.  Decide in Phase 5.
4. Transient runtime resources (render targets, procedural atlases) stay OUT of the name
   space -- they are not packageable.  Confirm nothing in rhi wants a name.
5. '.' in names.  The user wants '.' reserved and forbidden (a name is <stem>, a file is
   <stem>.<ext> with one dot, so the two never blur).  Blocked by the shader stage tag,
   which is a dotted stem suffix engine-wide: gui_quad.vs.hlsl, draw_solid.ps.oshd, the
   asset_tool profile sniff, build_tool's shader cook, gui_render_init, draw_material,
   scripts/cook_shaders.bat, orb.targets `shader` lines, docs/shader_pipeline.md.  Options:
   (a) rename the tag to an underscore suffix (gui_quad_vs.hlsl) across all of the above and
   then forbid '.'; (b) keep '.' legal and let the tool treat it as an ordinary byte, as it
   does today (the stem is everything before the LAST dot, so "tri.vs" names tri.vs.hlsl).
   Decide before any content beyond shaders relies on a dotted stem.

--------------------------------------------------------------------------------
## Rejected (do not re-propose without new evidence)
--------------------------------------------------------------------------------

  - A runtime name catalogue / compiled-in table / invertible 4-byte id: built as Phases 0-4,
    removed the same week (see "The reduction").
  - A second interner beside sid: dissolved with the catalogue.
  - A path column in the manifest or a cooked-path concept: the name IS the path minus the
    extension; the loader owns the extension; the mounts own the tree.
  - Folding content file names to lowercase at cook time: needed a second spelling of every
    file.  Content is spelled lowercase or the build fails.
  - Asset lists in orb.targets; a runtime load trace; startup validation of the name set.

--------------------------------------------------------------------------------
## Notes / constraints honored
--------------------------------------------------------------------------------

  - ASCII only.  res is header-only, the one engine directory without an _api/_host split.
  - base is a no-allocation stdlib replacement linked into hot-reload DLLs -- it is NOT the
    home for a stateful table, and there is no stateful table.
  - Engine libraries stay statically linked into hosts, never in a DLL.
  - No new orb.targets vocabulary; the manifest is derived from the existing graph.
