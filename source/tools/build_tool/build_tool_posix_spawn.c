#if !defined( _WIN32 )
/*==============================================================================================

    build_tool_posix_spawn.c -- POSIX process-spawning platform layer for the ORB build tool.

    Both functions use posix_spawn rather than system() / popen / fork+exec.
    posix_spawn is safe under concurrent worker threads: it does not duplicate the
    parent's file descriptors as a side effect. fork() in a multi-threaded process
    triggers undefined behavior with threads that hold locks; posix_spawn avoids
    this by handing child stdio setup to the kernel via file-actions rather than
    performing it in a post-fork child context. The Win32 counterpart is
    build_tool_win_spawn.c; both files expose identical symbols.

    Functions implemented:
        platform_spawn()         -- run cmd, redirect output to log file or inherit console
        platform_spawn_capture() -- run cmd, deliver each output line to a callback

==============================================================================================*/
// clang-format off

#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

extern char** environ;

/*==============================================================================================
    --- platform_spawn ---

    Runs cmd under "/bin/sh -c" and waits for it to finish.

    If log_path is non-NULL, both stdout and stderr are redirected to that file
    (opened with O_APPEND so concurrent workers never truncate each other's output).
    If log_path is NULL, the child inherits the parent's console handles.

    Returns the child's exit code, or -1 if the process could not be created.
==============================================================================================*/

static int
platform_spawn( const char* cmd, const char* log_path )
{
    int log_fd = -1;
    if ( log_path )
    {
        log_fd = open( log_path, O_WRONLY | O_APPEND | O_CREAT, 0644 );
        if ( log_fd < 0 )
        {
            printf( ORB_INDENT "[orb error] could not open log file %s\n", log_path );
            return -1;
        }
    }

    const char* argv[] = { "/bin/sh", "-c", cmd, NULL };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init( &actions );
    if ( log_fd >= 0 )
    {
        posix_spawn_file_actions_adddup2( &actions, log_fd, STDOUT_FILENO );
        posix_spawn_file_actions_adddup2( &actions, log_fd, STDERR_FILENO );
    }

    pid_t pid;
    int rc = posix_spawn( &pid, "/bin/sh", &actions, NULL, ( char* const* )argv, environ );
    posix_spawn_file_actions_destroy( &actions );
    if ( log_fd >= 0 ) close( log_fd );

    if ( rc != 0 )
    {
        printf( ORB_INDENT "[orb error] posix_spawn failed (rc %d): %s\n", rc, cmd );
        return -1;
    }

    int status = 0;
    waitpid( pid, &status, 0 );
    return WIFEXITED( status ) ? WEXITSTATUS( status ) : 1;
}

/*==============================================================================================
    --- platform_spawn_capture ---

    Runs cmd under "/bin/sh -c", captures both stdout and stderr via an anonymous
    pipe, and calls fn( line, userdata ) once per complete output line.

    Lines are null-terminated with no trailing newline. CRLF is normalized to LF
    before line assembly (for any cross-platform output). Lines longer than the
    internal 4096-byte buffer are flushed mid-line -- the content is not lost; fn
    just receives it in two consecutive calls without a newline break.

    Returns the child's exit code, or -1 if the process could not be created.
==============================================================================================*/

