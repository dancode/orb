/*==============================================================================================

    reflect_tool_attr.c - attribute argument parser for RS_STRUCT / RS_PROP annotation args

    Grammar (comma-separated entries):

        entry  := IDENT [ '=' value ]
        value  := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL | IDENT

    Continuation: for `name=v1, v2, v3` the v2, v3 are emitted as repeated entries under
    the same `name`, matching rs_attrib multi-value runs (e.g. `range=0, 100`).

    All functions are static; compiled only as part of the reflect_tool unity build.

==============================================================================================*/

#include "reflect_tool_internal.h"

/*----------------------------------------------------------------------------------------------
    Literal classification
----------------------------------------------------------------------------------------------*/

static int
classify_literal( const char* s )
{
    /* Starts with double-quote? It's a string. */
    if ( s[ 0 ] == '"' )
        return RT_ATTR_STRING;

    int has_digit = 0;
    int has_dot   = 0;

    /* Skip optional sign for numeric checks. */
    int i = ( s[ 0 ] == '-' || s[ 0 ] == '+' ) ? 1 : 0;

    /* Scan string for numeric characteristics. */
    for ( ; s[ i ]; i++ )
    {
        char c = s[ i ];
        if ( c == '.' )
            has_dot = 1;
        else if ( is_digit( c ) )
            has_digit = 1;
        else if ( c == 'f' || c == 'F' )
            has_dot = 1; /* Treat 'f' suffix (e.g. 1.0f) as a float indicator. */
        else
            return RT_ATTR_TAG; /* Found non-numeric character; treat as identifier/tag. */
    }

    /* If it had no digits, it's not a number (e.g. just a dot or sign). */
    if ( !has_digit )
        return RT_ATTR_TAG;

    /* Distinction based on presence of decimal or 'f' suffix. */
    return has_dot ? RT_ATTR_FLOAT : RT_ATTR_INT;
}

/*----------------------------------------------------------------------------------------------
    String helpers
----------------------------------------------------------------------------------------------*/

static void
unquote_string( char* s )
{
    int n = str_len( s );
    if ( n >= 2 && s[ 0 ] == '"' && s[ n - 1 ] == '"' )
    {
        memmove( s, s + 1, ( size_t )( n - 2 ) );
        s[ n - 2 ] = '\0';
    }
}

// clang-format off
/*----------------------------------------------------------------------------------------------
    Attribute argument parser

    For strings like: `name = "MyStruct", flags = 1, range = 0, 100`

    This function processes the text inside the parentheses of macros like RS_PROP(...).
    It breaks the string into a list of attr_t structs. It handles:

        1. key = value pairs    (e.g. flags = 1).
        2. bare tags            (e.g. hidden).
        3. multi-value keys     (e.g. range = 0, 100),
----------------------------------------------------------------------------------------------*/

static void
parse_attr_args( const char* args, attr_t* out, int max, int* out_count )
{
    *out_count = 0;
    if ( !args || !*args )
        return;

    char current_name[ RT_MAX_NAME ] = { 0 };   // Keeps track of the key for multi-value args.
    int  in_value = 0;                          // 0 if parsing key/tag, 1 if parsing value.
    char token[ RT_MAX_NAME ] = { 0 };          // Accumulates characters for the current fragment.
    int  token_length = 0;                      // token length
    int  depth  = 0;                            // Tracks nesting level of brackets/parens.

    const char* p = args;
    while ( 1 )
    {
        char c = *p;

        /* If we hit a comma (at root depth) or the end of string, we've finished a token. */
        if ( ( c == ',' || c == '\0' ) && depth == 0 )
        {
            token[ token_length ] = '\0';
            trim( token );

            if ( token[ 0 ] )
            {
                if ( !in_value )
                {
                    /* No '=' seen yet; this is a "Tag" attribute (e.g. `read_only`). */
                    if ( *out_count < max )
                    {
                        attr_t* a = &out[ ( *out_count )++ ];
                        str_copy( a->name, token, RT_MAX_NAME );
                        a->kind       = RT_ATTR_TAG;
                        a->value[ 0 ] = '\0';
                        str_copy( current_name, token, RT_MAX_NAME );
                    }
                }
                else
                {
                    /* We are after an '='; this token is the value. */
                    if ( *out_count < max )
                    {
                        attr_t* a = &out[ ( *out_count )++ ];
                        /* Reuse the name found before the '='. */
                        str_copy( a->name, current_name, RT_MAX_NAME );
                        a->kind = classify_literal( token );
                        str_copy( a->value, token, RT_MAX_NAME );
                        if ( a->kind == RT_ATTR_STRING )
                            unquote_string( a->value );
                    }
                }
            }

            /* Reset for the next entry. */
            token_length = 0;
            token[ 0 ] = '\0';
            if ( c == '\0' )
                break;
            p++;

            /* Note: We DO NOT reset in_value here.
               This allows `range=0, 100` to work, as `100` will be treated as another value for `range`. */
            continue;
        }

        /* Detect transition from key to value. */
        if ( c == '=' && depth == 0 && !in_value )
        {
            token[ token_length ] = '\0';
            trim( token );
            /* The token we just finished is the name of the attribute. */
            str_copy( current_name, token, RT_MAX_NAME );
            in_value   = 1;
            token_length = 0;
            token[ 0 ] = '\0';
            p++;
            continue;
        }

        /* Track nesting to avoid splitting commas inside nested structures. */
        if ( c == '(' || c == '[' || c == '{' )
            depth++;
        else if ( c == ')' || c == ']' || c == '}' )
            depth--;

        /* Special handling for quoted strings to ensure commas inside quotes are ignored. */
        if ( c == '"' )
        {
            if ( token_length < RT_MAX_NAME - 1 )
                token[ token_length++ ] = c;
            p++;
            while ( *p && *p != '"' )
            {
                /* Handle escaped quotes: \" */
                if ( *p == '\\' && p[ 1 ] )
                {
                    if ( token_length < RT_MAX_NAME - 1 )
                        token[ token_length++ ] = *p;
                    p++;
                }
                if ( token_length < RT_MAX_NAME - 1 )
                    token[ token_length++ ] = *p;
                p++;
            }
            if ( *p == '"' )
            {
                if ( token_length < RT_MAX_NAME - 1 )
                    token[ token_length++ ] = *p;
                p++;
            }
            continue;
        }

        /* Normal character accumulation. */
        if ( token_length < RT_MAX_NAME - 1 )
            token[ token_length++ ] = c;
        p++;
    }
}

/*--------------------------------------------------------------------------------------------*/
