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

Phase 2 -- runtime_service/asset registry (synchronous, no loaders yet)   [DONE 2026-07-06]
   - New STATIC service (three-header split asset.h/_api.h/_host.h + asset.c unity entry +
     asset_registry.c impl + asset_api.c wiring). NOTE: services (rhi/draw/gui) are STATIC
     libs with a mod_desc, NOT hot-reload DLLs as the plan assumed -- asset matches them
     (type static, dep core, registered via mod_static; func_api_size = sizeof asset_api_t;
     state in file-scope globals since a static service never hot-reloads).
   - asset_id_t { u32 index (1-based; 0=invalid); u32 generation }. Records: sid path, hash,
     type, state, refcount, resource, bytes, mtime, generation. Open-addressing path index
     (sid-hash -> slot, tombstones) for O(1) dedup; sid interning of the slash-normalized,
     case-folded vpath makes "a/B.png" == "a\\b.png" the SAME record for free.
   - Type dispatch by extension: asset()->type_register(name, exts[], load, unload, ud) ->
     type id; acquire picks the type from the file extension. No GPU types yet.
   - Vtable: type_register / acquire / release / reload / get / state / valid / refcount /
     count. Synchronous load (acquire: fs_read -> type.load -> LOADED/FAILED); LOADING + id
     indirection reserved for a later async loader. reload() = unload+load in place.
   - Proof (sb_engine_asset): boots sys+ref+core+asset via mod_static; blob type over a
     scratch file; acquire x2 = same id refcount 2 loads 1 (dedup); LOADED + byte match;
     backslash/case alt-form folds to same record (no reload); release unwinds refcount then
     unloads at 0; stale handle rejected by generation; missing file = FAILED but releasable.

Phase 3 -- Image loader + on-screen proof   [DONE 2026-07-06]
   - loaders/asset_image.{h,c}: built-in "image" type (auto-registered in asset_mod_init for
     .png/.jpg/.jpeg/.bmp/.tga/.psd/.gif/.hdr). Loader = stb_image decode (STB_IMAGE_IMPLEMENTATION
     compiled ONLY in the asset TU, STBI_NO_STDIO, forced RGBA8) -> rhi texture_create (RGBA8_UNORM,
     SAMPLED|TRANSFER_DST, GPU_ONLY) -> upload_texture -> register_texture. Resource is a private
     image_res_t whose FIRST member is the public asset_image_t { u32 tex_index; u32 width; height },
     so the stored pointer doubles as the asset_image_t* returned by get(); the rhi_texture_t handle
     stays private and is released on unload (unregister_texture + texture_destroy).
   - Service now `dep core rhi` (loader calls rhi()); asset.c adds MOD_USE_RHI + includes the loader
     unit; asset_mod_init/reload MOD_FETCH_RHI; mod_desc deps { "core", "rhi" }. Direct-path only
     (cooked .tex slots in later behind an extension/header check). draw NOT a service dep -- the
     DRAWING code (sandbox) owns draw()->image; the loader only needs rhi.
   - BUILD NOTE (direct-dep linking again): every asset consumer now also links rhi (+ rhi's dep
     app). Headless sb_engine_asset gained `dep rhi app` and mod_static(app/rhi) -- rhi_mod_init only
     probes vulkan-1.dll (no device until rhi()->init()), so the blob test stays headless.
   - Proof: new windowed sandbox sb_asset_image (source/sandbox/asset; dep sys ref mod core app rhi
     draw asset; in orb_sandbox_vulkan). Boots the stack, mounts CWD, acquire("gui_issue.png") ->
     get() -> draw()->image centered+aspect-fit each frame via draw()->begin_pass/end_pass. Optional
     argv[1]=frame count for a clean headless smoke exit. Verified: RTX 3080 device up, PNG decoded
     1656x1050 -> bindless tex_index=1 behind asset id {1,0}, 90 frames, exit 0.

