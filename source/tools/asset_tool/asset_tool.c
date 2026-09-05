/*==============================================================================================

    asset_tool.c (asset cooker -- offline cook orchestrator)

    The build_tool analog for data: a job runner that dispatches a source file to a converter
    chosen by its extension. Some converters are built in (they read/write bytes directly); some
    are spawned sub-tools (font_tool bakes .ttf -> .orb_font, via sys_process_run -- the same
    child-process primitive build_tool uses to drive cl.exe).

    Usage:

        asset_tool cook <src> <dst> [args...]        single job
            .ttf/.otf   -> spawn font_tool  <src> <size> <dst>   (args[0] = size_px, default 16)
            image       -> built-in .tex converter (pre-decode to RGBA8)
            .hlsl       -> spawn shader_tool cook <src> -o <dst> -T <profile>, where the
                           profile comes from the filename's stage tag (foo.vs.hlsl -> vs_6_0;
                           also ps/cs/gs/hs/ds).  Untagged .hlsl fails loudly; .hlsli copies.
            .recipe     -> what its "kind" line says; a font recipe (size, optional sdf /
                           range, and a face -- its own "face" line, else the "face" in the
                           family.txt beside it) spawns font_tool like a .ttf does
            other       -> built-in verbatim copy

        asset_tool -src <dir> -dst <dir> [-f]        incremental tree cook
            Walks <dir> recursively, cooks each file into a mirrored path under -dst, skips
            sources whose mtime is unchanged since the last run (unless -f forces a full cook),
            and emits a cook cache + manifest under -dst.

        asset_tool pack <cooked_dir> <out.zip>       package a cooked tree
            Bundles every output listed in <cooked_dir>/cook_manifest.txt into <out.zip>, which
            core/fs mounts as a read-only bundle (a higher-priority loose mount still shadows it).

        asset_tool -list <manifests.txt> -root <dir>... -out <dir> [-check] [-f] [-silent]
            The build's cook.  Reads the resource manifests res_tool wrote (one path per line
            in <manifests.txt>, or each given directly with -manifest <file>), resolves every
            name against the content roots in priority order, and cooks the names that need a
            cooked form -- a stage-tagged .hlsl, a .recipe -- into <out>/<name>.<cooked ext>.
            Only what is missing or older than its inputs is cooked; -check reports instead
            (exit 3 when anything needs a cook); -f cooks everything.  -tool <name>=<path>
            names the newest source of a tool in the cook chain, so an edit to the tool
            counts as an input too (build_tool passes one per tool).

    Cook scope (docs/CONTENT.md): single-job CLI + extension dispatch + sub-tool spawn; tree
    scan + staleness cache + manifest; cooked .tex converter; manifest-driven .zip packaging;
    resource-manifest cook for build_tool's content phase.

    Tools that don't need hot-reload, a service registry, or a game loop skip the module system
    entirely and call sys directly.

    Link list for this executable:

        base            (headers only -- unity built in)
        sys             (file_io, clock, process spawn, dir walk/make -- statically linked)
        pack            (ZIP bundle writer -- the engine-wide compression library)

    Plus tool-local vendored code compiled straight in: stb_image (source decode -> .tex).
    No core, no module system, no app.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/base.h"
#include "engine/sys/sys_host.h"
#include "engine/res/res_cook.h"
#include "runtime_service/asset/loaders/asset_tex.h"

/* stb_image: memory-only decode. The converter uses it to pre-decode a source image to RGBA8
   before writing the cooked .tex payload. Implementation compiled right here (tool-local). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "vendor/stb_image.h"

/* pack ZIP writer for -pack: the archive is built in a heap block and written via sys. */
#include "engine/pack/pack_host.h"

/*============================================================================================*/

#define ASSET_TOOL_DEFAULT_FONT_SIZE 16

#define COOK_PATH_MAX     512
#define COOK_MAX_JOBS     4096
#define COOK_CACHE_FILE   ".cook_cache"
#define COOK_MANIFEST_FILE "cook_manifest.txt"

/* path_ext -- returns a pointer to the extension (past the last '.'), or "" if none. The '.'
   must come after the last path separator so a dotted directory does not fool it. */
static const char*
path_ext( const char* path )
{
    const char* dot = NULL;
    for ( const char* p = path; *p; ++p )
    {
        if ( *p == '.' )
            dot = p;
        else if ( *p == '/' || *p == '\\' )
            dot = NULL;
    }
    return dot ? dot + 1 : "";
}

/* ext_is -- case-insensitive extension compare (ext has no leading dot). */
static bool
ext_is( const char* ext, const char* want )
{
    while ( *ext && *want )
    {
        char a = *ext, b = *want;
        if ( a >= 'A' && a <= 'Z' )
            a = ( char )( a - 'A' + 'a' );
        if ( b >= 'A' && b <= 'Z' )
            b = ( char )( b - 'A' + 'a' );
        if ( a != b )
            return false;
        ++ext;
        ++want;
    }
    return *ext == 0 && *want == 0;
}

/* Dispatch predicates over the shared source-extension table (engine/res/res_cook.h), the
   same one res_tool records cooked paths with, so a cooked file is always where the table
   says it is.  Image formats mirror ASSET_IMAGE_EXTS on the runtime side (minus ".tex", the
   output); .hlsli headers are not shaders and copy verbatim. */
static bool
ext_is_font( const char* ext )
{
    return res_kind_from_ext( ext ) == RES_KIND_FONT;
}

static bool
ext_is_image( const char* ext )
{
    return res_kind_from_ext( ext ) == RES_KIND_IMAGE;
}

static bool
ext_is_hlsl( const char* ext )
{
    return res_kind_from_ext( ext ) == RES_KIND_SHADER;
}

/* shader_profile_from_name -- derive the dxc target profile from the stage tag the naming
   convention embeds in the filename: foo.vs.hlsl -> vs_6_0, foo.ps.hlsl -> ps_6_0, likewise
   cs/gs/hs/ds.  Returns false (profile untouched) when the tag is missing or unknown -- an
   untagged .hlsl cannot be cooked and must fail loudly, not guess a stage. */
static bool
shader_profile_from_name( const char* src_path, char* out, size_t cap )
{
    /* The tag is the extension of the path with ".hlsl" stripped. */
    const char* ext = path_ext( src_path );          /* -> "hlsl" */
    if ( ext == src_path || ext[ -1 ] != '.' )
        return false;

    char stem[ COOK_PATH_MAX ];
    int  stem_len = ( int )( ext - 1 - src_path );   /* chars before the ".hlsl" dot */
    if ( stem_len <= 0 || stem_len >= ( int )sizeof( stem ) )
        return false;
    memcpy( stem, src_path, ( size_t )stem_len );
    stem[ stem_len ] = 0;

    const char* tag = path_ext( stem );              /* -> "vs" from "foo.vs" */
    static const char* const tags[] = { "vs", "ps", "cs", "gs", "hs", "ds" };
    for ( u32 i = 0; i < sizeof( tags ) / sizeof( tags[ 0 ] ); ++i )
    {
        if ( ext_is( tag, tags[ i ] ) )
        {
            snprintf( out, cap, "%s_6_0", tags[ i ] );
            return true;
        }
    }
    return false;
}

/*============================================================================================*/
/*  Converters                                                                                */
/*============================================================================================*/

/* cook_copy -- built-in passthrough converter: read src, write dst verbatim. Used for formats
   with no dedicated cooker yet (everything that is not a font or a source image). */
