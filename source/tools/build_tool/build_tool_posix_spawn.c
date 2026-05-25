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

#if defined( _WIN32 )
    #error "build_tool_posix_spawn.c is only for POSIX builds"
#endif

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

// clang-format on
/*============================================================================================*/