static int
platform_spawn_capture( const char* cmd, platform_line_fn_t fn, void* userdata )
{
    int pipefd[ 2 ];
    if ( pipe( pipefd ) < 0 ) return -1;

    const char* argv[] = { "/bin/sh", "-c", cmd, NULL };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init( &actions );
    // Close the read end in the child and route both output streams to the write end.
    posix_spawn_file_actions_addclose( &actions, pipefd[ 0 ] );
    posix_spawn_file_actions_adddup2(  &actions, pipefd[ 1 ], STDOUT_FILENO );
    posix_spawn_file_actions_adddup2(  &actions, pipefd[ 1 ], STDERR_FILENO );
    posix_spawn_file_actions_addclose( &actions, pipefd[ 1 ] );

    pid_t pid;
    int rc = posix_spawn( &pid, "/bin/sh", &actions, NULL, ( char* const* )argv, environ );
    posix_spawn_file_actions_destroy( &actions );
    // Close the parent's write end so read() returns EOF when the child exits.
    close( pipefd[ 1 ] );

    if ( rc != 0 )
    {
        close( pipefd[ 0 ] );
        return -1;
    }

    // Stream pipe bytes into a line buffer; fire fn() for each complete line.
    char    line[ 4096 ];
    char    buf [ 4096 ];
    size_t  line_len   = 0;
    ssize_t bytes_read = 0;

    while ( ( bytes_read = read( pipefd[ 0 ], buf, sizeof( buf ) ) ) > 0 )
    {
        for ( ssize_t i = 0; i < bytes_read; ++i )
        {
            char c = buf[ i ];
            if ( c == '\r' ) continue;      // normalize CRLF -> LF

            if ( c == '\n' || line_len >= sizeof( line ) - 1 )
            {
                line[ line_len ] = '\0';
                fn( line, userdata );
                line_len = 0;

                // If flushed due to buffer overflow (not a true newline), keep the char.
                if ( c != '\n' ) line[ line_len++ ] = c;
            }
            else
            {
                line[ line_len++ ] = c;
            }
        }
    }

    // Flush any trailing partial line (child may exit without a final newline).
    if ( line_len > 0 )
    {
        line[ line_len ] = '\0';
        fn( line, userdata );
    }

    close( pipefd[ 0 ] );

    int status = 0;
    waitpid( pid, &status, 0 );
    return WIFEXITED( status ) ? WEXITSTATUS( status ) : 1;
}

/*==============================================================================================
    --- posix_parse_dep_file ---

    Reads a single Makefile .d file produced by GCC/Clang -MMD and writes each
    dependency path (one per line) to out. The .d format is:
        obj/target/unit.o: source/unit.c source/dep.h \
            source/other.h
    Everything before the first ':' is the make target (skipped). The backslash
    continuation token '\' is skipped; all other whitespace-separated tokens are
    written as dependency paths.
==============================================================================================*/

static void
posix_parse_dep_file( const char* dep_path, FILE* out )
{
    FILE* f = fopen( dep_path, "r" );
    if ( !f ) return;

    char   buf[ CMD_BUF_MAX ];
    size_t n = fread( buf, 1, sizeof( buf ) - 1, f );
    fclose( f );
    buf[ n ] = '\0';

    // Skip the make target (everything up to and including the first ':').
    char* p = strchr( buf, ':' );
    if ( !p ) return;
    ++p;

    // Each space/newline-separated token is a dependency path.
    // '\' alone is a line-continuation marker -- skip it.
    // strtok_r keeps state in a local saveptr so concurrent workers are safe.
    char* saveptr = NULL;
    char* tok = strtok_r( p, " \t\r\n", &saveptr );
    while ( tok )
    {
        if ( tok[ 0 ] != '\\' && tok[ 0 ] != '\0' )
            fprintf( out, "%s\n", tok );
        tok = strtok_r( NULL, " \t\r\n", &saveptr );
    }
}

/*==============================================================================================
    --- posix_collect_dep_files ---

    Scans obj_dir for all .d files emitted by -MMD and writes their header
    dependencies to includes_path (_includes.txt). Called after the per-unit
    compile loop in build_target_compile() to populate the file the next
    incremental header-change check reads.
==============================================================================================*/

static void
posix_collect_dep_files( const char* obj_dir, const char* includes_path )
{
    FILE* out = fopen( includes_path, "w" );
    if ( !out ) return;

    char pattern[ PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "%s/*.d", obj_dir );

    platform_find_data_t fd;
    platform_find_t h = platform_find_first( pattern, &fd );
    if ( h != PLATFORM_FIND_INVALID )
    {
        do
        {
            if ( fd.is_dir ) continue;
            char dep_path[ PATH_MAX ];
            snprintf( dep_path, sizeof( dep_path ), "%s/%s", obj_dir, fd.name );
            posix_parse_dep_file( dep_path, out );
        }
        while ( platform_find_next( h, &fd ) );
        platform_find_close( h );
    }

    fclose( out );
}

// clang-format on
/*============================================================================================*/
#endif
