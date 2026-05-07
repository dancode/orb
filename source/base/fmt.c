/*==============================================================================================

    fmt.c -- Formatting implementation.

    No global state. No allocation.

==============================================================================================*/

i32
fmt_vbuf( char* buf, usize buf_cap, const char* fmt, va_list args )
{
    /* Write formatted text into buf. Returns bytes written (no null term). */

    if ( !buf || buf_cap == 0 )
        return 0;
    int written = vsnprintf( buf, buf_cap, fmt, args );
    if ( written < 0 )
    {
        buf[ 0 ] = '\0';
        return 0;
    }
    // Clamp to what actually fits
    if ( ( usize )written >= buf_cap )
        return ( i32 )( buf_cap - 1 );
    return written;
}

i32
fmt_buf( char* buf, usize buf_cap, const char* fmt, ... )
{
    /* Write formatted text into buf. Returns bytes written (no null term). */

    va_list args;
    va_start( args, fmt );
    i32 n = fmt_vbuf( buf, buf_cap, fmt, args );
    va_end( args );
    return n;
}

i32
fmt_append( char* buf, usize buf_cap, i32 offset, const char* fmt, ... )
{
    /*  Write formatted text after existing content in buf.
        Useful for building strings incrementally without tracking position yourself.

        char line[256];
        i32 pos = 0;
        pos += fmt_append(line, sizeof(line), pos, "Player: %s", name);
        pos += fmt_append(line, sizeof(line), pos, "  HP: %d", hp);
    */

    if ( !buf || buf_cap == 0 || ( usize )offset >= buf_cap )
        return 0;
    va_list args;
    va_start( args, fmt );
    i32 n = fmt_vbuf( buf + offset, buf_cap - ( usize )offset, fmt, args );
    va_end( args );
    return n;
}

/*============================================================================================*/

i32
fmt_u64( char* buf, usize buf_cap, u64 v )
{
    /* Write decimal representation of v into buf. Returns bytes written (no null term). */

    if ( !buf || buf_cap == 0 )
        return 0;
    // Reverse-write digits, then flip
    char tmp[ 24 ];
    i32  len = 0;
    if ( v == 0 )
    {
        tmp[ len++ ] = '0';
    }
    while ( v > 0 )
    {
        tmp[ len++ ] = ( char )( '0' + v % 10 );
        v /= 10;
    }
    // Reverse into buf
    i32 out = len < ( i32 )buf_cap - 1 ? len : ( i32 )buf_cap - 1;
    for ( i32 i = 0; i < out; ++i )
    {
        buf[ i ] = tmp[ len - 1 - i ];
    }
    buf[ out ] = '\0';
    return out;
}

i32
fmt_i64( char* buf, usize buf_cap, i64 v )
{
    /* Write decimal representation of v into buf. Returns bytes written (no null term). */

    if ( !buf || buf_cap == 0 )
        return 0;
    if ( v >= 0 )
        return fmt_u64( buf, buf_cap, ( u64 )v );
    if ( buf_cap < 2 )
    {
        buf[ 0 ] = '\0';
        return 0;
    }
    buf[ 0 ] = '-';
    // Handle LLONG_MIN carefully: negate as unsigned
    u64 uv = ( v == ( -9223372036854775807LL - 1 ) ) ? 9223372036854775808ULL : ( u64 )( -v );
    i32 n  = fmt_u64( buf + 1, buf_cap - 1, uv );
    return n + 1;
}

i32
fmt_hex64( char* buf, usize buf_cap, u64 v )
{
    // Write hex representation (lowercase, no "0x" prefix) of v into buf.

    static const char hex[] = "0123456789abcdef";
    if ( !buf || buf_cap == 0 )
        return 0;

    char tmp[ 18 ];
    i32  len = 0;
    if ( v == 0 )
    {
        tmp[ len++ ] = '0';
    }
    while ( v > 0 )
    {
        tmp[ len++ ] = hex[ v & 0xF ];
        v >>= 4;
    }
    i32 out = len < ( i32 )buf_cap - 1 ? len : ( i32 )buf_cap - 1;
    for ( i32 i = 0; i < out; ++i )
    {
        buf[ i ] = tmp[ len - 1 - i ];
    }
    buf[ out ] = '\0';
    return out;
}

/*============================================================================================*/