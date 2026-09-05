# Content -- names, roots, cooking, loading, shipping

How a file under `content/` gets a name, how the build proves that name has a file, how the
cooked form is produced, how the runtime loads it, and how a ship finds exactly the set an
executable can reach.  Nothing about names runs at runtime: the program loads by string
through fs, and the build reads an annotation.

---

## 1. A name

A resource is named by its path under a content root, minus the extension:
`"ui/icon/save"` is `content/ui/icon/save.png`.  Names are lowercase with `/` separators;
`.` is an ordinary byte, so `"shader/gui_quad.vs"` names `gui_quad.vs.hlsl`.  The
extension belongs to the loader, which asks fs for the name plus each extension it accepts.

Every literal name in source passes through one marker, `RID( "..." )`, which evaluates to
the literal.  `RES_TREE( "prefix" )` is the escape hatch for names composed at runtime: it
evaluates to the prefix with its trailing slash, and the build expands every file beneath
that directory.  Functions take `const char* name`; a marked literal is packaged, a
runtime-composed string is not.

`source/engine/res/` is header-only, so anything can include it at no cost:

    res.h        RID / RES_TREE, canonical-form check (res_name_ok), res_hash_name, res_path
    res_ref.h    the reference section every cooked file opens with (section 5)
    res_cook.h   source extension -> cooked extension (tools)

`res_hash_name` is a key for anyone who wants one (FNV-1a 32).  Nothing inverts it; whoever
holds a hash holds the name.  When code must store a name compactly it interns a `sid_t`.

---

## 2. Three tiers, two pipelines

    | Tier            | Tracked | Read by             | Holds                                 |
    |-----------------|---------|---------------------|---------------------------------------|
    | source_content/ | yes     | tools only          | raw sources (TTF, SVG), laid out as a |
    |                 |         |                     | mirror of content/; tool caches       |
    | content/        | yes     | engine and cooker   | source-form content: PNG, .hlsl,      |
    |                 |         |                     | .hlsli, .recipe, family.txt           |
    | build/content   | no      | engine, mounted     | cooked forms: .oshd, .orb_font, .tex  |
    |                 |         | above content/      |                                       |

content/ is the only tier a name resolves against.  A child project's `content/` shadows
the engine's name by name.

IMPORT moves a raw source into content/ and decides what content exists: `image_tool icons`
rasterizes `source_content/ui/icon/*.svg` into `content/ui/icon/` from
`config/icons.manifest`, by hand, checked in.  PACKAGE filters content/ plus build/content
into a shipped set and is decided by `RID()` (section 6).  Cooking sits between: it turns
content/ into build/content and reads source_content/ only through a recipe's face.

Content with no source file gets a recipe.  `content/font/cascadiamono/16.recipe` says what
it cooks to (kind) and its bake parameters (size, sdf, range); the typeface comes from
`content/font/<family>/family.txt`, which the gui's runtime baker reads for sizes no recipe
covers.  The face is a cook input, never a reference, and never ships.

---

## 3. The build proves every name

`res_tool` runs for every executable and dynamic module (never a static lib).  It follows
`#include "..."` from every unit in the image's link closure, lexes for `RID` / `RES_TREE`
tokens, validates spelling, resolves each name against the content roots (case-insensitive
match so a misspelled file is reported with its on-disk spelling), expands subtrees,
checks the marked set for hash collisions, and fails with file:line on any error.  Output
is `obj/<t>/<t>_res_manifest.txt`: `#` comments, then one line per name (name, resolved
source file, and the subtree it came from if expanded), sorted by name.

The manifest is derived output.  `orb.targets` carries no asset vocabulary; the manifest
comes from the unit/dep graph it already has.  An up-to-date target costs one stat per
content directory a marked name lives in.  A VS build runs the same scan as a PreBuildEvent.

---

## 4. The content phase cooks what the manifests name

After the code graph, build_tool runs asset_tool once over the manifests of the targets it
built (`build_tool_09_content.c`):

    asset_tool -list <obj>/_content_manifests.txt -root content [-root <engine>/content]
               -out build/content -tool <name>=<path>... [-check] [-f]

What cooks: a stage-tagged `.hlsl` -> `.oshd` (shader_tool), a `.recipe` -> what its kind
says, a font bake -> `.orb_font` (font_tool).  Images stay loose; the asset service prefers
a `.tex` when one exists.  asset_tool carries `tool_dep shader_tool font_tool` and spawns
them as bin/ siblings.

Flags: a plain build cooks with the asset_tool already in bin/ (a missing cooker is a note,
not a failure); `-no-content` only checks and reports how many cooked files are out of
date; `-content` builds the cooker first; `-shipping` implies `-content -strict-content`.
`-target gui -content` cooks only what gui's closure names.  Nothing in the code graph
waits on a cooked file, so the phase needs no edge into any target.

