/*  RESOURCE_ID_PLAN.md  --  design + phased build plan for resource names (res) and the package manifest  */

# Resource Names (res) + Package Manifest -- Plan

Status: ALL PHASES LANDED 2026-09-02.  Phases 0-4 were built as a runtime catalogue and
REDUCED the same day to a header-only marker plus a build-time tool (see "The reduction"
below); Phases 5, 6 and 7 (the packager) followed.
Successor to the reference half of ASSET_SYSTEM_PLAN.md.  That plan built the RUNTIME half
-- fs mounts, zip bundles, the asset registry, the cook track, packaging -- and left one
question unanswered: what does an application actually reference?  ship_tool used to answer
it by copying whole directories; it now answers it from the build's scan (Phase 7).

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

Nothing about this runs at runtime.  The program loads by name through fs, exactly as it
would without the marker.  RID() is an annotation the build reads, not a dependency the
program carries.

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
   gives the string back.  A cooked file's reference section (res_ref.h) carries NAMES, as a
   padded string table (decided in Phase 6): the packager reads them without inverting
   anything and without knowing the format.

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
   universally and every name has exactly one source file.  A recipe says what it cooks to
   (kind) and its bake parameters (size, sdf, range); the TYPEFACE it bakes from is stated
   once per family in content/font/<family>/family.txt, which the recipes beside it inherit
   (a recipe may still spell its own "face" to override) and which the gui's runtime baker
   reads for sizes no recipe covers -- so a cooked bake and a runtime bake of one family come
   from one spelling, and gui compiles in no family table.  The face is a COOK INPUT (a TTF
   under source_content/, or an OS face name), not a reference: it is never written to a cooked
   file's reference section and never ships.
   config/fonts.manifest -- the ship-time cook list dev_ship reads -- is fully expressed by
   the recipe set plus RID(); it and dev_ship's font cook loop go when Phase 7 lands the
   manifest walk.  config/icons.manifest is NOT a recipe candidate: it is an IMPORT manifest
   (decision 17) and stays.

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

12. ONE LOADING TIER: EVERY CONTENT BYTE COMES THROUGH fs.  (Rewritten 2026-09-02; the first
    cut gave gui a host-installed reader callback with a plain-file default.)  fs sits below
    core with deps on sys and pack only, and it IS the source-agnostic reader -- a directory,
    a cooked mirror, a pack, later a socket, all behind eight functions that know bytes and
    paths and nothing above them.  A library depending on it is depending downward on a
    narrow, stable vtable, which is what the module system's dep list is for; a callback is
    for a dependency that would point UP (dev_font above gui, the font baker) or that varies
    per host (the log sink).  So gui declares fs as a module dep and reads every bake, icon,
    sprite and shader as name + extension through the mounts (gui_res.h); it holds no root.
    Only the HOST mounts, once, at boot: run_host mounts <root>/build/content (the cooked
    mirror, priority 10) over <root>/content (loose, 0); the gui boot path does the same on a
    sandbox's behalf and unmounts at shutdown.  Files the running machine produces -- the
    pipeline cache, the dev font bake cache, logs -- are not content and stay on sys.

13. orb.targets STAYS ABOUT COMPILING.  It gains no asset configuration.

14. NO RUNTIME TRACE.  It answers "what was loaded", which is the wrong question.

15. THREE-WAY SPLIT, UNCHANGED.  res says how a name is spelled and how the build finds it.
    engine/fs says WHERE the bytes are (paths, mounts, priority) and learns nothing about
    resources.  runtime_service/asset says WHAT IS LOADED and who holds it (aid_t).

