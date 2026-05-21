/*==============================================================================================

    build_tool_cc.c -- Compiler and Linker command generation.

==============================================================================================*/
#include "build_tool.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <io.h>
#include <time.h>

/*============================================================================================*/
// --- Internal Helpers ---

static void
get_target_upper( const char* name, char* out )
{
    strcpy( out, name );
    for ( char* p = out; *p; ++p ) *p = ( char )toupper( *p );
}

// Sweep bin/<name>_*.pdb. Files locked by an attached debugger fail silently —
// exactly what we want: the running debug session keeps its PDB, and any
// unlocked leftovers from previous rebuilds get garbage-collected here.
static void
cleanup_stale_pdbs( const char* target_name )
{
    char pattern[ 256 ];
    sprintf( pattern, "bin/%s_*.pdb", target_name );

    struct _finddata_t fd;
    intptr_t h = _findfirst( pattern, &fd );
    if ( h == -1 ) return;
    do
    {
        char path[ 256 ];
        sprintf( path, "bin/%s", fd.name );
        remove( path );
    }
    while ( _findnext( h, &fd ) == 0 );
    _findclose( h );
}

/*============================================================================================*/
// --- Compilation ---

/**
 * build_target_compile()
 * 
 * Generates and executes the cl.exe command line for a target.
 */
bool
build_target_compile( build_context_t* ctx, target_info_t* target, const char* obj_dir, const char* gen_dir )
{
    cmd_buf_t cmd = { 0 };
    const char* cc = ctx->is_clang ? "clang-cl.exe" : "cl.exe";

    // Base flags. /showIncludes emits a "Note: including file:" line for every
    // header pulled in, which we capture into a per-target dep file so the
    // incremental rebuild logic can detect header edits (essential for unity
    // builds where the listed .units are just umbrella TUs).
    cmd_append( &cmd, "%s /c /nologo /W4 /WX /Zc:preprocessor /std:c11 /showIncludes ", cc );
    
    // Include paths and output directories.
    cmd_append( &cmd, "/I source /I %s /Fo%s/ /Fd%s/ ", gen_dir, obj_dir, obj_dir );

    // Architectural Defines
    cmd_append( &cmd, "/DOS_WINDOWS /DCOMPILER_MSVC /DARCH_X64 /D_CRT_SECURE_NO_WARNINGS " );

    // Target-specific static define (e.g. /DBASE_STATIC).
    char target_upper[ 128 ];
    get_target_upper( target->name, target_upper );
    cmd_append( &cmd, "/D%s_STATIC ", target_upper );

    if ( ctx->is_monolithic )
        cmd_append( &cmd, "/DBUILD_STATIC " );

    // Config-specific flags.
    if ( ctx->config == CONFIG_DEBUG )
        cmd_append( &cmd, "/Zi /Od /MDd /D_DEBUG " );
    else
        cmd_append( &cmd, "/O2 /MD /DNDEBUG " );

    // Add translation units.
    for ( int i = 0; i < target->unit_count; ++i )
        cmd_append( &cmd, "%s/%s ", target->root_dir, target->units[ i ] );

    // Add generated reflection code.
    if ( target->has_reflect )
    {
        cmd_append( &cmd, "%s/%s.generated.c ", gen_dir, target->reflect_name );
    }

    // Write the captured header list to <obj_dir>/_deps.txt for the next
    // incremental check to read. Coarse-grained (one file per target) which
    // matches the unity-build pattern: any header change forces a target
    // recompile, but headers in OTHER targets don't trigger spurious rebuilds.
    char deps_path[ 256 ];
    snprintf( deps_path, sizeof( deps_path ), "%s/_deps.txt", obj_dir );

    return build_run_cmd_capture_deps( cmd.buf, deps_path ) == 0;
}

/*============================================================================================*/
// --- Linking / Archiving ---

/**
 * build_target_link()
 * 
 * Generates and executes the link.exe or lib.exe command line for a target.
 */
bool
build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir )
{
    ( void )ctx;
    cmd_buf_t cmd = { 0 };

    if ( target->type == TARGET_STATIC_LIB )
    {
        cmd_append( &cmd, "lib.exe /nologo /OUT:bin/%s.lib %s/*.obj", target->name, obj_dir );
    }
    else
    {
        const char* linker = "link.exe";
        cmd_append( &cmd, "%s /nologo ", linker );
        
        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            cmd_append( &cmd, "/DLL " );
            cmd_append( &cmd, "/IMPLIB:bin/%s.lib ", target->name );
        }

        cmd_append( &cmd, "/OUT:bin/%s%s %s/*.obj ", target->name,
                    (target->type == TARGET_EXECUTABLE) ? ".exe" : ".dll", obj_dir );

        // Rotate the PDB filename every link so an attached debugger never
        // collides with the linker over the previous build's symbol file.
        // Stale unlocked PDBs are removed first; locked ones (held by a live
        // debug session) survive the sweep and remain valid for that session.
        cleanup_stale_pdbs( target->name );
        cmd_append( &cmd, "/DEBUG /PDB:bin/%s_%lld.pdb ",
                    target->name, ( long long )time( NULL ) );

        // Link against dependencies.
        for ( int i = 0; i < target->dep_count; ++i )
        {
            cmd_append( &cmd, "bin/%s.lib ", target->deps[ i ] );
        }
        
        // System libraries.
        cmd_append( &cmd, "user32.lib shell32.lib gdi32.lib advapi32.lib " );
    }

    return build_run_cmd( cmd.buf ) == 0;
}

/*============================================================================================*/