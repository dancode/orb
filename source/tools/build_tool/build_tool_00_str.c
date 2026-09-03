/*==============================================================================================

    build_tool_00_str.c -- ASCII string helpers.

    Case folding is fixed at ASCII a-z/A-Z: no locale, no ctype.h, no CRT string routines.
    Everything the build tool compares -- target names, file extensions, flag spellings,
    toolchain paths -- is ASCII by construction, and a locale-sensitive fold would make
    those comparisons depend on the environment the build ran in.

    Included FIRST in the unity chain, ahead of the platform layer, because the platform
    files need these too. Nothing here may call platform_* or touch a file: those helpers
    live in 01_prim.c, which is included after the platform layer for that reason.

==============================================================================================*/
// clang-format off

/* Copy src into buf uppercased; ASCII only, no ctype.h dependency. */

static void
str_upper( const char* src, char* buf, size_t buf_size )
{
    size_t i = 0;
    for ( ; src[ i ] && i < buf_size - 1; ++i )
    {
        char c   = src[ i ];
        buf[ i ] = ( c >= 'a' && c <= 'z' ) ? ( char )( c - 32 ) : c;
    }
    buf[ i ] = '\0';
}

/* ASCII case-insensitive strcmp / strncmp; no locale, no CRT dependency. */

static int
str_icmp( const char* a, const char* b )
{
    for ( ;; )
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if ( ca >= 'A' && ca <= 'Z' ) ca = (unsigned char)( ca + 32 );
        if ( cb >= 'A' && cb <= 'Z' ) cb = (unsigned char)( cb + 32 );
        if ( ca != cb ) return (int)ca - (int)cb;
        if ( ca == '\0' ) return 0;
    }
}

static int
str_nicmp( const char* a, const char* b, size_t n )
{
    for ( ; n; --n )
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if ( ca >= 'A' && ca <= 'Z' ) ca = (unsigned char)( ca + 32 );
        if ( cb >= 'A' && cb <= 'Z' ) cb = (unsigned char)( cb + 32 );
        if ( ca != cb ) return (int)ca - (int)cb;
        if ( ca == '\0' ) return 0;
    }
    return 0;
}

/* ASCII case-insensitive strstr. Returns the first occurrence of needle in hay, or NULL.
   An empty needle matches at hay. */

static const char*
str_istr( const char* hay, const char* needle )
{
    for ( ; *hay; ++hay )
    {
        const char* h = hay;
        const char* n = needle;
        for ( ;; )
        {
            if ( !*n ) return hay;
            unsigned char ch = (unsigned char)*h++;
            unsigned char cn = (unsigned char)*n++;
            if ( ch >= 'A' && ch <= 'Z' ) ch = (unsigned char)( ch + 32 );
            if ( cn >= 'A' && cn <= 'Z' ) cn = (unsigned char)( cn + 32 );
            if ( ch != cn ) break;
        }
    }
    return *needle ? NULL : hay;
}

// clang-format on
/*============================================================================================*/