static bool
cook_copy( const char* src_path, const char* dst_path )
{
    sys_file_data_t src = sys_file_read_entire( src_path );
    if ( !src.ok )
    {
        fprintf( stderr, "asset_tool: error: could not read %s\n", src_path );
        return false;
    }

    u32  size  = src.size; /* capture before free zeroes the result */
    bool wrote = sys_file_write_entire( dst_path, src.data, src.size );
    sys_file_free( &src );

    if ( !wrote )
    {
        fprintf( stderr, "asset_tool: error: could not write %s\n", dst_path );
        return false;
    }

    printf( "asset_tool:   copy %s -> %s (%u bytes)\n", src_path, dst_path, size );
    return true;
}

/* cook_font -- spawn font_tool to bake a TTF/OTF into an .orb_font atlas. font_tool lives next
   to asset_tool in bin/, so we locate it via the executable directory rather than PATH.
   font_tool's CLI is: font_tool <input.ttf> <size_px> <output.orb_font>. */
static bool
cook_font( const char* src_path, const char* dst_path, int size_px )
{
    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    /* Quote every path in case bin/ or the asset paths contain spaces. */
    char cmd[ 1536 ];
    snprintf( cmd, sizeof( cmd ), "\"%s\\font_tool.exe\" \"%s\" %d \"%s\"", exe_dir, src_path,
              size_px, dst_path );

    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) )
    {
        fprintf( stderr, "asset_tool: error: could not launch font_tool (is it built?)\n" );
        return false;
    }
    if ( res.exit_code != 0 )
    {
        fprintf( stderr, "asset_tool: error: font_tool exited %d for %s\n", res.exit_code, src_path );
        return false;
    }

    printf( "asset_tool:   font %s -> %s (%dpx, %.0f ms)\n", src_path, dst_path, size_px,
            res.elapsed_seconds * 1000.0 );
    return true;
}

/* cook_shader -- spawn shader_tool to cook a stage-tagged .hlsl into an .oshd container
   (SPIR-V + reflection tables, rhi_shader_format.h).  shader_tool lives next to asset_tool in
   bin/ (the font_tool pattern); the dxc profile comes from the filename's stage tag, so no
   per-shader build metadata is needed.  Note the staleness cache is per-file: editing a
   shared .hlsli include does not re-trigger dependents -- use -f after include changes. */
static bool
cook_shader( const char* src_path, const char* dst_path )
{
    char profile[ 16 ];
    if ( !shader_profile_from_name( src_path, profile, sizeof( profile ) ) )
    {
        fprintf( stderr, "asset_tool: error: %s has no stage tag -- name it foo.vs.hlsl / "
                         "foo.ps.hlsl / foo.cs.hlsl (also gs/hs/ds)\n", src_path );
        return false;
    }

    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    char cmd[ 1536 ];
    snprintf( cmd, sizeof( cmd ), "\"%s\\shader_tool.exe\" cook \"%s\" -o \"%s\" -T %s",
              exe_dir, src_path, dst_path, profile );

    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) )
    {
        fprintf( stderr, "asset_tool: error: could not launch shader_tool (is it built?)\n" );
        return false;
    }
    if ( res.exit_code != 0 )
    {
        fprintf( stderr, "asset_tool: error: shader_tool exited %d for %s\n", res.exit_code,
                 src_path );
        return false;
    }

    printf( "asset_tool:   oshd %s -> %s (%s, %.0f ms)\n", src_path, dst_path, profile,
            res.elapsed_seconds * 1000.0 );
    return true;
}

/* cook_image -- pre-decode a source image to RGBA8 and write the cooked .tex (asset_tex.h):
   a fixed header followed by the tightly packed pixel payload. The runtime loader then uploads
   it with zero decode. Header + pixels are written as one buffer (sys does whole-file writes). */
static bool
cook_image( const char* src_path, const char* dst_path )
{
    sys_file_data_t src = sys_file_read_entire( src_path );
    if ( !src.ok )
    {
        fprintf( stderr, "asset_tool: error: could not read %s\n", src_path );
        return false;
    }

    int      w = 0, h = 0, comp = 0;
    stbi_uc* pixels =
        stbi_load_from_memory( ( const stbi_uc* )src.data, ( int )src.size, &w, &h, &comp, STBI_rgb_alpha );
    sys_file_free( &src );
    if ( !pixels )
    {
        fprintf( stderr, "asset_tool: error: image decode failed for %s: %s\n", src_path,
                 stbi_failure_reason() );
        return false;
    }

    asset_tex_header_t hdr = { 0 };
    hdr.magic      = ASSET_TEX_MAGIC;
    hdr.version    = ASSET_TEX_VERSION;
    hdr.width      = ( u32 )w;
    hdr.height     = ( u32 )h;
    hdr.format     = ASSET_TEX_FORMAT_RGBA8;
    hdr.mip_levels = 1;
    hdr.flags      = 0;

    /* An image names no other resource: the reference section (res_ref.h) is empty. */
    hdr.ref_count  = 0;
    hdr.ref_size   = 0;
    hdr.ref_offset = ( u32 )sizeof( hdr );

    /* u64 math: a huge image would wrap w*h*4 in 32 bits; the format caps the file at u32. */
    u64 pixel_bytes = ( u64 )w * ( u64 )h * 4;
    u64 refs        = hdr.ref_size;
    u64 total       = sizeof( hdr ) + refs + pixel_bytes;
    if ( total > 0xFFFFFFFFull )
    {
        stbi_image_free( pixels );
        fprintf( stderr, "asset_tool: error: %s too large to cook (%dx%d)\n", src_path, w, h );
        return false;
    }
    hdr.data_size = ( u32 )pixel_bytes;

    void* buf = malloc( ( size_t )total );
    if ( !buf )
    {
        stbi_image_free( pixels );
        fprintf( stderr, "asset_tool: error: out of memory cooking %s\n", src_path );
        return false;
    }
    memcpy( buf, &hdr, sizeof( hdr ) );
    memcpy( ( u8* )buf + sizeof( hdr ) + refs, pixels, hdr.data_size );    /* past the (empty) refs */
    stbi_image_free( pixels );

    bool wrote = sys_file_write_entire( dst_path, buf, ( u32 )total );
    free( buf );
    if ( !wrote )
    {
        fprintf( stderr, "asset_tool: error: could not write %s\n", dst_path );
        return false;
    }

    printf( "asset_tool:   tex  %s -> %s (%dx%d RGBA8, %u bytes)\n", src_path, dst_path, w, h,
            ( u32 )total );
    return true;
}

/*============================================================================================*/
/*  Recipes                                                                                   */
/*============================================================================================*/

/* A parsed .recipe (engine/res/res_cook.h): content with no source file of its own.  Lines are
   "<key> <value>", '#' starts a comment.  Only kind "font" cooks today. */
typedef struct recipe_s
{
    res_kind_t kind;
    char       face[ 256 ];     // font_tool input: a source_content/font TTF or an OS face name
    int        size;            // pixel size
    int        sdf;             // 0 = coverage bake; > 0 = distance field with this spread
    char       range[ 128 ];    // font_tool -range spec; "" = the ASCII default
} recipe_t;

/* Callback per "<key> <value>" line of a recipe-style text file (comments and blanks skipped,
   whitespace trimmed).  Returns false to flag the line as an error. */
typedef bool ( *kv_line_fn )( const char* key, const char* val, void* user );

/* Read `path` and feed every key/value line to `fn`.  False when the file cannot be read or
   any line was refused. */
