/*==============================================================================================

    engine/core/cmd/cmd_buffer.c

    Deferred command text buffer (Quake3 Cbuf model): producers append/insert raw text,
    the host loop pumps it once per frame.  Statements are extracted quote-aware, split
    on '\n' and ';', with '//' comments stripped -- so raw config file contents can be
    queued directly.  Compiled inside the core unity build (core_cvar.c) before cmd.c.

==============================================================================================*/

static char s_cmd_text[ CMD_BUF_CAP ];    // pending command text
static u32  s_cmd_len  = 0;               // bytes pending
static u32  s_cmd_wait = 0;               // frames to defer before pumping resumes

#define CMD_EXEC_MAX_PER_PUMP 16          // guards a self-referential "exec" from looping all frame
static u32 s_exec_calls = 0;              // "exec" invocations this pump cycle; reset in cmd_pump

/* Called by cmd_exec before it opens a file.  There's no true call-stack recursion here
   (exec queues text and returns; the requeued statement fires on a later pump iteration), so
   this bounds repeated exec-of-exec within one frame instead of a depth. */

static bool
cmd_exec_budget_take( void )
{
    if ( s_exec_calls >= CMD_EXEC_MAX_PER_PUMP )
        return false;
    s_exec_calls++;
    return true;
}

/*============================================================================================*/
/* Append text to the end of the buffer; a '\n' terminator is added if missing. */

void
cmd_queue( const char* text )
{
    if ( !text || !text[ 0 ] )
        return;

    const u32 len = ( u32 )strlen( text );
    if ( s_cmd_len + len + 1 > CMD_BUF_CAP )
    {
        con_printf( "cmd: buffer overflow, text discarded\n" );
        return;
    }

    memcpy( s_cmd_text + s_cmd_len, text, len );
    s_cmd_len += len;
    if ( text[ len - 1 ] != '\n' )
        s_cmd_text[ s_cmd_len++ ] = '\n';
}

/*============================================================================================*/
/* Insert text ahead of pending contents: an "exec" runs its file before the rest of the
   line that triggered it. */

void
cmd_queue_front( const char* text )
{
    if ( !text || !text[ 0 ] )
        return;

    u32 len  = ( u32 )strlen( text );
    u32 term = ( text[ len - 1 ] != '\n' ) ? 1 : 0;

    if ( s_cmd_len + len + term > CMD_BUF_CAP )
    {
        con_printf( "cmd: buffer overflow, text discarded\n" );
        return;
    }

    memmove( s_cmd_text + len + term, s_cmd_text, s_cmd_len );
    memcpy( s_cmd_text, text, len );
    if ( term )
        s_cmd_text[ len ] = '\n';
    s_cmd_len += len + term;
}

/*============================================================================================*/
/* Translate command-line arguments into buffered text: each "+command arg..." group becomes
   one queued statement ("+set r_width 1920" -> "set r_width 1920").  Tokens before the
   first '+' belong to the platform/launcher and are ignored.  Args containing spaces are
   re-quoted so they survive tokenization. */

void
cmd_queue_args( int argc, char** argv )
{
    if ( !argv )
        return;

    char line[ CMD_LINE_LEN ];
    u32  len = 0;

    for ( int i = 1; i <= argc; ++i )
    {
        /* Flush the pending statement at the next '+' group or end of argv. */
        if ( ( i == argc || argv[ i ][ 0 ] == '+' ) && len > 0 )
        {
            line[ len ] = '\0';
            cmd_queue( line );
            len = 0;
        }

        if ( i == argc )
            break;

        const char* a = argv[ i ];
        if ( a[ 0 ] == '+' )
            a++;                 // starts a new statement, '+' stripped
        else if ( len == 0 )
            continue;            // before the first '+': not ours

        /* Quote if the token has a space, or a '"'/'\\' that would otherwise confuse
           cmd_tokenize (e.g. a leading '"' would be misread as starting a quoted token). */
        const bool quote = ( strpbrk( a, " \"\\" ) != NULL );
        const u32  alen  = ( u32 )strlen( a );

        /* Reserve worst case: every byte could need a backslash escape, plus separator/quotes. */
        const u32 need = ( quote ? alen * 2 : alen ) + ( len ? 1u : 0u ) + ( quote ? 2u : 0u );
        if ( len + need >= CMD_LINE_LEN )
            continue;            // overlong statement: drop the token

        if ( len )
            line[ len++ ] = ' ';
        if ( quote )
            line[ len++ ] = '"';

        /* Escape '"' and '\\' the same way cmd_write_quoted does, so the token round-trips
           through cmd_tokenize exactly. */
        for ( const char* p = a; *p; ++p )
        {
            if ( quote && ( *p == '"' || *p == '\\' ) )
                line[ len++ ] = '\\';
            line[ len++ ] = *p;
        }

        if ( quote )
            line[ len++ ] = '"';
    }
}

