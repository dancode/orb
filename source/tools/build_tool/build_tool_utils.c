/*==============================================================================================

    build_tool_utils.c -- Common helper utilities for the build orchestrator.

==============================================================================================*/
#include "build_tool.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

/*============================================================================================*/
// --- Command Buffer Management ---

/**
 * cmd_append()
 *
 * Appends a formatted string to a command buffer.
 *
 * vsnprintf() returns the length it WOULD have written, not the truncated
 * length. The old implementation added that return value to b->size directly,
 * which silently pushed b->size past CMD_BUF_MAX. The next append would then
 * compute (CMD_BUF_MAX - b->size) as a huge size_t underflow and write
 * arbitrarily far past the buffer.
 *
 * This version clamps to the remaining space and surfaces truncation via
 * the `truncated` flag so callers can spill to a response file.
 */
void
cmd_append( cmd_buf_t* b, const char* fmt, ... )
{
    if ( b->size >= CMD_BUF_MAX ) { b->truncated = true; return; }

    size_t remaining = CMD_BUF_MAX - b->size;

    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( b->buf + b->size, remaining, fmt, args );
    va_end( args );

    if ( written < 0 ) { b->truncated = true; return; }

    if ( ( size_t )written >= remaining )
    {
        // Did not fit. Mark truncated and pin size at the null-terminator.
        b->size                  = CMD_BUF_MAX - 1;
        b->buf[ CMD_BUF_MAX - 1 ] = '\0';
        b->truncated             = true;
    }
    else
    {
        b->size += ( size_t )written;
    }
}

/**
 * cmd_spill_to_response_file()
 *
 * cmd.exe caps argument strings near 8191 chars, and CreateProcessW caps at
 * 32767. As targets gain dependencies the link line in particular grows
 * without bound. When the buffer crosses CMD_RSP_THRESHOLD (or has been
 * marked truncated), spill the argument tail to a response file so the
 * actual invocation stays short.
 *
 * cl/link/lib all consume "@file" arguments where the file contains the
 * remaining tokens separated by whitespace. Quoting and backslashes pass
 * through literally — no escaping needed for our paths.
 */
bool
cmd_spill_to_response_file( cmd_buf_t* b, const char* rsp_path )
{
    if ( !b->truncated && b->size < CMD_RSP_THRESHOLD ) return false;

    // Find the end of the first token (the tool exe).
    const char* args = b->buf;
    while ( *args && *args != ' ' && *args != '\t' ) ++args;
    if ( !*args ) return false;

    size_t exe_len = ( size_t )( args - b->buf );
    if ( exe_len == 0 || exe_len >= 64 ) return false;

    char exe[ 64 ];
    memcpy( exe, b->buf, exe_len );
    exe[ exe_len ] = '\0';

    // Skip whitespace between exe and first arg.
    while ( *args == ' ' || *args == '\t' ) ++args;

    FILE* f = fopen( rsp_path, "w" );
    if ( !f )
    {
        printf( "Warning: could not open response file %s\n", rsp_path );
        return false;
    }
    fputs( args, f );
    fclose( f );

    // Rewrite the buffer to the short "@file" form. Clear truncated so the
    // (now short) buffer is treated as valid by downstream callers.
    b->size      = 0;
    b->truncated = false;
    cmd_append( b, "%s @%s", exe, rsp_path );
    return true;
}

/*============================================================================================*/
// --- File System Helpers ---

/**
 * build_get_mtime()
 * 
 * Returns the last modification time of a file. Returns 0 if the file doesn't exist.
 * This is the foundation of our incremental build system.
 */
__time64_t
build_get_mtime( const char* path )
{
    struct __stat64 s;
    if ( _stat64( path, &s ) == 0 ) return s.st_mtime;
    return 0;
}