static bool
kv_file_read( const char* path, kv_line_fn fn, void* user )
{
    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
        return false;

    bool  ok = true;
    char* p  = ( char* )fd.data;    /* NUL-terminated by sys_file_read_entire */
    while ( *p )
    {
        char* line = p;
        while ( *p && *p != '\n' )
            ++p;
        if ( *p )
            *p++ = '\0';

        char* hash = strchr( line, '#' );
        if ( hash )
            *hash = '\0';
        while ( *line == ' ' || *line == '\t' || *line == '\r' )
            ++line;
        if ( !*line )
            continue;

        char* key = line;
        char* val = line;
        while ( *val && *val != ' ' && *val != '\t' )
            ++val;
        if ( *val )
            *val++ = '\0';
        while ( *val == ' ' || *val == '\t' )
            ++val;
        for ( char* e = val + strlen( val ); e > val && ( e[ -1 ] == ' ' || e[ -1 ] == '\t' || e[ -1 ] == '\r' ); --e )
            e[ -1 ] = '\0';

        if ( !fn( key, val, user ) )
            ok = false;
    }
    sys_file_free( &fd );
    return ok;
}

typedef struct
{
    const char* src_path;    // for diagnostics
    recipe_t*   out;
} recipe_parse_ctx_t;

static bool
recipe_line( const char* key, const char* val, void* user )
{
    recipe_parse_ctx_t* c = ( recipe_parse_ctx_t* )user;
    recipe_t*           out = c->out;

    if ( strcmp( key, "kind" ) == 0 )
        out->kind = res_kind_from_name( val );
    else if ( strcmp( key, "face" ) == 0 )
        snprintf( out->face, sizeof( out->face ), "%s", val );
    else if ( strcmp( key, "size" ) == 0 )
        out->size = atoi( val );
    else if ( strcmp( key, "sdf" ) == 0 )
        out->sdf = atoi( val );
    else if ( strcmp( key, "range" ) == 0 )
        snprintf( out->range, sizeof( out->range ), "%s", val );
    else
    {
        fprintf( stderr, "asset_tool: error: %s: unknown recipe key '%s'\n", c->src_path, key );
        return false;
    }
    return true;
}

/* The family descriptor's "face" line only; every other key is someone else's business. */
static bool
family_line( const char* key, const char* val, void* user )
{
    if ( strcmp( key, "face" ) == 0 )
        snprintf( ( char* )user, 256, "%s", val );
    return true;
}

/* A font recipe with no "face" line of its own takes the face from content/font/<family>/
   family.txt -- the file beside it on disk, the same one the gui's runtime baker reads for the
   family, so the cooked bake and a runtime bake come from one spelling.  The sibling is looked
   up on disk, not across content roots: a child project that shadows one size of an engine
   family also carries the family's descriptor (or spells the face in the recipe). */
static bool
recipe_inherit_face( const char* src_path, recipe_t* out )
{
    char dir[ 1024 ];
    snprintf( dir, sizeof( dir ), "%s", src_path );
    char* sep = strrchr( dir, '/' );
    char* alt = strrchr( dir, '\\' );
    if ( alt > sep )
        sep = alt;
    if ( !sep )
        return false;
    *sep = '\0';

    char family_path[ 1024 ];
    snprintf( family_path, sizeof( family_path ), "%s/family.txt", dir );
    kv_file_read( family_path, family_line, out->face );    /* a missing file leaves face "" */
    return out->face[ 0 ] != 0;
}

static bool
recipe_parse( const char* src_path, recipe_t* out )
{
    memset( out, 0, sizeof( *out ) );

    recipe_parse_ctx_t ctx = { src_path, out };
    if ( !sys_file_exists( src_path ) )
    {
        fprintf( stderr, "asset_tool: error: could not read %s\n", src_path );
        return false;
    }
    if ( !kv_file_read( src_path, recipe_line, &ctx ) )
        return false;

    if ( out->kind != RES_KIND_FONT )
    {
        fprintf( stderr, "asset_tool: error: %s: recipe kind must be 'font'\n", src_path );
        return false;
    }
    if ( out->size <= 0 )
    {
        fprintf( stderr, "asset_tool: error: %s: a font recipe needs 'size'\n", src_path );
        return false;
    }
    if ( !out->face[ 0 ] && !recipe_inherit_face( src_path, out ) )
    {
        fprintf( stderr, "asset_tool: error: %s: no 'face' line and no family.txt beside it\n", src_path );
        return false;
    }
    return true;
}

/* The cooked extension a recipe produces, read from its "kind" line; NULL on a bad recipe. */
static const char*
recipe_cooked_ext( const char* src_path )
{
    recipe_t r;
    if ( !recipe_parse( src_path, &r ) )
        return NULL;
    return res_kind_cooked_ext( r.kind );
}

/* cook_recipe -- bake the font a recipe describes.  Same font_tool spawn as cook_font, with the
   recipe's face, size and flags on the command line. */
static bool
cook_recipe( const char* src_path, const char* dst_path )
{
    recipe_t r;
    if ( !recipe_parse( src_path, &r ) )
        return false;

    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    char flags[ 192 ] = "";
    int  n            = 0;
    if ( r.sdf > 0 )
        n += snprintf( flags + n, sizeof( flags ) - ( size_t )n, " -sdf=%d", r.sdf );
    if ( r.range[ 0 ] )
        n += snprintf( flags + n, sizeof( flags ) - ( size_t )n, " \"-range=%s\"", r.range );

    char cmd[ 1536 ];
    snprintf( cmd, sizeof( cmd ), "\"%s\\font_tool.exe\" \"%s\" %d \"%s\"%s", exe_dir, r.face,
              r.size, dst_path, flags );

    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) )
    {
        fprintf( stderr, "asset_tool: error: could not launch font_tool (is it built?)\n" );
        return false;
    }
    if ( res.exit_code != 0 )
    {
        fprintf( stderr, "asset_tool: error: font_tool exited %d for %s\n", res.exit_code, src_path );
        return false;
    }

    printf( "asset_tool:   font %s -> %s (%s %dpx%s, %.0f ms)\n", src_path, dst_path, r.face,
            r.size, flags, res.elapsed_seconds * 1000.0 );
    return true;
}

/* cook_file -- dispatch one source file to a converter chosen by its extension. */
static bool
cook_file( const char* src_path, const char* dst_path, int font_size_px )
{
    const char* ext = path_ext( src_path );
    if ( ext_is_font( ext ) )
        return cook_font( src_path, dst_path, font_size_px );
    if ( ext_is_image( ext ) )
        return cook_image( src_path, dst_path );
    if ( ext_is_hlsl( ext ) )
        return cook_shader( src_path, dst_path );
    if ( res_kind_from_ext( ext ) == RES_KIND_RECIPE )
        return cook_recipe( src_path, dst_path );
    return cook_copy( src_path, dst_path ); /* everything else: verbatim copy */
}

/*============================================================================================*/
/*  Single-job mode (Cook-A)                                                                  */
/*============================================================================================*/

static bool
cook_single( const char* src_path, const char* dst_path, char** args, int arg_count )
{
    i64 start   = sys_tick_milliseconds();
    int size_px = ( arg_count > 0 ) ? atoi( args[ 0 ] ) : ASSET_TOOL_DEFAULT_FONT_SIZE;
    if ( size_px <= 0 )
        size_px = ASSET_TOOL_DEFAULT_FONT_SIZE;

    bool ok = cook_file( src_path, dst_path, size_px );
    if ( ok )
        printf( "asset_tool: cook done (%lld ms)\n", ( long long )( sys_tick_milliseconds() - start ) );
    return ok;
}

