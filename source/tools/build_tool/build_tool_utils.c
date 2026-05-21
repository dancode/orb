/*==============================================================================================

    build_tool_utils.c -- Common helper utilities for the build orchestrator.

==============================================================================================*/
#include "build_tool.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>

/*============================================================================================*/
// --- Command Buffer Management ---

/**
 * cmd_append()
 * 
 * Appends a formatted string to a command buffer. 
 * High-performance alternative to repeated strcat/sprintf calls.
 */
void
cmd_append( cmd_buf_t* b, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( b->buf + b->size, CMD_BUF_MAX - b->size, fmt, args );
    va_end( args );
    if ( written > 0 ) b->size += (size_t)written;
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
