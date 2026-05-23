/*==============================================================================================

    reflect_tool_std.c - minimal string helpers for the reflect_tool tool

    Avoids any engine dependency; wraps only what the tool actually needs.

==============================================================================================*/

#include "reflect_tool_internal.h"

/*----------------------------------------------------------------------------------------------
    String helpers
----------------------------------------------------------------------------------------------*/

void
str_copy( char* dst, const char* src, int max )
{
    int i = 0;
    while ( i < max - 1 && src[ i ] )
    {
        dst[ i ] = src[ i ];
        i++;
    }
    dst[ i ] = '\0';
}

int
str_len( const char* s )
{
    int n = 0;
    while ( s[ n ] )
        n++;
    return n;
}

void
str_cat( char* dst, const char* src, int max )
{
    int n = str_len( dst );
    str_copy( dst + n, src, max - n );
}

int
str_ends_with( const char* s, const char* suffix )
{
    int sl = str_len( s );
    int xl = str_len( suffix );
    if ( xl > sl )
        return 0;
    return memcmp( s + sl - xl, suffix, (size_t)xl ) == 0;
}

/*--------------------------------------------------------------------------------------------*/