/*============================================================================================*/
/*  Path helpers                                                                              */
/*============================================================================================*/

/* to_slashes/to_backslashes -- normalize separators in place. Manifest/cache use '/'; the OS
   calls use '\\'. */
static void
path_norm( char* s, char sep )
{
    for ( ; *s; ++s )
        if ( *s == '/' || *s == '\\' )
            *s = sep;
}

/* path_parent -- copy `full` minus its last path component into `out`. */
static void
path_parent( char* out, size_t cap, const char* full )
{
    snprintf( out, cap, "%s", full );
    for ( int i = ( int )strlen( out ) - 1; i >= 0; --i )
    {
        if ( out[ i ] == '\\' || out[ i ] == '/' )
        {
            out[ i ] = '\0';
            return;
        }
    }
    out[ 0 ] = '\0';
}

/* job_dst_rel -- map a source relative path to its cooked output relative path: fonts become
   .orb_font, source images become .tex, shaders .oshd (the stage tag survives: foo.vs.oshd),
   everything else keeps its extension (verbatim copy).  The directory and stem pass through
   unchanged: content file names are canonical lowercase already (res_tool fails the build on
   one that is not), so the cooked file sits under the same name the runtime asks for.  A
   .recipe's cooked extension comes from its "kind" line (read from src_full); a malformed
   recipe copies verbatim and fails loudly when its cook runs. */
static void
job_dst_rel( char* out, size_t cap, const char* src_rel, const char* src_full )
{
    const char* ext  = path_ext( src_rel );
    res_kind_t  kind = res_kind_from_ext( ext );
    const char* cext = kind == RES_KIND_RECIPE ? recipe_cooked_ext( src_full ) : res_kind_cooked_ext( kind );
    int         keep = ( int )( ext - src_rel ); /* chars up to and including the '.' */
    if ( cext )
        snprintf( out, cap, "%.*s%s", keep, src_rel, cext );
    else
        snprintf( out, cap, "%s", src_rel );
}

/*============================================================================================*/
/*  Tree cook (Cook-B): scan, staleness cache, manifest                                       */
/*============================================================================================*/

typedef struct cook_job_s
{
    char src_full[ COOK_PATH_MAX ]; /* absolute source path (from the walk)                   */
    char src_rel[ COOK_PATH_MAX ];  /* path relative to -src, '/'-normalized (cache key)      */
    char dst_rel[ COOK_PATH_MAX ];  /* output path relative to -dst, cooked extension         */
    u64  src_mtime;                 /* source last-write time                                 */
    bool present;                   /* output exists after this run (cooked or already fresh) */
} cook_job_t;

typedef struct cache_ent_s
{
    char rel[ COOK_PATH_MAX ];
    u64  mtime;
} cache_ent_t;

/* These arrays are a few MB -- live in BSS, not on the stack. */
static cook_job_t  s_jobs[ COOK_MAX_JOBS ];
static cache_ent_t s_cache[ COOK_MAX_JOBS ];

typedef struct cook_ctx_s
{
    int  src_root_len; /* strlen of the (trailing-slash-stripped) source root */
    int  job_count;
    bool overflow;
} cook_ctx_t;

/* collect_job -- sys_dir_walk callback: record one source file as a pending job. */
static bool
collect_job( const char* filename, const char* full_path, void* ud )
{
    ( void )filename;
    cook_ctx_t* c = ( cook_ctx_t* )ud;
    if ( c->job_count >= COOK_MAX_JOBS )
    {
        c->overflow = true;
        return false;
    }

    cook_job_t* j = &s_jobs[ c->job_count ];
    snprintf( j->src_full, sizeof( j->src_full ), "%s", full_path );

    /* src_rel = full_path with the root prefix removed, '/'-normalized. */
    const char* rel = full_path + c->src_root_len;
    while ( *rel == '\\' || *rel == '/' )
        ++rel;
    snprintf( j->src_rel, sizeof( j->src_rel ), "%s", rel );
    path_norm( j->src_rel, '/' );

    job_dst_rel( j->dst_rel, sizeof( j->dst_rel ), j->src_rel, j->src_full );
    j->src_mtime = sys_file_time( full_path );
    j->present   = false;
    ++c->job_count;
    return true;
}

/* cache_load -- read <dst>/.cook_cache (lines "<mtime> <src_rel>") into s_cache. */
static int
cache_load( const char* dst_root )
{
    int  count = 0;
    char path[ COOK_PATH_MAX ];
    snprintf( path, sizeof( path ), "%s\\%s", dst_root, COOK_CACHE_FILE );

    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
        return 0; /* first run: no cache yet */

    char* p = ( char* )fd.data; /* NUL-terminated by sys_file_read_entire */
    while ( *p && count < COOK_MAX_JOBS )
    {
        while ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' )
            ++p;
        if ( !*p )
            break;

        char* endp = NULL;
        u64   m    = strtoull( p, &endp, 10 );
        if ( endp == p ) /* malformed line: skip to next newline */
        {
            while ( *p && *p != '\n' )
                ++p;
            continue;
        }
        p = endp;
        while ( *p == ' ' || *p == '\t' )
            ++p;

        char* rel = s_cache[ count ].rel;
        int   k   = 0;
        while ( *p && *p != '\n' && *p != '\r' && k < COOK_PATH_MAX - 1 )
            rel[ k++ ] = *p++;
        rel[ k ] = '\0';

        if ( k > 0 )
        {
            s_cache[ count ].mtime = m;
            ++count;
        }
        while ( *p && *p != '\n' )
            ++p;
    }

    sys_file_free( &fd );
    return count;
}

static bool
cache_lookup( int cache_count, const char* rel, u64* out_mtime )
{
    for ( int i = 0; i < cache_count; ++i )
    {
        if ( strcmp( s_cache[ i ].rel, rel ) == 0 )
        {
            *out_mtime = s_cache[ i ].mtime;
            return true;
        }
    }
    return false;
}

/* Tiny growable string buffer for building the cache/manifest files (whole-file writes only). */
typedef struct sbuf_s
{
    char*  p;
    size_t len;
    size_t cap;
} sbuf_t;

static void
sbuf_add( sbuf_t* s, const char* str )
{
    size_t n = strlen( str );
    if ( s->len + n + 1 > s->cap )
    {
        size_t want = ( s->len + n + 1 ) * 2;
        char*  np   = ( char* )realloc( s->p, want );
        if ( !np )
            return; /* out of memory: drop the append (tool will just write less) */
        s->p   = np;
        s->cap = want;
    }
    memcpy( s->p + s->len, str, n );
    s->len += n;
    s->p[ s->len ] = '\0';
}

