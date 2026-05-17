/*==============================================================================================

    rs_gen_lex.c - character predicates, tokenizer, and source navigation helpers

    All functions here are static; this file is compiled only as part of the rs_gen unity
    build (via rs_gen.c). Include order: lex -> attr -> parse, so statics defined here are
    visible to subsequent translation units in the same TU.

==============================================================================================*/

#include "rs_gen_internal.h"

/*----------------------------------------------------------------------------------------------
    Character predicates
----------------------------------------------------------------------------------------------*/

static int
is_space( int c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int
is_digit( int c )
{
    return c >= '0' && c <= '9';
}

static int
is_ident_start( int c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}

static int
is_ident_char( int c )
{
    return is_ident_start( c ) || is_digit( c );
}

/*----------------------------------------------------------------------------------------------
    Whitespace and comment skipper
----------------------------------------------------------------------------------------------*/

/* Advance past whitespace, // line comments, and / * block * / comments. */
static const char*
skip_ws( const char* p )
{
    for ( ;; )
    {
        while ( *p && is_space( (unsigned char)*p ) )
            p++;
        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            p += 2;
            while ( *p && *p != '\n' )
                p++;
            continue;
        }
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p += 2;
            while ( *p && !( p[ 0 ] == '*' && p[ 1 ] == '/' ) )
                p++;
            if ( *p )
                p += 2;
            continue;
        }
        break;
    }
    return p;
}

/*----------------------------------------------------------------------------------------------
    Token readers
----------------------------------------------------------------------------------------------*/

/* Match literal `kw` at *p with a non-identifier boundary on both sides. */
static int
match_word( const char* p, const char* buf_start, const char* kw )
{
    int kl = rg_str_len( kw );
    if ( p > buf_start && is_ident_char( (unsigned char)p[ -1 ] ) )
        return 0;
    if ( strncmp( p, kw, (size_t)kl ) != 0 )
        return 0;
    if ( is_ident_char( (unsigned char)p[ kl ] ) )
        return 0;
    return 1;
}

/* Read an identifier into out (NUL-terminated). Returns advanced cursor. */
static const char*
read_ident( const char* p, char* out, int max )
{
    int n = 0;
    if ( !is_ident_start( (unsigned char)*p ) )
    {
        if ( max > 0 )
            out[ 0 ] = '\0';
        return p;
    }
    while ( is_ident_char( (unsigned char)*p ) && n < max - 1 )
        out[ n++ ] = *p++;
    out[ n ] = '\0';
    while ( is_ident_char( (unsigned char)*p ) )   /* drain overflow */
        p++;
    return p;
}

/* Parse `(...)` starting at the opening paren. Writes the inner text into out (no parens).
   Returns position after the closing paren. Handles nested parens and string literals. */
static const char*
read_paren_block( const char* p, char* out, int max )
{
    if ( *p != '(' )
    {
        if ( max > 0 )
            out[ 0 ] = '\0';
        return p;
    }
    p++;
    int depth = 1;
    int n     = 0;
    while ( *p && depth > 0 )
    {
        char c = *p;
        if ( c == '"' )
        {
            if ( n < max - 1 )
                out[ n++ ] = c;
            p++;
            while ( *p && *p != '"' )
            {
                if ( *p == '\\' && p[ 1 ] )
                {
                    if ( n < max - 1 )
                        out[ n++ ] = *p;
                    p++;
                }
                if ( n < max - 1 )
                    out[ n++ ] = *p;
                p++;
            }
            if ( *p )
            {
                if ( n < max - 1 )
                    out[ n++ ] = *p;
                p++;
            }
            continue;
        }
        if ( c == '(' )
            depth++;
        else if ( c == ')' )
        {
            depth--;
            if ( depth == 0 )
            {
                p++;
                break;
            }
        }
        if ( n < max - 1 )
            out[ n++ ] = c;
        p++;
    }
    out[ n < max ? n : max - 1 ] = '\0';
    return p;
}

/*----------------------------------------------------------------------------------------------
    Brace block navigation
----------------------------------------------------------------------------------------------*/

/* Skip a balanced brace block starting at '{'. Returns position after the closing '}'. */
static const char*
skip_brace_block( const char* p )
{
    if ( *p != '{' )
        return p;
    p++;
    int depth = 1;
    while ( *p && depth > 0 )
    {
        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            while ( *p && *p != '\n' )
                p++;
            continue;
        }
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p += 2;
            while ( *p && !( p[ 0 ] == '*' && p[ 1 ] == '/' ) )
                p++;
            if ( *p )
                p += 2;
            continue;
        }
        if ( *p == '"' || *p == '\'' )
        {
            char q = *p++;
            while ( *p && *p != q )
            {
                if ( *p == '\\' && p[ 1 ] )
                    p++;
                p++;
            }
            if ( *p )
                p++;
            continue;
        }
        if ( *p == '{' )
            depth++;
        else if ( *p == '}' )
            depth--;
        p++;
    }
    return p;
}

/* Returns pointer TO the closing '}' (skip_brace_block returns position after it). */
static const char*
find_close_brace( const char* p )
{
    const char* end = skip_brace_block( p );
    return ( end > p ) ? end - 1 : p;
}

/*----------------------------------------------------------------------------------------------
    Struct / enum head parsing
----------------------------------------------------------------------------------------------*/

/* After `} TYPENAME;`, read the typedef name. p should point at the closing '}'.
   Skips __attribute__((...)) and similar identifier(...) pairs.
   Returns 1 on success, 0 if no name was found. */
static int
read_typedef_name( const char* close_brace, char* out, int max )
{
    const char* p = close_brace;
    if ( *p == '}' )
        p++;
    p = skip_ws( p );
    while ( is_ident_start( (unsigned char)*p ) )
    {
        char tmp[ RG_MAX_NAME ];
        p = read_ident( p, tmp, RG_MAX_NAME );
        const char* q = skip_ws( p );
        if ( *q == '(' )
        {
            char dummy[ 4 ];
            p = skip_ws( read_paren_block( q, dummy, sizeof dummy ) );
            continue;
        }
        rg_str_copy( out, tmp, max );
        return 1;
    }
    out[ 0 ] = '\0';
    return 0;
}

/*----------------------------------------------------------------------------------------------
    Path helper
----------------------------------------------------------------------------------------------*/

/* Derive a source-relative include path from an absolute file path.
   Strips everything up to and including the first "/source/" segment.
   Normalises backslashes to forward slashes. */
static void
make_include_path( const char* abs_path, char* out, int max )
{
    char tmp[ RG_MAX_PATH ];
    int  n = 0;
    for ( int i = 0; abs_path[ i ] && n < RG_MAX_PATH - 1; i++ )
        tmp[ n++ ] = ( abs_path[ i ] == '\\' ) ? '/' : abs_path[ i ];
    tmp[ n ] = '\0';

    const char* sep = strstr( tmp, "/source/" );
    rg_str_copy( out, sep ? sep + 8 : tmp, max );
}