Phase 4 -- Hot-reload   [DONE 2026-07-06]
   - Mechanism = mtime-poll, caller-driven (smallest thing that swaps the PNG live; a real OS
     file watch is deferred -- it would only gate WHICH records get re-stat'd, same reload path).
     New vtable entry asset()->refresh(): scans every live record, fs_stat's its source, and
     re-runs the loader IN PLACE for any whose mtime changed (also retries FAILED records so a
     since-appeared file recovers). Id + refcount are preserved, so a caller that re-get()s sees
     the fresh resource with no handle churn. Returns the reload count. A momentarily unreadable
     source (editor mid-write) stats as gone and is skipped until it settles.
   - ENABLING FIX in core/fs (this was the Phase-1 "watch forwarding not built yet" gap):
     fs_stat cached size/mtime in the catalog, so a rewritten loose file never looked changed.
     fs_stat now RE-STATS live on a catalog hit for DIR mounts (the entry still caches the
     resolved real path; only size/mtime are volatile) and reports a miss if the file vanished.
     The OS is the source of truth for DIR mounts, per fs.h -- ZIP entries (Phase 5) will keep
     their eager catalog values.
   - Proof A (headless, deterministic -- sb_engine_asset asset_refresh_test): acquire a blob;
     refresh() with an unchanged source = 0 reloads / no new load; rewrite the file (+40ms sleep
     past Windows' ~15ms file-time granularity) then refresh() = 1 reload, loads 1->2, SAME id,
     refcount preserved, bytes now v2. Proof B (on-screen -- sb_asset_image): loop calls
     refresh() each frame and re-get()s on a nonzero return (the old image_res_t is freed by the
     in-place reload, so the cached pointer must be refreshed; a transient FAILED save skips the
     draw). Edit/re-save gui_issue.png and the texture swaps live; 90-frame headless smoke green.

Phase 5 -- Packaged (.zip) mounts   [DONE 2026-07-06]
   - Vendored miniz 3.0.2 amalgamation at source/vendor/miniz.{c,h} (+ miniz_LICENSE.txt).
     Compiled as its OWN object (orb.targets core: second `unit fs/fs_zip_miniz.c`), NOT folded
     into core.c's unity -- fs_zip_miniz.c sets MINIZ_NO_STDIO (shared via fs/fs_zip.h) and wraps
     the include in #pragma warning(push,0) so miniz's own warnings don't trip the engine's /WX.
     Archive WRITING stays enabled (tests build zips in memory; cook track will want it).
   - ZIP mount kind in fs.c (fs_mount_kind_t DIR/ZIP, internal to fs.c -- public fs.h unchanged,
     no API/vtable growth). fs_mount auto-detects a bundle by a ".zip" real_path: it reads the
     whole archive via sys (sys owns disk I/O) and opens a miniz reader over the bytes with
     mz_zip_reader_init_mem (bytes kept alive for the mount's life; freed at unmount/exit).
   - Resolution is LAZY and zip-aware (NOT eager catalog fill) -- this is what preserves
     loose-over-bundle: fs_resolve asks each matching mount "do you have it" (DIR = file exists;
     ZIP = mz_zip_reader_locate_file >= 0) and the highest-priority hit wins, so a loose DIR file
     shadows a zip entry for free. The catalog caches the winner's mount+`real` (an OS path for
     DIR, the in-archive name for ZIP). fs_read branches on kind: ZIP = locate + file_stat +
     extract_to_mem into a malloc'd size+1 buffer (hidden trailing NUL per fs_blob_t; freed by
     fs_free like any blob). fs_glob skips ZIP mounts (bundle enumeration not wired this phase;
     reads still work). Helpers: fs_has_zip_ext / fs_zip_rel / fs_zip_locate / fs_zip_meta /
     fs_zip_read / fs_entry_meta.
   - HOT-RELOAD interaction (Phase 4): miniz's file_stat carries no per-entry mtime, and a bundle
     is immutable in place, so every zip entry reports the .zip's own mtime (captured at mount).
     fs_stat's live-restat is gated to DIR mounts only (a ZIP catalog hit returns cached size/
     mtime -- its `real` is an in-archive name, not an OS path), so a zip-backed asset never
     spuriously hot-reloads while a loose shadow (DIR-backed) still does.
   - Proof A (headless, deterministic -- sb_engine_core fs_zip_test): builds a two-file zip in
     memory with the miniz writer (no committed binary), writes it out, then: mount ok; read a
     nested DEFLATE'd entry (byte match -> inflate works); bundle serves shared.txt "FROM ZIP";
     zip stat stable+nonzero across two calls; missing entry = not found. Then a second scenario
     mounts the zip (prio 0) + loose CWD (prio 10): read resolves to the loose "FROM LOOSE"
     (loose-over-bundle); the loose shadow's mtime changes after a rewrite (+40ms) = still
     hot-reloadable; a path only in the bundle still resolves through the shadow mount.
   - Proof B (on-screen -- sb_asset_image "zip" arg): packs gui_issue.png into a scratch
     sb_asset_pack.zip (DEFLATE) at startup, mounts the bundle at "", and acquire()s the PNG from
     it -- the asset service reads through core/fs, so the call is identical to loose mode; only
     the backing changes. Verified: RTX 3080, "serving from bundle", PNG decoded 1656x1050 ->
     bindless tex_index 1, 60-frame smoke green; scratch zip deleted at shutdown.