Staleness is one make-style rule, inside asset_tool:

    stale = output missing
         || newest( source, sibling inputs, newest source of asset_tool,
                    newest source of the kind's cooker, dxc.exe for a shader ) >= output

Sibling inputs are the `.hlsli` beside a shader and the `family.txt` beside a recipe.
build_tool passes each tool's newest source as a path (its units plus the headers in
`obj/<t>/_includes.txt`), so an edit to shader_tool.c recooks the shaders, a bump of
`ORB_FONT_VERSION` recooks the fonts, and a relink of a cooker because sys was rebuilt
recooks nothing.  A failed cook deletes its output.  `-f` (and `build_tool -force`) recooks
everything.

Known gap: a source reverted with its old timestamp intact is not recooked, the same rule
build_tool applies to code; `-f` is the escape hatch.

---

## 5. Cooked files carry their references

Every cooked format's header opens with the five fields of `res_ref_head_t` (magic,
version, ref_count, ref_size, ref_offset), followed by a reference section: a string table
of the names the content needs loaded beside it, NUL-terminated, zero-padded to
`RES_REF_ALIGN`.  `res_ref_locate` finds and validates the section in any cooked file
without knowing its format; `res_ref_next` walks it.  Runtime loaders step over it.

No cooker emits a reference yet; every file written today has an empty section.  A
material naming its textures is the first expected writer.

---

## 6. Runtime: fs mounts, then the asset service

**fs** (`engine/fs`, deps sys + pack) knows bytes, never assets.  An ordered mount table
maps a virtual prefix onto a directory or a `.zip`; the highest-priority mount that has
the file wins, so a loose file shadows a bundled one.  A hashed catalog caches the winner
per path; a DIR mount above the cached winner is re-checked on stat, which is what makes
hot-reload see a newly written file.  Eight verbs: mount / unmount / read / free / exists /
stat / glob / file_count.

Only the host mounts, once: `run_host` mounts `<root>/build/content` (priority 10) over
`<root>/content` (priority 0).  The gui boot path does the same for the sandboxes.  A
shipped exe has one `content/` mount and finds every name unchanged.  Files the machine
produces (pipeline cache, dev font bake cache, logs) are not content and stay on sys.

**asset** (`runtime_service/asset`, deps fs + core + rhi) says what is loaded and who holds
it.  `acquire( name, type )` finds or creates a record keyed by the interned name, tries
the type's extensions in preference order (cooked first), loads synchronously on the first
acquire, and returns an `aid_t { index, generation }`.  `release` unloads at refcount zero
and bumps the generation.  `reload` re-runs the loader in place.  `refresh()` polls every
live record's source mtime and reloads the changed ones, keeping id and refcount; hosts
that want live reload call it a few times a second.  A non-canonical name is refused; one
name, one type.  Built-in types: image (`.tex`, then PNG/JPG/... via stb_image, into a
bindless texture) and shader (`.oshd`, see SHADERS.md).  Game and editor DLLs add types
with `type_register`.

**gui** reads every content byte through fs by name + extension: fonts as
`"font/<family>/<size>"` (cooked bake, else the dev baker, else a resident size), icons
and sprites under `ui/`, its two shaders under `shader/`.  It holds no root and opens no
file outside the mounts.

---

## 7. Shipping

`ship_tool <exe> -target [-config Debug|Release] [-monolithic|-modular] [-verify] [-clean]`
runs build -> verify -> stage -> package -> deploy (`developer/dev_ship` is the logic, the
editor's Deploy window spawns it).

The content set is derived: the exe target plus the modules its `mono_dep` line in
`orb.targets` names, the union of their resource manifests, each name resolved the way
res_tool resolved it and represented by its cooked file under build/content when its
source cooks, closed over every cooked file's reference section.  Verify fails on a name
with no source, two sources, a missing cook, or a corrupt section, and lists files no image
references (they simply do not ship).  Stage copies the set to `<out>/content/<name>.<ext>`
so the shipped exe's single mount finds every name.

The discriminating case: sb_gui boots with `RID( "font/cascadiamono/16" )`, the engine has
recipes for four families, and the package holds exactly that one bake.

---

## 8. Tools and sandboxes

    res_tool      scanner + resolver + manifest writer               (build_tool runs it)
    asset_tool    cook dispatcher for the content phase; also a tree cook (-src/-dst) and
                  a `pack` verb that only sb_asset_image uses
    shader_tool   compile / reflect / cook / header                    (SHADERS.md)
    font_tool     TTF -> .orb_font atlas bake; `info` dumps a bake
    image_tool    SVG icon import (source_content -> content)
    ship_tool     the packager CLI over dev_ship
    launch_tool   gui front end that spawns the tools above

    sb_res        markers, canonical form, hashing, res_path, the manifest of a fixture set
    sb_fs         mounts, zip bundles, loose-over-bundle, stat refresh
    sb_asset_test dedup, extension preference, refcount, stale handles, refresh (headless)
    sb_asset_image  PNG and .tex by name, loose and from a pack, on screen
    sb_asset_shader cook -> .oshd asset -> hash-gated hot reload

Open, small: `RES_TREE` takes the whole subtree; a type filter waits for a site that
over-includes.  Transient rhi resources (render targets, procedural atlases) stay outside
the name space.