/* cook_tree -- the -src/-dst incremental tree cook. Returns process exit code. */
static int
cook_tree( const char* src_arg, const char* dst_arg, bool force )
{
    i64 start = sys_tick_milliseconds();

    /* Strip trailing separators from the roots so relative-path math is exact. */
    char src_root[ COOK_PATH_MAX ];
    char dst_root[ COOK_PATH_MAX ];
    snprintf( src_root, sizeof( src_root ), "%s", src_arg );
    snprintf( dst_root, sizeof( dst_root ), "%s", dst_arg );
    for ( int i = ( int )strlen( src_root ) - 1; i > 0 && ( src_root[ i ] == '\\' || src_root[ i ] == '/' ); --i )
        src_root[ i ] = '\0';
    for ( int i = ( int )strlen( dst_root ) - 1; i > 0 && ( dst_root[ i ] == '\\' || dst_root[ i ] == '/' ); --i )
        dst_root[ i ] = '\0';

    if ( !sys_dir_make( dst_root ) )
    {
        fprintf( stderr, "asset_tool: error: could not create dst dir %s\n", dst_root );
        return 1;
    }

    /* 1. Scan the source tree. */
    cook_ctx_t ctx = { ( int )strlen( src_root ), 0, false };
    sys_dir_walk( src_root, collect_job, &ctx );
    if ( ctx.overflow )
        fprintf( stderr, "asset_tool: warning: more than %d source files -- truncated\n", COOK_MAX_JOBS );
    if ( ctx.job_count == 0 )
    {
        fprintf( stderr, "asset_tool: warning: no source files under %s\n", src_root );
        return 0;
    }

    /* 2. Load the previous cook cache (source mtimes recorded last run). */
    int cache_count = cache_load( dst_root );

    /* 3. Cook the stale jobs; skip the fresh ones. */
    int cooked = 0, fresh = 0, failed = 0;
    for ( int i = 0; i < ctx.job_count; ++i )
    {
        cook_job_t* j = &s_jobs[ i ];

        char dst_full[ COOK_PATH_MAX ];
        snprintf( dst_full, sizeof( dst_full ), "%s\\%s", dst_root, j->dst_rel );
        path_norm( dst_full, '\\' );

        bool stale = force;
        if ( !stale )
        {
            u64  cached_mtime = 0;
            bool have         = cache_lookup( cache_count, j->src_rel, &cached_mtime );
            stale = !have || cached_mtime != j->src_mtime || !sys_file_exists( dst_full );
        }

        if ( !stale )
        {
            ++fresh;
            j->present = true; /* output already exists and is up to date */
            continue;
        }

        /* Make sure the mirrored output directory exists before writing into it. */
        char parent[ COOK_PATH_MAX ];
        path_parent( parent, sizeof( parent ), dst_full );
        if ( parent[ 0 ] )
            sys_dir_make( parent );

        if ( cook_file( j->src_full, dst_full, ASSET_TOOL_DEFAULT_FONT_SIZE ) )
        {
            ++cooked;
            j->present = true;
        }
        else
        {
            ++failed; /* leave present=false so it is retried next run (not cached) */
        }
    }

    /* 4. Write the refreshed cache and the manifest of cooked outputs. */
    sbuf_t cache = { 0 };
    sbuf_t man   = { 0 };
    sbuf_add( &man, "# asset_tool manifest -- cooked outputs relative to this directory\n" );
    for ( int i = 0; i < ctx.job_count; ++i )
    {
        cook_job_t* j = &s_jobs[ i ];
        if ( !j->present )
            continue; /* failed cook: do not record, so it stays stale */

        char line[ COOK_PATH_MAX + 32 ];
        snprintf( line, sizeof( line ), "%llu %s\n", ( unsigned long long )j->src_mtime, j->src_rel );
        sbuf_add( &cache, line );

        snprintf( line, sizeof( line ), "%s\n", j->dst_rel );
        sbuf_add( &man, line );
    }

    char path[ COOK_PATH_MAX ];
    snprintf( path, sizeof( path ), "%s\\%s", dst_root, COOK_CACHE_FILE );
    sys_file_write_entire( path, cache.p ? cache.p : "", ( u32 )cache.len );
    snprintf( path, sizeof( path ), "%s\\%s", dst_root, COOK_MANIFEST_FILE );
    sys_file_write_entire( path, man.p ? man.p : "", ( u32 )man.len );
    free( cache.p );
    free( man.p );

    i64 ms = sys_tick_milliseconds() - start;
    printf( "asset_tool: tree cook %s -> %s: %d cooked, %d up-to-date, %d failed (%d ms)\n", src_root,
            dst_root, cooked, fresh, failed, ( int )ms );
    return failed ? 1 : 0;
}

/*============================================================================================*/
/*  Manifest cook (Cook-E): the build's resource manifests -> the cooked mirror               */
/*============================================================================================*/

/* build_tool's harvest (res_tool) writes obj/<target>/<target>_res_manifest.txt: '#' comment
   lines, then one resolved name per line as whitespace-separated columns -- the name, the
   source file under its content root, and for a subtree leaf "in <subtree>".  This mode takes
   any number of those manifests, resolves each source against the content roots in priority
   order (the first root holding the file wins), and cooks every name whose source kind has a
   cooked form into <out>/<name>.<cooked ext>: a stage-tagged .hlsl becomes an .oshd, a .recipe
   becomes the file its "kind" line names.  Loose content -- images, text -- is not touched; the
   runtime reads it from content/ as it is.  A name that several manifests share is one job.

   Staleness is decided per output, here and nowhere else, make-style: an output is stale when
   it is missing or when any input is at least as new as it.  The inputs are
     - the source;
     - the files the cook reads beside the source (the .hlsli siblings of a shader, since dxc
       resolves #include relative to the including file; the family.txt beside a recipe with
       no face line of its own);
     - the newest source of each tool that produces the bytes: this tool, which picks the
       sub-tool and its arguments, and the cooker the kind dispatches to.  build_tool names
       each with -tool <name>=<path>, where <path> is the newest of the tool's units and
       recorded headers.  A path, not a time: the two programs read mtimes on different
       clocks.  A tool not named contributes nothing.  A format-version header is one of a
       cooker's recorded headers, so a bump recooks that kind with no help from here; a
       relink because a library the cooker links was rebuilt touches none of its sources and
       recooks nothing.

   -check reports instead of cooking and exits 3 when anything needs a cook.  Both modes end
   with one fixed-format line a caller can parse:

       asset_tool: content <check|cook>: total=N fresh=N stale=N missing=N cooked=N failed=N

   where stale counts every output that needs a cook and missing the subset with no output at
   all.  A cook that fails deletes its output, so it is missing on the next run rather than a
   stale file that looks fresh. */

#define MAN_MAX_ROOTS    8
#define MAN_MAX_TOOLS    8

typedef struct man_job_s
{
    char       name[ COOK_PATH_MAX ];   // resource name: path under a root, no extension
    char       src[ COOK_PATH_MAX ];    // resolved source file
    char       dst[ COOK_PATH_MAX ];    // <out>/<name>.<cooked ext>
    res_kind_t kind;                    // kind of the COOKED file (a font recipe -> RES_KIND_FONT)
    bool       stale;                   // needs a cook this run
    bool       missing;                 // no cooked file exists at all
    char       why[ 96 ];               // stale reason for the report; "" when fresh
} man_job_t;

typedef struct man_tool_s
{
    const char* name;                   // build target name: asset_tool, shader_tool, font_tool
    const char* path;                   // the tool's newest source, from -tool
} man_tool_t;

static man_job_t s_man_jobs[ COOK_MAX_JOBS ];   /* BSS, like s_jobs */

/* Every name a manifest row has named this run, cookable or not, so that a name shared by
   several manifests is resolved, parsed and reported once.  Same bound as the jobs. */
static char s_man_seen[ COOK_MAX_JOBS ][ COOK_PATH_MAX ];

typedef struct man_ctx_s
{
    const char* roots[ MAN_MAX_ROOTS ];
    int         root_count;
    const char* out_root;               // trailing separator stripped
    int         job_count;
    int         seen_count;             // names classified so far (s_man_seen)
    int         bad_rows;               // names that could not be classified (a malformed recipe)
    bool        overflow;
    const man_tool_t* tools;            // the cook chain's newest sources, from -tool
    int         tool_count;
} man_ctx_t;

/* The cooker a kind dispatches to, by build target name; NULL for kinds this mode never cooks.
   The names match the exes cook_shader() and cook_font() spawn. */