/*============================================================================================*/
/* Extract one statement from the front of the buffer into line[] (quote-aware, split on
   '\n' and ';', '//' comment runs to end of line).  Returns false when the buffer is empty. */

static bool
cmd_buffer_extract( char* line, u32 line_size )
{
    if ( s_cmd_len == 0 )
        return false;

    u32  stmt_end = s_cmd_len;    // statement text ends here (exclusive)
    u32  consumed = s_cmd_len;    // bytes removed from the buffer
    bool in_quote = false;

    for ( u32 i = 0; i < s_cmd_len; ++i )
    {
        const char c = s_cmd_text[ i ];

        if ( c == '"' )
        {
            in_quote = !in_quote;
        }
        else if ( c == '\n' || ( !in_quote && c == ';' ) )
        {
            stmt_end = i;
            consumed = i + 1;
            break;
        }
        else if ( !in_quote && c == '/' && i + 1 < s_cmd_len && s_cmd_text[ i + 1 ] == '/' )
        {
            /* Comment: statement ends here; consume through the end of the line. */
            stmt_end = i;
            consumed = s_cmd_len;
            for ( u32 j = i; j < s_cmd_len; ++j )
            {
                if ( s_cmd_text[ j ] == '\n' )
                {
                    consumed = j + 1;
                    break;
                }
            }
            break;
        }
    }

    u32 copy = stmt_end;
    if ( copy >= line_size )
    {
        con_printf( "cmd: statement exceeds %u bytes, truncated\n", line_size - 1 );
        copy = line_size - 1;    // overlong statements truncate
    }

    memcpy( line, s_cmd_text, copy );
    line[ copy ] = '\0';

    s_cmd_len -= consumed;
    memmove( s_cmd_text, s_cmd_text + consumed, s_cmd_len );
    return true;
}

/*============================================================================================*/
/* Execute pending statements.  Called once per frame by the host loop.  "wait" suspends
   the pump for N frames; the budget stops runaway self-queueing scripts (remaining text
   stays pending for the next frame). */

void
cmd_pump( void )
{
    if ( s_cmd_wait > 0 )
    {
        s_cmd_wait--;
        return;
    }

    s_exec_calls = 0;    // fresh exec allowance for this pump cycle

    char line[ CMD_LINE_LEN ];

    for ( u32 budget = CMD_PUMP_BUDGET; budget > 0; --budget )
    {
        if ( !cmd_buffer_extract( line, sizeof( line ) ) )
            return;

        cmd_execute_string( line );

        if ( s_cmd_wait > 0 )
            return;    // statement was "wait": stop, resume next frame
    }

    con_printf( "cmd: pump budget exhausted, %u bytes deferred to next frame\n", s_cmd_len );
}

/*==============================================================================================
    Built-in: wait
==============================================================================================*/

/* Suspend buffered command execution for N frames (default 1). */

static void
cmd_cmd_wait( int argc, char** argv )
{
    if ( argc > 1 )
    {
        int frames = atoi( argv[ 1 ] );
        s_cmd_wait = ( frames > 0 ) ? ( u32 )frames : 1;
    }
    else
    {
        s_cmd_wait = 1;
    }
}

/*==============================================================================================
    Lifetime (called by cmd_system_init/exit)
==============================================================================================*/

static void
cmd_buffer_init( void )
{
    s_cmd_len    = 0;
    s_cmd_wait   = 0;
    s_exec_calls = 0;
    cmd_register( "wait", cmd_cmd_wait, "Suspend buffered commands for N frames" );
}

static void
cmd_buffer_exit( void )
{
    s_cmd_len  = 0;
    s_cmd_wait = 0;
}

/*============================================================================================*/
