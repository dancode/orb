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
    Whitespace and comment skipper
----------------------------------------------------------------------------------------------*/

/**
 * Advances the cursor past all whitespace and C/C++ style comments.
 * Ensures the parser only sees relevant code tokens.
 */
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

/*----------------------------------------------------------------------------------------------
    Token readers
----------------------------------------------------------------------------------------------*/

/**
 * Checks if a specific word (keyword) exists at the current cursor position.
 * It ensures the word is not just a substring of a larger identifier.
 */
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

/**
 * Extracts a full C identifier (like a variable or struct name) into 'out'.
 */
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

/**
 * Reads the content inside a set of parentheses: ( ... )
 * Correctly handles nested parentheses and string literals to find the true closing paren.
 */
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

/**
 * Skips an entire { ... } block.
 * Handles nested braces, comments, and strings to ensure it finds the correct matching brace.
 */
static const char*
skip_brace_block( const char* p )
{
    if ( *p != '{' )
        return p;
    p++;
    int depth = 1;
    while ( *p && depth > 0 )
    {
        /* Skip line comments. */
        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            while ( *p && *p != '\n' ) p++;
            continue;
        }

        /* Skip block comments. */
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p += 2;
            while ( *p && !( p[ 0 ] == '*' && p[ 1 ] == '/' ) ) p++;
            if ( *p )
                p += 2;
            continue;
        }

        /* Skip strings and character literals so braces inside them don't affect depth. */
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

        /* Track nesting depth. */
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

/**
 * Attempts to read a typedef name after a closing brace.
 * Example: `} MyType;` -> extracts "MyType".
 * It also handles attributes like `} __attribute__((...)) MyType;`
 */
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
    rg_str_copy( out, sep ? sep + 8 : tmp, max );
}

/*--------------------------------------------------------------------------------------------*/