16. THE BUILD COOKS WHAT THE MANIFEST NAMES.  (2026-09-02.)  A marked name whose source
    needs a cooked form -- a stage-tagged .hlsl (-> .oshd), a .recipe (-> what its kind line
    says; a font bake) -- is cooked by build_tool into <build>/content/<name>.<cooked ext>,
    the mirror the host mounts above content/, so the runtime asks for the name and the cooked
    file wins.  build_cook_content runs per image over its manifest, before the up-to-date
    check (an edited shader recooks with no C change) and again after res_tool writes a fresh
    manifest (a newly marked name cooks on the build that introduced it); asset_tool does the
    cooking and reads the stage tag or the recipe itself.  This replaced the 'shader' lines in
    orb.targets -- decision 13 now holds literally -- and it is what makes "only RID'd font
    sizes ship" produce the bakes: content/font/cascadiamono/16.recipe cooks because sb_gui
    marks the name, and no other size does.  Images stay loose (gui decodes PNG itself; the
    asset service's .tex preference is a ship-time cook, Phase 7).  build_cook_content finds
    and builds whichever target carries 'is_asset_tool' (asset_tool) the moment it meets a
    manifest entry that needs cooking, so a target whose manifest names a shader or a recipe
    needs no 'tool_dep' of its own; asset_tool itself carries 'tool_dep shader_tool font_tool',
    since it spawns them as bin/ siblings at cook time.

17. THREE TIERS, TWO PIPELINES.  (2026-09-02.)  What a directory holds is decided by who reads
    it:

      | Tier            | Tracked   | Read by              | Holds                          |
      |-----------------|-----------|----------------------|--------------------------------|
      | source_content/ | yes, less | tools only           | raw sources: TTFs, SVGs, laid   |
      |                 | font_cache|                      | out as a MIRROR of content/;    |
      |                 |           |                      | tool caches (font_cache)        |
      | content/        | yes       | engine and cooker    | source-form content: PNGs,      |
      |                 |           |                      | .hlsl, .recipe, family.txt      |
      | build/content   | generated | engine (mounted      | cooked forms: .orb_font, .oshd, |
      |                 |           | above content/)      | .tex                            |

    A tier is named for what the engine does with it, and content/ is the only tier a resource
    name resolves against.

    Two pipelines cross those tiers and must not be confused.  IMPORT moves a raw source into
    content/ (image_tool icons rasterizes source_content/ui/icon/*.svg into content/ui/icon/
    from config/icons.manifest, by hand, checked in); it decides what content EXISTS.
    PACKAGE filters content/ plus build/content into a shipped set; RID() and the per-target
    manifest decide what SHIPS (dev_ship, Phase 7).  asset_tool belongs to neither: it cooks
    content/ into build/content and never reads source_content/ except through a recipe's face.
    Nothing reads source_content/ at runtime except the dev-only font baker's cache.
    source_content/ MIRRORS content/ (since Phase 7): a raw source and the content it becomes
    sit at the same path -- source_content/font/jetbrains/JetBrainsMonoNL-Regular.ttf beside
    content/font/jetbrains/, source_content/ui/icon/save.svg beside content/ui/icon/save.png --
    so a family.txt face is a path under source_content/font that reads like its own directory.
    source_content/font_cache stays a cache.  The old OUTPUT directories under it (font, icon)
    are gone: no tool writes there.

--------------------------------------------------------------------------------
## Where it sits
--------------------------------------------------------------------------------

    source/engine/res/
      res.h        -- markers, canonical form, hash, path join   (header-only)
      res_ref.h    -- the reference section of a cooked file: the res_ref_head_t every cooked
                      format opens with, and the string-table writer / validator / iterator /
                      whole-file locator the cookers and the packager share (header-only)
      res_cook.h   -- source extension -> cooked extension        (header-only, tools)
    source/tools/res_tool/res_tool.c   -- scanner + resolver + manifest writer (builtin target)
    build/obj/<target>/<target>_res_manifest.txt, _res_units.txt, _res_deps.txt
    build/content/<name>.<cooked ext>   -- the cooked mirror build_cook_content writes from the
                                           manifest (build_tool_09_exec.c); mounted above content/
    content/font/<family>/family.txt    -- the family's typeface (decision 9); recipes inherit it,
                                           gui_font_family.c reads it for the runtime baker
    source/runtime_service/gui/gui_res.h -- how gui reads a name + ext through fs
    source/developer/dev_ship/dev_ship.c -- the packager: image manifests + mono_dep modules,
                                           closed over reference sections, verified and staged
                                           into <out>/content (ship_tool is its CLI)

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

sb_gui boots with a CHOSEN font, not a default: .font = RID( "font/cascadiamono/16" ).  The
engine has recipes for four families under content/font (cascadiamono, cascadiacode,
jetbrains, roboto) and gui_font_family.c knows the typeface behind each for the baker.

A correct package for sb_gui contains CascadiaMono at 16px and NOT Roboto, JetBrains, or
CascadiaCode.  Every rejected approach fails here: grepping the app finds no string; grepping
the library finds all four; whole-directory staging ships all four.

Under this design the app names "font/cascadiamono/16" through the marker -- and the name IS
the request gui boots from, so there is no second spelling to drift -- so the other three are
never harvested and cannot ship.  Today: build/obj/sb_gui/sb_gui_res_manifest.txt holds that
font plus what gui itself always names (its five built-in icons under ui/icon and its two
shaders under shader/), resolved against content/, and build/content holds their cooked forms.

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
   - Cooked formats (Phase 3, re-cut in Phase 6): one shared contract, engine/res/res_ref.h --
     a reference section immediately after the fixed header, before the payload; every
     reader rejects the head before any size arithmetic.  Phase 3 laid it down as a
     ref_count field and u32 id slots; Phase 6 replaced that with the res_ref_head_t opening
     and a string table (see Phase 6).
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

Phase 5 -- GUI adoption (the real surface)                               [DONE 2026-09-02]
   - gui depends on fs (decision 12, rewritten) and reads every content byte as a resource
     name + extension through the mounts (gui_res.h).  No root, no sys_root_dir, no
     sys_exe_dir, no fopen anywhere in gui.  Hosts mount content/ and build/content once;
     the gui boot path mounts them for the sandboxes.
   - Fonts: a font is the name "font/<family>/<size>".  init / boot take the name itself
     (.font = RID( "font/cascadiamono/16" )); gui_font_family_t and font_size are gone; the
     resolver composes "font/<family>/<N>" for the DPI retarget and type ramp, reads the
     cooked bake through fs, else asks the baker, else settles for a resident size.  The
     directory scan of source_content/font, the filename parser (font_ship_name_parse) and the
     nearest-shipped ladder are gone: the mounts are never enumerated.  The baker callback
     returns BYTES, not a path (gui_font_bake_fn; dev_font_get_bytes is the adapter), so
     gui never opens a file outside the mounts.  font_load / font_load_into take names;
     _mem twins take bytes (the style editor's live bake preview, the unit tests).
   - Icons and sprites: load_icon / load_icons / load_icon_sdf / load_sprite take a resource
     name; the built-in table names RID( "ui/icon/settings" ) etc., so every image linking
     gui carries the five built-ins in its manifest.  content/ui/icon/ holds the PNGs
     (tracked, like the SVGs they come from) and image_tool icons writes there.
   - Shaders: gui_quad.{vs,ps}.hlsl moved to content/shader/, named RID( "shader/gui_quad.vs" )
     / .ps in gui_render_init.c and read as .oshd through fs into the rhi's in-memory .oshd
     loader.  sb_quad_pull's three shaders moved to content/sandbox/quad_pull/ the same way.
     The 'shader' lines left orb.targets (decision 16).
   - Deviation from the plan as written: s_family[] did not retire outright.  The family
     directory -> typeface table (font_family_face) survived because the runtime baker needs
     "JetBrains Mono NL", not "jetbrains"; Phase 6 moved that spelling into content
     (family.txt) and the table is gone.
   - Left alone, on purpose: rhi's path-taking shader loaders and draw_material's optional
     cooked pair under bin/shaders (scripts/cook_shaders.bat) -- a dev-only affordance with
     an embedded fallback, not content.  dev_ship kept cooking config/fonts.manifest into
     source_content/font until Phase 7 replaced it with the manifest walk.
   - Proof: full Debug build; sb_gui_test passes (the .orb_font contract now runs over bytes);
     sb_gui's manifest is the acceptance-test set (its font, gui's five icons, gui's two
     shaders), build/content holds font/cascadiamono/16.orb_font and shader/gui_quad.{vs,ps}.oshd,
     and sb_gui renders from them.

Phase 6 -- Recipes + content-declared edges                              [DONE 2026-09-02]
   - Reference section, decided and built (res_ref.h).  Every cooked format's header OPENS
     with the five res_ref_head_t fields -- magic, version, ref_count, ref_size, ref_offset
     -- and the section is a string table: ref_count names, NUL-terminated, in order,
     zero-padded to a multiple of RES_REF_ALIGN (8), ref_size bytes long, at ref_offset
     (which is the fixed header's size; loaders check that).  RES_REF_MAX 4096 names,
     RES_REF_SIZE_MAX 1 MiB; res_ref_head_ok bounds the head before any arithmetic;
     res_ref_section_ok accepts exactly one byte string per name list (padding shorter
     than one alignment unit, all zero; every name canonical); res_ref_measure / _write
     produce it; res_ref_next walks it; res_ref_locate does the whole-file find-and-
     validate the packager calls on any cooked file without knowing its format.  Formats:
     orb_font v7 (52-byte header; ORB_FONT_HEADER_BASE_SIZE and the v2-v4 tail reading are
     gone), .tex v3 (44 bytes), .oshd v4 (72 bytes; oshd_ref_bytes gone -- the section's
     own alignment keeps the u64 members aligned).  Every cooker still writes an empty
     section: nothing that exists today names anything.  A cook input (a recipe's face) is
     not a reference and is never written.
   - Recipes (decision 9).  content/font/<family>/family.txt states the family's face; the
     recipes inherit it (asset_tool reads the sibling on disk when a recipe has no "face"
     line -- not across content roots, so a child project shadowing one size of an engine
     family carries the descriptor or spells the face); build_cook_content folds
     family.txt's mtime into a recipe's staleness the way it folds .hlsli siblings into a
     shader's.  Every size config/fonts.manifest lists now has a recipe (cascadiamono,
     jetbrains, roboto at 12/16/20/24/32; cascadiacode 16), so nothing is lost when the
     manifest goes; only RID'd ones cook.
   - gui.  gui_font_family.c reads "font/<family>/family" + ".txt" through the mounts and
     hands the face to the baker; the compiled table is gone.  No descriptor = the family
     directory name is the face request (dev_font resolves "consolas" directly).
   - Out of scope, on purpose: config/icons.manifest is an import manifest (decision 17)
     and stays as it is.  config/fonts.manifest, dev_ship's font cook loop and font_tool's
     `manifest` subcommand stay one more phase: deleting them before the manifest walk
     exists would leave a ship with no fonts.
   - Proof: full modular Debug build green; -gen and -doctor green (the one warning is the
     pre-existing 'run' descriptor note).  sb_res 78/0: measure / write / validate /
     iterate round-trip, every malformed shape refused (unaligned size, count without bytes
     and bytes without count, counts short of and past the names, nonzero padding, a whole
     alignment unit of slack, an unterminated name, a non-canonical name, a section past the
     end of the file, a file shorter than the head), and the walk: a synthetic parent names
     two children, one child names the other, no RID() spells either, and the walk from the
     parent visits exactly the three, depth first, reading the twice-named child once, and
     stops on a corrupt section.  sb_gui_test 35/0 (the .orb_font contract over the new
     head: short/long sections, count-without-size, unaligned, over-cap, wrong offset, old
     version all refused).  sb_asset_test 21/0.  sb_gui and sb_gui_example boot from
     recooked v7 bakes.  Byte-identical: content/font/jetbrains/16.recipe cooked through
     asset_tool, the same face and size baked through font_tool directly, and the build's
     own build/content/font/jetbrains/16.orb_font share one SHA-256.

Phase 7 -- ship_tool consumes the manifest                               [DONE 2026-09-02]
   - The content set is derived (dev_ship.c, "The content set").  The ship IMAGE is the exe
     target plus the modules its mono_dep line in orb.targets names -- read from the file,
     the mirrored s_runtime_modules[] table is gone -- and the set is the union of their
     resource manifests (build/obj/<t>/<t>_res_manifest.txt, first column), each name
     resolved the way res_tool resolved it (first content root holding exactly one file
     with that stem; the project's content/ shadows the engine's), represented by its cooked
     file under build/content when its source cooks (.hlsl -> .oshd, .recipe -> its kind's
     extension; images stay loose), and closed over every cooked file's reference section
     (res_ref_locate / res_ref_next).  Errors name the offender and what asked for it: no
     source, two sources, not cooked, corrupt section, non-canonical.
   - Pipeline is build -> VERIFY -> stage -> package -> deploy.  The cook stage is gone with
     config/fonts.manifest, ship_cook and font_tool's `manifest` subcommand: the build cooks
     what the manifests name, so there is nothing left for a ship to cook.  verify (also
     `ship_tool <x> -verify`) resolves the whole set, then walks the complete content roots
     for a stem claimed by two files in one directory (fails) and for files no image of the
     ship references (listed; they simply do not ship).
   - stage copies the set to <out>/content/<name>.<ext>: a cooked file lands where its
     recipe or .hlsl would, so the shipped exe's one content/ mount (sys_root_dir() is one
     above bin/) finds every name with no path change and the absent build/content mount
     is harmless.  The staged config/ and bin/shaders trees are gone with the stale comment
     that called missing shaders fine.
   - `ship_tool <exe> -target` ships any exe target under its own name: -monolithic by
     default, the exe plus its mono_dep DLLs with -modular.  A project ship is unchanged in
     shape; its module list now comes from <project>_ship's mono_dep (host_game's for a
     child project, plus the project).
   - source_content/ mirrors content/ (decision 17): font_source -> font, icon_source ->
     ui/icon; dev_font resolves faces under source_content/font, image_tool icons reads
     source_content/ui/icon, family.txt faces are unchanged (already family-relative).
     The old output directories font (bakes) and icon were moved aside to _retired/
     (untracked; delete at leisure).  font_tool's default output is the
     current directory and `font_tool info` with no argument walks build/content; the style
     editor's "Export final" button and dev_font_dir are gone.
   - Proof: full modular Debug build, -gen and -doctor green (the one warning is the
     pre-existing 'run' descriptor note); sb_res 78/0, sb_gui_test 35/0.
     `ship_tool sb_gui -target -config Debug -clean` ran the whole pipeline: build_tool
     cooked the two gui shaders and the boot font, verify reported "sb_gui + 1 module: 8
     files referenced, 3 cooked, 0 collisions, 37 unreferenced", and the staged package is
     exactly bin/sb_gui.exe, sb_gui.bat, manifest.txt and content/{font/cascadiamono/
     16.orb_font, shader/gui_quad.{vs,ps}.oshd, ui/icon/{file,folder,save,settings,temp}
     .png} -- the acceptance-test set and nothing else.  Copied to a directory outside the
     engine tree, sb_gui.exe booted from it: both shaders loaded from .oshd, "loaded 5/5
     built-in icons", "loaded font 'font/cascadiamono/16'", window up.  The -modular stage
     path ships sb_gui.exe + render.dll + the same content; `ship_tool sample_game -verify`
     resolves sample_game_ship + 3 modules (9 files, 4 cooked, 0 collisions).

--------------------------------------------------------------------------------
## Open decisions
--------------------------------------------------------------------------------

1. DECIDED 2026-09-02 (Phase 6): the reference section is a string table of names behind a
   head every format opens with (res_ref.h).  Names keep the packager format-free and need
   no inversion; the size cost is nothing until a cooker writes one.
2. RES_TREE granularity: whole subtree, or subtree plus a type filter?  Start with whole
   subtree and tighten only if a real site over-includes badly.
3. DECIDED 2026-09-02: there is no reader hook.  gui reads through fs directly (decision 12)
   and parses from the blob; the one extra copy (blob -> the slot's resident pixels) is the
   same copy the fread path made.
4. Transient runtime resources (render targets, procedural atlases) stay OUT of the name
   space -- they are not packageable.  Confirm nothing in rhi wants a name.
5. DECIDED 2026-09-02: '.' is legal in a name and is an ordinary byte.  A file's extension
   is whatever follows its LAST dot; everything before it is the name, dots included.  So
   "tri.vs" names tri.vs.hlsl (loader adds .oshd), which is the shader stage-tag convention
   used engine-wide (gui_quad.vs.hlsl, draw_solid.ps.oshd, asset_tool's profile sniff), and
   the tool's rule and the shader tools' rule are the same rule.  A shader program is NOT one
   name for both stages: a name is exactly one file, and sb_quad_pull already shares qp.ps
   across two vertex shaders.  Forbidding dots was considered and rejected: it would have
   forced an underscore rename across the shader pipeline for no visible behavior.

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
