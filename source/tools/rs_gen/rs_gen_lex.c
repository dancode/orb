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

/* Returns true if character is whitespace (space, tab, newline, etc.). */
static int
is_space( int c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* Returns true if character is a digit 0-9. */
static int
is_digit( int c )
{
    return c >= '0' && c <= '9';
}

/* Returns true if character can start a C identifier (A-Z, a-z, _). */
static int
is_ident_start( int c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}

/* Returns true if character can be part of a C identifier (A-Z, a-z, _, 0-9). */
static int
is_ident_char( int c )
{
    return is_ident_start( c ) || is_digit( c );
}

/*----------------------------------------------------------------------------------------------
    String trimmer
----------------------------------------------------------------------------------------------*/

static void
trim( char* s )
{
    int n = rg_str_len( s );
    while ( n > 0 && is_space( ( unsigned char )s[ n - 1 ] ) ) s[ --n ] = '\0';
    int i = 0;
    while ( s[ i ] && is_space( ( unsigned char )s[ i ] ) ) i++;
    if ( i > 0 )
        memmove( s, s + i, ( size_t )( rg_str_len( s + i ) + 1 ) );
}

/*----------------------------------------------------------------------------------------------
    Whitespace and comment skipper
----------------------------------------------------------------------------------------------*/

static const char*
skip_ws( const char* p )
{
    for ( ;; )
    {
        /* Skip standard whitespace. */
        while ( *p && is_space( ( unsigned char )*p ) ) p++;

        /* Skip line comments: // ... */
        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            p += 2;
            while ( *p && *p != '\n' ) p++;
            continue;
        }

        /* Skip block comments: / * ... * / */
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p += 2;
            while ( *p && !( p[ 0 ] == '*' && p[ 1 ] == '/' ) ) p++;
            if ( *p )
                p += 2;
            continue;
        }

        /* No more whitespace or comments found. */
        break;
    }
    return p;
}

/* Strip C and C++ comments from a mutable buffer in-place before parsing.
   Line comments (//) become spaces up to (not including) the newline.
   Block comments become spaces with embedded newlines preserved.
   String and character literal contents are passed through unchanged. */
static void
strip_comments( char* p )
{
    while ( *p )
    {
        /* String literal: skip to closing quote so // inside "..." is ignored. */
        if ( *p == '"' )
        {
            p++;
            while ( *p && *p != '"' )
            {
                if ( *p == '\\' && p[ 1 ] ) p++;
                p++;
            }
            if ( *p ) p++;
            continue;
        }

        /* Character literal: same reason. */
        if ( *p == '\'' )
        {
            p++;
            while ( *p && *p != '\'' )
            {
                if ( *p == '\\' && p[ 1 ] ) p++;
                p++;
            }
            if ( *p ) p++;
            continue;
        }

        /* Line comment: blank everything up to (not including) the newline. */
        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            while ( *p && *p != '\n' ) *p++ = ' ';
            continue;
        }

        /* Block comment: blank everything, preserving embedded newlines. */
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p[ 0 ] = p[ 1 ] = ' ';
            p += 2;
            while ( *p )
            {
                if ( p[ 0 ] == '*' && p[ 1 ] == '/' )
                {
                    p[ 0 ] = p[ 1 ] = ' ';
                    p += 2;
                    break;
                }
                if ( *p != '\n' ) *p = ' ';
                p++;
            }
            continue;
        }

        p++;
    }
}

/*----------------------------------------------------------------------------------------------
    Token readers
----------------------------------------------------------------------------------------------*/

/* True if kw appears at p as a whole word (not preceded or followed by an ident char). */
static int
match_word( const char* p, const char* buf_start, const char* kw )
{
    int kl = rg_str_len( kw );

    /* Check previous character to ensure we aren't in the middle of a word. */
    if ( p > buf_start && is_ident_char( ( unsigned char )p[ -1 ] ) )
        return 0;

    /* Check if the keyword matches. */
    if ( strncmp( p, kw, ( size_t )kl ) != 0 )
        return 0;

    /* Check following character to ensure the word ends here. */
    if ( is_ident_char( ( unsigned char )p[ kl ] ) )
        return 0;

    return 1;
}

/*--------------------------------------------------------------------------------------------*/

