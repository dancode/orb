/*  ASSET_SYSTEM_PLAN.md  --  design + phased build plan for the asset system  */

# Asset System + Filesystem Framework -- Plan

Status: PROPOSED (2026-07-06). Split settled: core/fs = filesystem plumbing (with a file
catalog); runtime_service/asset = asset registry + typed loaders; asset_tool = offline cook
orchestrator. Defaults: synchronous but async-ready load model; vendored stb_image
(source/vendor/stb_image.h) for image decode; runtime loads source formats DIRECTLY first,
cooked engine formats (.tex etc.) introduced later.

--------------------------------------------------------------------------------
## Three tracks (cook-time vs runtime), and where asset_tool sits
--------------------------------------------------------------------------------

Every asset system is two machines that meet at a FORMAT boundary:

  COOK-TIME (asset_tool)                 RUNTIME (engine)
  - dev machine / build server           - shipping engine
  - can be slow; heavy deps ok           - must be fast; minimal deps
  - authoring in (.png/.fbx/.ttf/.wav)   - cooked engine formats in
  - cooked formats + pack/manifest out   - live GPU/CPU resources out

font_tool ALREADY is a cook-time converter: TTF -> .orb_font (orb_font_header_t with
magic+version + glyphs + atlas). It is the precedent and the first converter. build_tool
does NOT invoke it today -- cooking is currently a manual, separate pass.

So the system is THREE tracks that connect at the format/manifest boundary:

  1. core/fs               -- mount + read cooked (and loose source) bytes.
  2. runtime_service/asset -- bytes -> resources; refcount; hot-reload.
  3. asset_tool + formats  -- cook authoring -> cooked, incrementally; pack.
                              font_tool is the first converter; asset_tool orchestrates.

Cooked engine formats (.tex/.mesh/.snd) are the CONTRACT between asset_tool (writer) and
the runtime loaders (readers). The manifest/pack is the contract between asset_tool and
core/fs. We do not have these formats yet (except .orb_font) -- hence the direct-first
approach below.

Loader design consequence -- every runtime loader has TWO source paths from day one:
  - direct mode (now):   load the authoring format live (PNG via stb_image). Iteration.
  - cooked mode (later): load a versioned engine format, upload with zero transform.
Loader picks by extension/header. Dev tolerates direct; ship prefers/requires cooked.
Building loaders format-version-aware now avoids a rewrite when .tex lands.

--------------------------------------------------------------------------------
## asset_tool -- offline cook orchestrator (the build_tool analog for data)
--------------------------------------------------------------------------------

Standalone CLI, links base+sys only, no engine runtime (as asset_tool.c is already
scaffolded). It is a job runner, NOT a monolith that re-implements font baking.

  asset_tool cook foo.png            -> data/foo.tex          (built-in image converter)
  asset_tool cook bar.ttf 16         -> spawns font_tool -> data/bar.orb_font
  asset_tool -src assets/ -dst data/ -> scan tree, cook everything stale (incremental)
  asset_tool -manifest assets.json   -> cook the declared job list
  asset_tool -pack data/ game.pak    -> bundle cooked outputs for shipping

Responsibilities:
  1. Job dispatch  -- source extension -> converter (built-in OR spawned sub-tool such as
                      font_tool, via sys_process_run, which build_tool already uses).
  2. Incremental   -- skip when source hash/mtime == recorded; cook cache/manifest on the
                      cooked side. Make-or-break for iteration speed.
  3. Manifest/index-- emit what got cooked so core/fs can mount it and packaging knows the
                      entry list.
  4. Packaging     -- optionally bundle cooked outputs into a .pak/.zip; ties directly to
                      the fs loose-over-bundle mounts.

Symmetry: build_tool orchestrates cl.exe/link.exe for code; asset_tool orchestrates
converters for data.

--------------------------------------------------------------------------------
## Goal
--------------------------------------------------------------------------------