static const char*
man_kind_tool( res_kind_t kind )
{
    switch ( kind )
    {
        case RES_KIND_SHADER: return "shader_tool";
        case RES_KIND_FONT:   return "font_tool";
        default:              return NULL;
    }
}

static const char*
man_basename( const char* path )
{
    const char* s = strrchr( path, '/' );
    const char* b = strrchr( path, '\\' );
    if ( b > s )
        s = b;
    return s ? s + 1 : path;
}

/* The newest among the tools that produce a job's bytes: this tool, the cooker its kind
   dispatches to (each as the -tool path build_tool named), and for a shader the dxc.exe that
   shader_tool runs, at the path its dxc_locate() resolves -- an SDK update changes every
   .oshd and no source of ours.  0 when nothing could be timed.  *out_file is the path that
   won; for dxc it points at static storage that the next call overwrites. */
static u64
man_tool_time( const man_ctx_t* c, const man_job_t* j, const char** out_file )
{
    u64 t     = 0;
    *out_file = NULL;
    for ( int i = 0; i < c->tool_count; ++i )
    {
        const char* name = c->tools[ i ].name;
        if ( strcmp( name, "asset_tool" ) != 0 && strcmp( name, man_kind_tool( j->kind ) ) != 0 )
            continue;
        u64 m = sys_file_time( c->tools[ i ].path );
        if ( m > t )
        {
            t         = m;
            *out_file = c->tools[ i ].path;
        }
    }

    if ( j->kind == RES_KIND_SHADER )
    {
        static char dxc[ COOK_PATH_MAX ];
        const char* sdk = getenv( "VULKAN_SDK" );
        if ( sdk && *sdk )
        {
            snprintf( dxc, sizeof( dxc ), "%s\\Bin\\dxc.exe", sdk );
            u64 m = sys_file_time( dxc );
            if ( m > t )
            {
                t         = m;
                *out_file = dxc;
            }
        }
    }
    return t;
}

/* sys_file_glob callback: fold a sibling file's mtime into the newest-input time. */
static bool
man_newest_cb( const char* filename, const char* full_path, void* ud )
{
    ( void )filename;
    u64 m = sys_file_time( full_path );
    if ( m > *( u64* )ud )
        *( u64* )ud = m;
    return true;
}

/* The newest mtime among a job's inputs: the source plus the files its cook reads beside it. */
static u64
man_input_time( const man_job_t* j )
{
    u64  t = sys_file_time( j->src );
    char dir[ COOK_PATH_MAX ];
    path_parent( dir, sizeof( dir ), j->src );
    if ( !dir[ 0 ] )
        return t;

    if ( ext_is( path_ext( j->src ), "hlsl" ) )
        sys_file_glob( dir, "*.hlsli", man_newest_cb, &t );
    else if ( ext_is( path_ext( j->src ), RES_RECIPE_EXT ) )
    {
        char fam[ COOK_PATH_MAX ];
        snprintf( fam, sizeof( fam ), "%s/family.txt", dir );
        man_newest_cb( "family.txt", fam, &t );
    }
    return t;
}

/* One manifest row.  A name is classified once per run no matter how many manifests carry it
   (an image's manifest is the union of its libraries', so most names arrive several times).
   Rows whose source does not resolve are res_tool's error, not ours, and rows whose kind has
   no cooked form are loose content: both are skipped without a word. */
static void
man_add_row( man_ctx_t* c, const char* name, const char* rel )
{
    for ( int i = 0; i < c->seen_count; ++i )
        if ( strcmp( s_man_seen[ i ], name ) == 0 )
            return;
    if ( c->seen_count >= COOK_MAX_JOBS )
    {
        c->overflow = true;
        return;
    }
    snprintf( s_man_seen[ c->seen_count++ ], COOK_PATH_MAX, "%s", name );

    char src[ COOK_PATH_MAX ] = "";
    for ( int r = 0; r < c->root_count && !src[ 0 ]; ++r )
    {
        char cand[ COOK_PATH_MAX ];
        snprintf( cand, sizeof( cand ), "%s/%s", c->roots[ r ], rel );
        if ( sys_file_exists( cand ) )
            snprintf( src, sizeof( src ), "%s", cand );
    }
    if ( !src[ 0 ] )
        return;

    res_kind_t kind = res_kind_from_ext( path_ext( rel ) );
    if ( kind == RES_KIND_RECIPE )
    {
        recipe_t r;
        if ( !recipe_parse( src, &r ) )    /* already reported */
        {
            ++c->bad_rows;
            return;
        }
        kind = r.kind;
    }
    if ( !man_kind_tool( kind ) )
        return;

    if ( c->job_count >= COOK_MAX_JOBS )
    {
        c->overflow = true;
        return;
    }

    man_job_t* j = &s_man_jobs[ c->job_count++ ];
    memset( j, 0, sizeof( *j ) );
    snprintf( j->name, sizeof( j->name ), "%s", name );
    snprintf( j->src, sizeof( j->src ), "%s", src );
    snprintf( j->dst, sizeof( j->dst ), "%s/%s.%s", c->out_root, name, res_kind_cooked_ext( kind ) );
    j->kind = kind;
}

/* Feed every entry row of one manifest to man_add_row. */
static bool
man_read_manifest( man_ctx_t* c, const char* path )
{
    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
    {
        fprintf( stderr, "asset_tool: error: could not read manifest %s\n", path );
        return false;
    }

    char* p = ( char* )fd.data;
    while ( *p )
    {
        char* line = p;
        while ( *p && *p != '\n' )
            ++p;
        if ( *p )
            *p++ = '\0';

        while ( *line == ' ' || *line == '\t' )
            ++line;
        if ( !*line || *line == '#' || *line == '\r' )
            continue;

        char* name = line;
        char* rel  = name;
        while ( *rel && *rel != ' ' && *rel != '\t' && *rel != '\r' )
            ++rel;
        if ( *rel )
            *rel++ = '\0';
        while ( *rel == ' ' || *rel == '\t' )
            ++rel;
        char* end = rel;
        while ( *end && *end != ' ' && *end != '\t' && *end != '\r' )
            ++end;
        *end = '\0';

        size_t nl = strlen( name );
        if ( !*rel || nl == 0 || name[ nl - 1 ] == '/' )
            continue;    /* a subtree row; its leaves follow as rows of their own */
        man_add_row( c, name, rel );
    }
    sys_file_free( &fd );
    return true;
}

/* Each non-blank, non-comment line of the list file is a manifest path. */
static bool
man_read_list( man_ctx_t* c, const char* list_path )
{
    sys_file_data_t fd = sys_file_read_entire( list_path );
    if ( !fd.ok )
    {
        fprintf( stderr, "asset_tool: error: could not read manifest list %s\n", list_path );
        return false;
    }

    bool  ok = true;
    char* p  = ( char* )fd.data;
    while ( *p )
    {
        char* line = p;
        while ( *p && *p != '\n' )
            ++p;
        if ( *p )
            *p++ = '\0';
        for ( char* e = line + strlen( line ); e > line && ( e[ -1 ] == '\r' || e[ -1 ] == ' ' ); --e )
            e[ -1 ] = '\0';
        while ( *line == ' ' || *line == '\t' )
            ++line;
        if ( !*line || *line == '#' )
            continue;
        if ( !man_read_manifest( c, line ) )
            ok = false;
    }
    sys_file_free( &fd );
    return ok;
}