COOK TRACK (asset_tool) -- parallel to runtime phases; not a blocker for 1-4.

Cook-A -- asset_tool as job runner [DONE 2026-07-06]
   - asset_tool is now a job runner (source/tools/asset_tool/asset_tool.c): CLI
     `cook <src> <dst> [args...]` dispatches by src extension.
       * .ttf/.otf -> spawn font_tool via sys_process_run: located next to asset_tool with
         sys_exe_dir (both land in bin/), command line
         `"<exedir>\font_tool.exe" "<src>" <size> "<dst>"` (paths quoted for spaces). size =
         args[0] or default 16 (ASSET_TOOL_DEFAULT_FONT_SIZE); non-zero child exit is an error.
       * image/other -> built-in cook_copy (read->write passthrough); this is the placeholder
         for the .png->.tex converter that lands in Cook-C.
     Helpers: path_ext (last dot after the last separator), ext_is (case-insensitive),
     ext_is_font. Legacy bare `<src> <dst>` grammar dropped in favor of the `cook` verb.
     Still links base+sys only; no engine runtime.
   - Proof (verified): `asset_tool cook C:\Windows\Fonts\arial.ttf out.orb_font 16` spawns
     font_tool and produces a BYTE-IDENTICAL atlas to `font_tool arial.ttf 16 out.orb_font`
     (same MD5, 264080 bytes). Copy path round-trips identically; no-args / missing-dst print
     usage and exit 1. NOTE: font_tool needs freetype.dll findable at runtime (now copied into
     bin/); this is font_tool's own dependency, not asset_tool's.

Cook-B -- incremental tree cook + manifest [DONE 2026-07-06]
   - New CLI mode `asset_tool -src <dir> -dst <dir> [-f]` alongside the Cook-A `cook` verb.
     Recursively walks -src, mirrors each file into -dst at the same relative path (fonts ->
     .orb_font, image/other -> copy), skips sources whose mtime is unchanged since the last run,
     and writes two bookkeeping files under -dst:
       * .cook_cache      -- lines "<src_mtime> <src_rel>"; the staleness record.
       * cook_manifest.txt -- list of cooked output rel-paths (the entry list core/fs / packaging
                              will consume). Only successfully-present outputs are recorded, so a
                              failed cook stays stale and is retried next run.
     Staleness = source mtime vs cached mtime AND output-file exists (missing output re-cooks).
     -f forces a full re-cook. Tree-mode fonts bake at the default 16px (per-font size is a
     future -manifest concern). Bounded to COOK_MAX_JOBS=4096 files (BSS job/cache arrays).
   - NEW SYS PRIMITIVES (win_file.c, declared in sys_host.h; sys is Windows-only today):
       * bool sys_dir_make(path)            -- recursive mkdir -p (idempotent); needed to mirror
                                               the source tree's subdirs under -dst.
       * int  sys_dir_walk(root, cb, ud)    -- recursive file enumeration (reuses sys_glob_fn;
                                               descends all subdirs, reports files only). The
                                               existing sys_file_glob is single-dir + skips dirs,
                                               so it could not drive a tree walk.
     Direct-linked like sys_file_glob (no vtable/api wiring); asset_tool still links base+sys only.
   - Proof (verified): tree of textures/{a,b}.png + fonts/arial.ttf. RUN1 cold = 3 cooked/0 fresh
     (mirrored subdirs created, manifest + cache written). RUN2 unchanged = 0 cooked/3 up-to-date
     (no-op). RUN3 after touching only b.png = 1 cooked/2 up-to-date (incremental). RUN4 -f =
     3 cooked. Full Debug build of all targets clean (sys is foundational).