A clean, minimal asset system backed by a virtual filesystem that:

  - knows what files exist (fast hashed catalog by path across dir + zip mounts),
  - knows what assets are loaded, and tracks loaded / unloaded state,
  - deduplicates: multiple requests for the same asset share one record,
  - gives each asset a stable id that knows its backing resource (e.g. a texture's
    bindless index) so it can be unloaded and reloaded,
  - supports hot-reload (touch a file on disk -> resource re-materializes),
  - eventually loads from packaged bundles (.zip) with a loose-file-override search order.

--------------------------------------------------------------------------------
## Architecture: plumbing below, asset service above
--------------------------------------------------------------------------------

core is plumbing (like the rest of engine). A *file* is plumbing; an *asset* (image, model,
audio -- a thing that decodes into a backend resource and hot-reloads through type-specific
logic) is NOT plumbing. So the system splits across the layer boundary:

  core/fs                 -- filesystem: mounts, path resolution, read bytes, stat, watch,
                             and a hashed file CATALOG. Knows bytes, never "assets".
  runtime_service/asset   -- asset registry (ids/refcounts/state/dedup) + typed loaders.
                             Sits above core, so it calls rhi/draw DIRECTLY.

Two distinct "registries" -- do not conflate them:

  - FILE registry (core/fs): path -> owning mount + offset + size. A catalog of files.
    Built at mount time (scan a dir, or read a .zip central directory). Pure plumbing.
  - ASSET registry (service): asset_id -> { type, state, refcount, typed resource handle }.
    Coupled to asset types and backend resources, so it lives with its loaders.

Because the asset registry lives in the service (above core), it holds TYPED handles
(bindless texture index, mesh handle, ...) and calls rhi directly. There is NO
callback-into-core seam and core never holds a void* resource it does not understand.

--------------------------------------------------------------------------------
## core/fs -- virtual filesystem (bytes + file catalog)
--------------------------------------------------------------------------------

  - Ordered mount table. Each mount maps a virtual prefix -> a backing:
       kind = DIR  (a real directory)     -- Phase 1
       kind = ZIP  (a packaged bundle)    -- Phase 5
  - File catalog: hashed index (reuse sid FNV-1a) path -> (mount, offset, size). Populated
    at mount time; unifies dir + zip lookups; makes exists/read O(1). FIRST mount by
    priority wins, so loose files override bundled ones (standard dev workflow).
  - Watch: forwards sys_filewatch changes on mount roots as a "path changed" signal that
    the asset service subscribes to. fs reports the change; it does NOT know how to rebuild.
  - API (draft):
       bool      fs_mount( const char* vprefix, const char* real_path, int priority );
       void      fs_unmount( const char* vprefix );
       fs_blob_t fs_read( const char* vpath );        // { void* data; u32 size; bool ok; }
       void      fs_free( fs_blob_t* blob );
       bool      fs_exists( const char* vpath );
       bool      fs_stat( const char* vpath, fs_stat_t* out ); // { u32 size; u64 mtime; }
       int       fs_glob( const char* vpat, fs_glob_fn cb, void* ud );
  - Wired into core like cvar/cmd: fs.c included by core.c, slots added to core_api_t,
    init in core_init(). DIR reads route through the new sys whole-file read (Phase 0).
    ZIP is just another mount kind implementing the same read path.