typedef struct man_args_s
{
    const char* list_path;                    // -list <file>, or NULL
    const char* manifests[ 64 ];              // -manifest <file>, repeatable
    int         manifest_count;
    const char* roots[ MAN_MAX_ROOTS ];       // -root <dir>, highest priority first
    int         root_count;
    const char* out;                          // -out <dir>
    man_tool_t  tools[ MAN_MAX_TOOLS ];       // -tool <name>=<path>, repeatable
    int         tool_count;
    bool        check;                        // -check: report, cook nothing
    bool        force;                        // -f: every job is stale
    bool        silent;                       // -silent: no per-name stale/cook lines (converters still report)
} man_args_t;

/* cook_manifests -- the -list/-manifest mode.  Returns the process exit code: 0 when every
   output is fresh (or was cooked), 3 under -check when something needs a cook, 1 on any error. */
static int
cook_manifests( const man_args_t* a )
{
    i64 start = sys_tick_milliseconds();

    char out_root[ COOK_PATH_MAX ];
    snprintf( out_root, sizeof( out_root ), "%s", a->out );
    for ( int i = ( int )strlen( out_root ) - 1; i > 0 && ( out_root[ i ] == '\\' || out_root[ i ] == '/' ); --i )
        out_root[ i ] = '\0';

    man_ctx_t c  = { 0 };
    c.out_root   = out_root;
    c.tools      = a->tools;
    c.tool_count = a->tool_count;
    for ( int r = 0; r < a->root_count; ++r )
        c.roots[ c.root_count++ ] = a->roots[ r ];

    /* 1. Gather the jobs from every manifest. */
    bool inputs_ok = true;
    if ( a->list_path && !man_read_list( &c, a->list_path ) )
        inputs_ok = false;
    for ( int i = 0; i < a->manifest_count; ++i )
        if ( !man_read_manifest( &c, a->manifests[ i ] ) )
            inputs_ok = false;
    if ( c.overflow )
        fprintf( stderr, "asset_tool: warning: more than %d resource names -- truncated\n", COOK_MAX_JOBS );

    /* 2. Decide staleness per output. */
    int stale = 0, missing = 0;
    for ( int i = 0; i < c.job_count; ++i )
    {
        man_job_t*  j         = &s_man_jobs[ i ];
        u64         dst_t     = sys_file_time( j->dst );
        const char* tool_file = NULL;

        j->missing = ( dst_t == 0 );
        if ( a->force )
            snprintf( j->why, sizeof( j->why ), "forced" );
        else if ( j->missing )
            snprintf( j->why, sizeof( j->why ), "missing" );
        else if ( man_input_time( j ) >= dst_t )
            snprintf( j->why, sizeof( j->why ), "source newer" );
        else if ( man_tool_time( &c, j, &tool_file ) >= dst_t )
            snprintf( j->why, sizeof( j->why ), "tool newer: %s", man_basename( tool_file ) );
        j->stale = ( j->why[ 0 ] != '\0' );
        stale   += j->stale;
        missing += j->missing;
    }

    /* 3. Report, or cook. */
    int cooked = 0, failed = c.bad_rows;
    if ( a->check )
    {
        if ( !a->silent )
            for ( int i = 0; i < c.job_count; ++i )
                if ( s_man_jobs[ i ].stale )
                    printf( "asset_tool:   stale %s (%s)\n", s_man_jobs[ i ].name, s_man_jobs[ i ].why );
    }
    else
    {
        if ( !sys_dir_make( out_root ) )
        {
            fprintf( stderr, "asset_tool: error: could not create %s\n", out_root );
            return 1;
        }
        for ( int i = 0; i < c.job_count; ++i )
        {
            man_job_t* j = &s_man_jobs[ i ];
            if ( !j->stale )
                continue;

            char parent[ COOK_PATH_MAX ];
            path_parent( parent, sizeof( parent ), j->dst );
            if ( parent[ 0 ] )
                sys_dir_make( parent );

            if ( !a->silent )
                printf( "asset_tool: cook %s (%s)\n", j->name, j->why );
            if ( cook_file( j->src, j->dst, ASSET_TOOL_DEFAULT_FONT_SIZE ) )
                ++cooked;
            else
            {
                ++failed;
                sys_file_delete( j->dst );    /* no half-written or old-format file left behind */
            }
        }
    }

    i64 ms = sys_tick_milliseconds() - start;
    printf( "asset_tool: content %s: total=%d fresh=%d stale=%d missing=%d cooked=%d failed=%d (%d ms)\n",
            a->check ? "check" : "cook", c.job_count, c.job_count - stale, stale, missing, cooked, failed,
            ( int )ms );

    if ( !inputs_ok || failed )
        return 1;
    if ( a->check && stale )
        return 3;
    return 0;
}

/*============================================================================================*/
/*  Packaging (Cook-D): bundle a cooked tree into a .zip core/fs can mount                    */
/*============================================================================================*/

/* cook_pack -- read the cook_manifest.txt in <dir> and bundle every listed cooked output into
   <out_zip>, using the manifest rel-path as the in-archive name.  core/fs mounts a ".zip" as a
   bundle, and a loose mount at higher priority still shadows it -- so a shipped pack can be
   overridden file-by-file for local iteration.  The archive is built in a heap block (pack
   never touches the disk) and written whole through sys, matching how the engine reads zips. */
static int
cook_pack( const char* dir_arg, const char* out_zip )
{
    i64 start = sys_tick_milliseconds();

    /* Strip trailing separators from the cooked-tree root. */
    char dir[ COOK_PATH_MAX ];
    snprintf( dir, sizeof( dir ), "%s", dir_arg );
    for ( int i = ( int )strlen( dir ) - 1; i > 0 && ( dir[ i ] == '\\' || dir[ i ] == '/' ); --i )
        dir[ i ] = '\0';

    /* The manifest is the entry list: one cooked output rel-path per line, '#' comments. */
    char man_path[ COOK_PATH_MAX ];
    snprintf( man_path, sizeof( man_path ), "%s\\%s", dir, COOK_MANIFEST_FILE );
    sys_file_data_t man = sys_file_read_entire( man_path );
    if ( !man.ok )
    {
        fprintf( stderr, "asset_tool: error: no %s in %s -- run a tree cook first\n", COOK_MANIFEST_FILE,
                 dir );
        return 1;
    }

    pack_zip_writer_t* zw = pack_zip_writer_begin();
    if ( !zw )
    {
        fprintf( stderr, "asset_tool: error: could not init zip writer\n" );
        sys_file_free( &man );
        return 1;
    }

    int   packed = 0, failed = 0;
    char* p = ( char* )man.data; /* NUL-terminated by sys_file_read_entire */
    while ( *p )
    {
        while ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' )
            ++p;
        if ( !*p )
            break;
        if ( *p == '#' ) /* comment line */
        {
            while ( *p && *p != '\n' )
                ++p;
            continue;
        }

        char rel[ COOK_PATH_MAX ];
        int  k = 0;
        while ( *p && *p != '\n' && *p != '\r' && k < COOK_PATH_MAX - 1 )
            rel[ k++ ] = *p++;
        while ( k > 0 && ( rel[ k - 1 ] == ' ' || rel[ k - 1 ] == '\t' ) )
            --k; /* trim trailing blanks */
        rel[ k ] = '\0';
        if ( k == 0 )
            continue;

        /* Read the cooked file from disk (OS separators), archive it under the manifest name
           (forward slashes, which the manifest already stores -- zip central-dir convention). */
        char src[ COOK_PATH_MAX ];
        snprintf( src, sizeof( src ), "%s\\%s", dir, rel );
        path_norm( src, '\\' );

        sys_file_data_t fd = sys_file_read_entire( src );
        if ( !fd.ok )
        {
            fprintf( stderr, "asset_tool: error: could not read %s (in manifest)\n", src );
            ++failed;
            continue;
        }

        if ( pack_zip_writer_add( zw, rel, fd.data, fd.size, PACK_LEVEL_DEFAULT ) )
        {
            printf( "asset_tool:   pack %s (%u bytes)\n", rel, fd.size );
            ++packed;
        }
        else
        {
            fprintf( stderr, "asset_tool: error: could not add %s to archive\n", rel );
            ++failed;
        }
        sys_file_free( &fd );
    }
    sys_file_free( &man );

    void* buf = NULL;
    u32   sz  = 0;
    bool  ok  = pack_zip_writer_end( zw, &buf, &sz );
    if ( ok )
        ok = sys_file_write_entire( out_zip, buf, sz );
    free( buf );                 /* writer_end handed us ownership of the heap block */
    if ( !ok )
    {
        fprintf( stderr, "asset_tool: error: could not finalize/write %s\n", out_zip );
        return 1;
    }

    i64 ms = sys_tick_milliseconds() - start;
    printf( "asset_tool: pack %s -> %s: %d entries, %d failed, %u bytes (%d ms)\n", dir, out_zip, packed,
            failed, sz, ( int )ms );
    return failed ? 1 : 0;
}