Cook-C -- first cooked engine format (.tex) + cooked loader path  [DONE 2026-07-06]
   - Define .tex (magic+version header + pre-decoded/mip'd payload); asset_tool image
     converter writes it; runtime asset_image gains its cooked path. Proof: same quad from
     a .tex with zero runtime decode.
   - Implemented:
     * Shared format contract: source/runtime_service/asset/loaders/asset_tex.h -- 32-byte
       header (magic 'OTEX' + version 1 + width/height/format/mip_levels/data_size/flags)
       followed by tightly packed RGBA8. Dependency-free (just u32 from orb.h) so both the
       tool and the engine include it. Only mip 0 / RGBA8 today; mip_levels + flags reserved.
     * Writer (asset_tool): new cook_image() decodes a source image with stb_image (compiled
       tool-local, STB_IMAGE_IMPLEMENTATION + STBI_NO_STDIO) to RGBA8 and writes header+pixels
       as one whole-file buffer. cook_file dispatch = font -> font_tool, image -> .tex,
       else -> copy. job_dst_rel maps image exts -> .tex; ext_is_image mirrors ASSET_IMAGE_EXTS
       (minus .tex). asset_tool still links base+sys only (stb + asset_tex.h are header-only).
     * Reader (asset_image.c): asset_image_load sniffs the 'OTEX' magic in the first 32 bytes;
       cooked path validates version/format/size then uploads the payload with ZERO decode;
       source path unchanged. Both converge on a shared image_upload_rgba8() helper (texture
       create -> upload -> bindless register -> resource). ".tex" added to ASSET_IMAGE_EXTS so
       acquire() dispatches it to the image type (also bumped ASSET_TYPE_EXTS 8 -> 12, since
       .tex was the 9th ext and silently truncated).
   - Proof (verified): `asset_tool cook gui_issue.png gui_issue.tex` -> 1656x1050 RGBA8, header
     dumps OTEX/v1/fmt1/mips1, data_size 6955200 == w*h*4, file 6955232 == 32+payload. New
     sb_asset_image "tex" mode acquires the .tex twin: renders the identical 1656x1050 quad,
     5-frame headless smoke exit 0, same tex_index as the source-decode path. Tree cook of
     src/{b.png, textures/a.png} mirrors to out/{b.tex, textures/a.tex} + manifest lists .tex.

Cook-D -- packaging
   - -pack cooked tree -> .pak/.zip that core/fs mounts. Proof: game runs from a pack with
     loose files overriding.

--------------------------------------------------------------------------------
## Open decisions
--------------------------------------------------------------------------------

1. Load model: synchronous-but-async-ready (recommended) vs threaded from day one.
2. RESOLVED: asset is a STATIC service. The premise (rhi/draw/gui are hot-reload DLLs) was
   wrong -- those services are STATIC libs with a mod_desc, registered via mod_static. asset
   matches them (type static, dep core). Loaders iterate fast via host rebuild, not DLL swap.
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
