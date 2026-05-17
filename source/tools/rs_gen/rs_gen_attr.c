/*==============================================================================================

    rs_gen_attr.c - attribute argument parser for RS_STRUCT / RS_PROP annotation args

    Grammar (comma-separated entries):

        entry  := IDENT [ '=' value ]
        value  := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL | IDENT

    Continuation: for `name=v1, v2, v3` the v2, v3 are emitted as repeated entries under
    the same `name`, matching rs_attrib multi-value runs (e.g. `range=0, 100`).

    All functions are static; compiled only as part of the rs_gen unity build.

==============================================================================================*/

#include "rs_gen_internal.h"

/*----------------------------------------------------------------------------------------------
    Literal classification
----------------------------------------------------------------------------------------------*/

/* Classify a trimmed, NUL-terminated token as int, float, or string literal (or tag). */
static int
classify_literal( const char* s )
{
    if ( s[ 0 ] == '"' )
        return RG_ATTR_STRING;

    int has_digit = 0;
    int has_dot   = 0;
    int i         = ( s[ 0 ] == '-' || s[ 0 ] == '+' ) ? 1 : 0;
    for ( ; s[ i ]; i++ )
    {
        char c = s[ i ];
        if ( c == '.' )
            has_dot = 1;
        else if ( is_digit( c ) )
            has_digit = 1;
        else if ( c == 'f' || c == 'F' )
            has_dot = 1;
        else
            return RG_ATTR_TAG;
    }
    if ( !has_digit )
        return RG_ATTR_TAG;
    return has_dot ? RG_ATTR_FLOAT : RG_ATTR_INT;
}

/*----------------------------------------------------------------------------------------------
    String helpers
----------------------------------------------------------------------------------------------*/

/* Strip surrounding double-quotes from a string literal (leaves escapes raw). */
static void
unquote_string( char* s )
{
    int n = rg_str_len( s );
    if ( n >= 2 && s[ 0 ] == '"' && s[ n - 1 ] == '"' )
    {
        memmove( s, s + 1, (size_t)( n - 2 ) );
        s[ n - 2 ] = '\0';
    }
}

/* Trim leading and trailing whitespace in-place. */
static void
trim( char* s )
{
    int n = rg_str_len( s );
    while ( n > 0 && is_space( (unsigned char)s[ n - 1 ] ) )
        s[ --n ] = '\0';
    int i = 0;
    while ( s[ i ] && is_space( (unsigned char)s[ i ] ) )
        i++;
    if ( i > 0 )
        memmove( s, s + i, (size_t)( rg_str_len( s + i ) + 1 ) );
}

/*----------------------------------------------------------------------------------------------
    Attribute argument parser
----------------------------------------------------------------------------------------------*/

static void
parse_attr_args( const char* args, rg_attr_t* out, int max, int* out_count )
{
    *out_count = 0;
    if ( !args || !*args )
        return;

    char current_name[ RG_MAX_NAME ] = { 0 };
    int  in_value                    = 0;
    char token[ RG_MAX_NAME ]        = { 0 };
    int  tn                          = 0;
    int  depth                       = 0;

    const char* p = args;
    while ( 1 )
    {
        char c = *p;

        if ( ( c == ',' || c == '\0' ) && depth == 0 )
        {
            token[ tn ] = '\0';
            trim( token );
            if ( token[ 0 ] )
            {
                if ( !in_value )
                {
                    /* bare tag attribute with no '=' */
                    if ( *out_count < max )
                    {
                        rg_attr_t* a = &out[ ( *out_count )++ ];
                        rg_str_copy( a->name, token, RG_MAX_NAME );
                        a->kind       = RG_ATTR_TAG;
                        a->value[ 0 ] = '\0';
                        rg_str_copy( current_name, token, RG_MAX_NAME );
                    }
                }
                else
                {
                    if ( *out_count < max )
                    {
                        rg_attr_t* a = &out[ ( *out_count )++ ];
                        rg_str_copy( a->name, current_name, RG_MAX_NAME );
                        a->kind = classify_literal( token );
                        rg_str_copy( a->value, token, RG_MAX_NAME );
                        if ( a->kind == RG_ATTR_STRING )
                            unquote_string( a->value );
                    }
                }
            }
            tn         = 0;
            token[ 0 ] = '\0';
            if ( c == '\0' )
                break;
            p++;
            /* keep in_value across commas: `range=0, 100` repeats name for each value */
            continue;
        }

        if ( c == '=' && depth == 0 && !in_value )
        {
            token[ tn ] = '\0';
            trim( token );
            rg_str_copy( current_name, token, RG_MAX_NAME );
            in_value   = 1;
            tn         = 0;
            token[ 0 ] = '\0';
            p++;
            continue;
        }

        if ( c == '(' || c == '[' || c == '{' )
            depth++;
        else if ( c == ')' || c == ']' || c == '}' )
            depth--;

        if ( c == '"' )
        {
            /* consume quoted string verbatim into token */
            if ( tn < RG_MAX_NAME - 1 )
                token[ tn++ ] = c;
            p++;
            while ( *p && *p != '"' )
            {
                if ( *p == '\\' && p[ 1 ] )
                {
                    if ( tn < RG_MAX_NAME - 1 )
                        token[ tn++ ] = *p;
                    p++;
                }
                if ( tn < RG_MAX_NAME - 1 )
                    token[ tn++ ] = *p;
                p++;
            }
            if ( *p == '"' )
            {
                if ( tn < RG_MAX_NAME - 1 )
                    token[ tn++ ] = *p;
                p++;
            }
            continue;
        }

        if ( tn < RG_MAX_NAME - 1 )
            token[ tn++ ] = c;
        p++;
    }
}