/*============================================================================================*/

static int
usage( void )
{
    fprintf( stderr,
             "usage:\n"
             "  asset_tool cook <src> <dst> [args...]   single job\n"
             "      .ttf/.otf  -> font_tool  (args[0] = size_px, default %d)\n"
             "      image      -> .tex       (pre-decoded RGBA8)\n"
             "      .hlsl      -> shader_tool cook -> .oshd (stage tag names the profile:\n"
             "                    foo.vs.hlsl -> vs_6_0; also ps/cs/gs/hs/ds)\n"
             "      .recipe    -> what its 'kind' line says (font: face/size/sdf/range -> font_tool)\n"
             "      other      -> copy\n"
             "  asset_tool -src <dir> -dst <dir> [-f]   incremental tree cook (-f = force all)\n"
             "  asset_tool pack <cooked_dir> <out.zip>  bundle a cooked tree (via its manifest)\n"
             "  asset_tool -list <manifests.txt> -root <dir>... -out <dir> [-tool <name>=<path>]...\n"
             "             [-check] [-f] [-silent]\n"
             "      cook the shaders and recipes the build's resource manifests name into\n"
             "      <out>/<name>.<ext>; only what is missing or older than its inputs.\n"
             "      -manifest <file> names one manifest directly (repeatable, with or without -list)\n"
             "      -tool <name>=<path>  the newest source of a tool in the cook chain (asset_tool,\n"
             "                 shader_tool, font_tool); an edit to the tool then counts as an input\n"
             "      -check     report instead of cooking; exit 3 when anything needs a cook\n"
             "      -f         cook every name;  -silent  omit the per-name stale/cook lines\n",
             ASSET_TOOL_DEFAULT_FONT_SIZE );
    return 1;
}

int
main( int argc, char** argv )
{
    sys_tick_init();

    int rc = 1;

    if ( argc >= 2 && strcmp( argv[ 1 ], "cook" ) == 0 )
    {
        /* Single-job mode (Cook-A): cook <src> <dst> [args...] */
        if ( argc < 4 )
            usage();
        else
            rc = cook_single( argv[ 2 ], argv[ 3 ], argv + 4, argc - 4 ) ? 0 : 1;
    }
    else if ( argc >= 2 && strcmp( argv[ 1 ], "pack" ) == 0 )
    {
        /* Packaging mode (Cook-D): pack <cooked_dir> <out.zip> */
        if ( argc < 4 )
            usage();
        else
            rc = cook_pack( argv[ 2 ], argv[ 3 ] );
    }
    else
    {
        /* Flag modes: tree cook (Cook-B) -src <dir> -dst <dir> [-f], or manifest cook (Cook-E)
           -list/-manifest ... -root ... -out <dir> [-check] [-f] [-silent].  Which one ran is
           decided by the flags present; the two sets do not mix. */
        const char* src   = NULL;
        const char* dst   = NULL;
        man_args_t  man   = { 0 };
        bool        bad   = false;

        for ( int i = 1; i < argc; ++i )
        {
            const char* f   = argv[ i ];
            bool        val = ( i + 1 < argc );
            if ( strcmp( f, "-src" ) == 0 && val )
                src = argv[ ++i ];
            else if ( strcmp( f, "-dst" ) == 0 && val )
                dst = argv[ ++i ];
            else if ( strcmp( f, "-f" ) == 0 )
                man.force = true;
            else if ( strcmp( f, "-list" ) == 0 && val )
                man.list_path = argv[ ++i ];
            else if ( strcmp( f, "-manifest" ) == 0 && val )
            {
                if ( man.manifest_count < ( int )( sizeof( man.manifests ) / sizeof( man.manifests[ 0 ] ) ) )
                    man.manifests[ man.manifest_count++ ] = argv[ ++i ];
                else
                {
                    fprintf( stderr, "asset_tool: error: too many -manifest arguments; use -list\n" );
                    bad = true;
                    ++i;
                }
            }
            else if ( strcmp( f, "-root" ) == 0 && val )
            {
                if ( man.root_count < MAN_MAX_ROOTS )
                    man.roots[ man.root_count++ ] = argv[ ++i ];
                else
                {
                    fprintf( stderr, "asset_tool: error: more than %d -root arguments\n", MAN_MAX_ROOTS );
                    bad = true;
                    ++i;
                }
            }
            else if ( strcmp( f, "-out" ) == 0 && val )
                man.out = argv[ ++i ];
            else if ( strcmp( f, "-tool" ) == 0 && val )
            {
                char* eq = strchr( argv[ ++i ], '=' );
                if ( !eq || eq == argv[ i ] || !eq[ 1 ] )
                {
                    fprintf( stderr, "asset_tool: error: -tool wants <name>=<path>, got '%s'\n", argv[ i ] );
                    bad = true;
                }
                else if ( man.tool_count < MAN_MAX_TOOLS )
                {
                    *eq                            = '\0';
                    man.tools[ man.tool_count ].name = argv[ i ];
                    man.tools[ man.tool_count ].path = eq + 1;
                    ++man.tool_count;
                }
                else
                {
                    fprintf( stderr, "asset_tool: error: more than %d -tool arguments\n", MAN_MAX_TOOLS );
                    bad = true;
                }
            }
            else if ( strcmp( f, "-check" ) == 0 )
                man.check = true;
            else if ( strcmp( f, "-silent" ) == 0 )
                man.silent = true;
            else
                bad = true;
        }

        bool tree_mode = ( src || dst );
        bool man_mode  = ( man.list_path || man.manifest_count || man.out || man.root_count || man.check ||
                           man.tool_count );

        if ( bad || ( tree_mode && man_mode ) )
            usage();
        else if ( tree_mode )
            rc = ( src && dst ) ? cook_tree( src, dst, man.force ) : usage();
        else if ( man_mode )
        {
            if ( !( man.list_path || man.manifest_count ) || !man.out || !man.root_count )
            {
                fprintf( stderr, "asset_tool: error: manifest cook needs -list or -manifest, -root and -out\n" );
                rc = usage();
            }
            else
                rc = cook_manifests( &man );
        }
        else
            rc = usage();
    }

    sys_tick_exit();
    return rc;
}

/*============================================================================================*/
