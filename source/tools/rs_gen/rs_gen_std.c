/*==============================================================================================

    rs_gen_std.c - minimal string helpers for the rs_gen tool

    Avoids any engine dependency; wraps only what the tool actually needs.

==============================================================================================*/

#include "rs_gen_internal.h"

/*----------------------------------------------------------------------------------------------
    String helpers
----------------------------------------------------------------------------------------------*/

void
rg_str_copy( char* dst, const char* src, int max )
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
rg_str_len( const char* s )
{
    int n = 0;
    while ( s[ n ] )
        n++;
    return n;
}

void
rg_str_cat( char* dst, const char* src, int max )
{
    int n = rg_str_len( dst );
    rg_str_copy( dst + n, src, max - n );
}

int
rg_str_ends_with( const char* s, const char* suffix )
{
    int sl = rg_str_len( s );
    int xl = rg_str_len( suffix );
    if ( xl > sl )
        return 0;
    return memcmp( s + sl - xl, suffix, (size_t)xl ) == 0;
}

/*--------------------------------------------------------------------------------------------*/