--------------------------------------------------------------------------------
## runtime_service/asset -- asset service (registry + typed loaders)
--------------------------------------------------------------------------------

  - Three-header split (asset.h / asset_api.h / asset_host.h) + hot-reload DLL, like
    rhi/draw/gui. Depends on core (fs) and rhi/draw.
  - asset_id_t = { u32 index; u32 generation; }  -- stale-handle safe.
  - Path -> id hash table (SID of the vpath). Same path twice -> same id = DEDUP.
  - Per-asset record:
       sid_t   path;        // interned virtual path
       u16     type;        // image / model / audio / ...
       u8      state;       // UNLOADED / LOADING / LOADED / FAILED
       i32     refcount;    // acquire/release balance
       void*   resource;    // TYPED backend handle (e.g. bindless texture index)
       u32     bytes;
       u64     mtime;       // source mtime at load, for hot-reload compare
  - Type dispatch by extension: ".png"/".jpg" -> image loader, etc. Custom types can be
    registered by game/editor DLLs via an asset()->type_register API (service-internal
    dispatch, NOT a core seam).
  - API (draft):
       asset_id_t asset_acquire( const char* vpath );   // find-or-create, ++ref, load on 1st
       void       asset_release( asset_id_t id );        // --ref, unload at 0
       void*      asset_get( asset_id_t id );            // typed resource or NULL if not LOADED
       int        asset_state( asset_id_t id );
       void       asset_reload( asset_id_t id );
  - Synchronous load first cut: acquire -> fs_read -> decode -> create backend resource ->
    LOADED. LOADING state + id indirection RESERVED so a background/streaming loader slots
    in later with no API break.
  - Asset records (with rhi handles) live in preserved module state, so they survive
    hot-reload of the service DLL -- same contract as any other service.
  - loaders/:
       asset_image.c   -- stb_image decode -> rhi texture_create + upload_texture +
                          register_texture; resource = bindless index. unload =
                          unregister_texture + texture_destroy.
       asset_model.c   -- later
       asset_audio.c   -- later

--------------------------------------------------------------------------------
## Phases
--------------------------------------------------------------------------------

Phase 0 -- Filesystem primitive (sys)  [shared by both halves]   [DONE 2026-07-06]
   - Add whole-file I/O to sys (win_file.c + sys_host.h):
       sys_file_read_entire( path ) -> sys_file_data_t { void* data; u32 size; bool ok; }
                                       (buffer has a hidden trailing NUL; size excludes it)
       sys_file_free( sys_file_data_t* )
       sys_file_write_entire( path, data, size ) -> bool
       sys_file_exists( path ) -> bool ; sys_file_size( path ) -> u32
   - Win32 impl (CreateFileA/ReadFile/WriteFile), chunked loops, >4GB rejected.
   - asset_tool: added a target (orb.targets: target asset_tool, dep sys, folder 08_TOOL,
     added to orb_build + orb_all solutions); switched include to sys_host.h; cook_asset is
     now a real read->write copy (no transform yet -- that is the COOK track).
   - Proof: asset_tool round-trips asset_tool.c to a temp file; MD5 identical; 2463 bytes.

   NOTE: this only unblocks asset_tool's plumbing. asset_tool's real design (cook
   orchestrator) is the separate COOK track below and is not required for runtime Phases
   1-4 -- runtime loads source formats directly until cooked formats exist.

Phase 1 -- core/fs virtual filesystem (DIR mounts + catalog)   [DONE 2026-07-06]
   - core/fs/fs.{h,c} + core_fs.c glue (included by core.c, mirrors core_cvar.c). Wired into
     core_api_t (8 slots: fs_mount/unmount/read/free/exists/stat/glob/file_count) and
     core_init/exit (fs_system_init/exit).
   - Mount table (vprefix -> real dir, priority; "" prefix matches all). Resolution: among
     matching mounts, highest priority whose file actually exists wins (loose-over-bundle).
   - Catalog: open-addressing hash keyed by sid_hash_len(vpath) (case-insensitive), LAZILY
     filled on first successful resolve (OS is source of truth for DIR mounts; ZIP will fill
     eagerly). Caps: FS_PATH_MAX 256, FS_MAX_MOUNTS 16, FS_MAX_FILES 4096. Entries cache the
     resolved real path so repeat reads skip the mount scan.
   - fs_read routes DIR reads through sys_file_read_entire (Phase 0). fs_free just free()s the
     malloc'd blob. fs_glob = "vdir/pattern" split -> sys_file_glob per matching mount (flat).
   - Watch forwarding: NOT built yet -- deferred to Phase 4 (hot-reload) where it is consumed.
   - BUILD NOTE: core now references sys symbols, and the build tool links only DIRECT deps.
     Every exe that statically links core must also list sys (hosts already do; sb_engine_core
     needed `dep core sys` added).
   - Proof (sb_engine_core fs_test): write a scratch file via sys, mount "data/" -> CWD, then
     exists/stat/read match (22 bytes); 2nd read is a catalog hit (file_count==1); missing
     path = not found; backslash+mixed-case vpath folds to the same file.