static const char*
read_ident( const char* p, char* out, int max )
{
    int n = 0;
    /* Must start with a valid identifier character. */
    if ( !is_ident_start( ( unsigned char )*p ) )
    {
        if ( max > 0 )
            out[ 0 ] = '\0';
        return p;
    }

    /* Accumulate characters until a non-identifier character is hit. */
    while ( is_ident_char( ( unsigned char )*p ) && n < max - 1 ) out[ n++ ] = *p++;
    out[ n ] = '\0';

    /* Drain any remaining characters if the identifier exceeds max buffer. */
    while ( is_ident_char( ( unsigned char )*p ) )
        p++;

    return p;
}

/*--------------------------------------------------------------------------------------------*/

/* Reads the content inside ( ... ), handling nesting and string literals. */
static const char*
read_paren_block( const char* p, char* out, int max )
{
    if ( *p != '(' )
    {
        if ( max > 0 )
            out[ 0 ] = '\0';
        return p;
    }

    p++; /* Skip the opening '(' */
    int depth = 1;
    int n     = 0;

    while ( *p && depth > 0 )
    {
        char c = *p;

        /* Skip strings verbatim so parens inside " ( " don't count towards depth. */
        if ( c == '"' )
        {
            if ( n < max - 1 )
                out[ n++ ] = c;
            p++;
            while ( *p && *p != '"' )
            {
                /* Handle escaped quotes: \" */
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

        /* Track nesting depth. */
        if ( c == '(' )
            depth++;
        else if ( c == ')' )
        {
            depth--;
            if ( depth == 0 )
            {
                p++; /* Done; cursor moved past closing ')' */
                break;
            }
        }

        /* Copy character into output buffer if space permits. */
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

/* Advances past a matching { ... } block, respecting string literals and nested braces. */
static const char*
skip_brace_block( const char* p )
{
    if ( *p != '{' )
        return p;
    p++;
    int depth = 1;
    while ( *p && depth > 0 )
    {
        if ( *p == '"' || *p == '\'' )
        {
            char q = *p++;
            while ( *p && *p != q )
            {
                if ( *p == '\\' && p[ 1 ] ) p++;
                p++;
            }
            if ( *p ) p++;
            continue;
        }
        if ( *p == '{' ) depth++;
        else if ( *p == '}' ) depth--;
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

/* Reads the typedef name after a closing '}', skipping any __attribute__((...)) tokens. */
static int
read_typedef_name( const char* close_brace, char* out, int max )
{
    const char* p = close_brace;
    if ( *p == '}' )
        p++;
    p = skip_ws( p );

    /* Look for an identifier. */
    while ( is_ident_start( ( unsigned char )*p ) )
    {
        char tmp[ RG_MAX_NAME ];
        p             = read_ident( p, tmp, RG_MAX_NAME );
        const char* q = skip_ws( p );

        /* If followed by '(', it's likely a macro/attribute (e.g. __attribute__), so skip it. */
        if ( *q == '(' )
        {
            char dummy[ 4 ];
            p = skip_ws( read_paren_block( q, dummy, sizeof dummy ) );
            continue;
        }

        /* Found the actual type name. */
        rg_str_copy( out, tmp, max );
        return 1;
    }
    out[ 0 ] = '\0';
    return 0;
}

// clang-format off
/*----------------------------------------------------------------------------------------------
    Path helper
----------------------------------------------------------------------------------------------*/
/**
 * Derives a relative include path for the generated code.
 * Transforms an absolute path like 'C:/dev/orb/source/engine/sys.h' into 'engine/sys.h'.
 */
static void
make_include_path( const char* abs_path, char* out, int max )
{
    char tmp[ RG_MAX_PATH ] = { 0 };
    int  n = 0;

    /* normalize backslashes to forward slashes, and copy up to max. */
    for ( int i = 0; abs_path[ i ] && n < RG_MAX_PATH - 1; i++ )
    {
        tmp[ n++ ] = ( abs_path[ i ] == '\\' ) ? '/' : abs_path[ i ];
    }
    tmp[ n ] = '\0';

    const char* sep = strstr( tmp, "/source/" );
    if ( sep )
    {
        rg_str_copy( out, sep + 8, max );
    }
    else if ( strncmp( tmp, "source/", 7 ) == 0 )
    {
        rg_str_copy( out, tmp + 7, max );
    }
    else
    {
        rg_str_copy( out, tmp, max );
    }
}

/*--------------------------------------------------------------------------------------------*/