Phase 2 -- runtime_service/asset registry (synchronous, no loaders yet)
   - New service module (three-header split, hot-reload DLL). Handle table + path hash +
     refcount + state + type dispatch scaffolding.
   - Proof: register a trivial "blob" type, acquire same vpath twice -> same id, refcount==2;
     release twice -> unloaded.

Phase 3 -- Image loader + on-screen proof
   - loaders/asset_image.c using source/vendor/stb_image.h -> rhi texture. sb_engine_asset
     (or extend a vulkan/gui sandbox) loads a PNG by id and draws it via draw()->image.
   - Proof: on-screen textured quad from an acquired asset id.

Phase 4 -- Hot-reload
   - fs watch -> "path changed" -> asset service re-runs the loader for the affected id
     (mtime compare as fallback). Proof: touch the PNG -> texture swaps live.

Phase 5 -- Packaged (.zip) mounts
   - Vendor miniz (inflate). ZIP mount kind: parse central directory into the fs catalog,
     read = locate + inflate. Same fs_read interface; loose-over-bundle override already
     handled by mount priority. Proof: same PNG served from a .zip, still hot-reloadable
     when a loose copy shadows it.

COOK TRACK (asset_tool) -- parallel to runtime phases; not a blocker for 1-4.

Cook-A -- asset_tool as job runner
   - Turn asset_tool from stub into: single-job CLI (cook <src> <dst>), extension->converter
     dispatch, and sub-tool spawning (font_tool via sys_process_run). Reuse build_tool's
     spawn/log/env primitives where sensible.
   - Proof: asset_tool cook bar.ttf 16 produces the same .orb_font font_tool would.

Cook-B -- incremental tree cook + manifest
   - -src/-dst tree scan, staleness by source hash/mtime, cook cache + emitted manifest of
     cooked outputs. Proof: second run is a no-op; touching one source re-cooks only it.

Cook-C -- first cooked engine format (.tex) + cooked loader path
   - Define .tex (magic+version header + pre-decoded/mip'd payload); asset_tool image
     converter writes it; runtime asset_image gains its cooked path. Proof: same quad from
     a .tex with zero runtime decode.

Cook-D -- packaging
   - -pack cooked tree -> .pak/.zip that core/fs mounts. Proof: game runs from a pack with
     loose files overriding.

--------------------------------------------------------------------------------
## Open decisions
--------------------------------------------------------------------------------

1. Load model: synchronous-but-async-ready (recommended) vs threaded from day one.
2. Whether the asset service should be a hot-reload DLL from the start (recommended, matches
   rhi/draw/gui) vs a static lib until the API settles.
3. asset_tool shape: orchestrator that spawns sub-tools like font_tool (recommended, mirrors
   build_tool) vs monolith linking all converters. (Default taken: orchestrator.)
4. Cook timing: runtime loads source directly first, cooked formats later (recommended,
   fastest to pixels) vs cooked-first. (Default taken: direct-first.)

--------------------------------------------------------------------------------
## Notes / constraints honored
--------------------------------------------------------------------------------

  - ASCII only; three-header split for the asset service per convention.
  - core gains no new dependency (fs uses sys, already a core dep); core stays asset-free.
  - Decoders (stb_image now, miniz later) compile only in their consuming TU, never in core.
  - Two registries kept distinct: file catalog (core/fs) vs asset registry (service).
  - Smallest-mechanism: registry lives WITH its loaders (no cross-layer callback seam).
