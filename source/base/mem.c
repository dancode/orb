/*==============================================================================================

    mem.c -- Memory operations (NO allocation).

        Delegates to compiler builtins / libc intrinsics for optimal codegen.
        No global state. No allocation.

==============================================================================================*/

/*============================================================================================*/

void
mem_swap( void* a, void* b, usize n )
{
    u8* pa = ( u8* )a;
    u8* pb = ( u8* )b;
    // Work in 8-byte chunks when possible for speed
    usize chunks = n / sizeof( u64 );
    usize remain = n % sizeof( u64 );

    // We need the u64 type — include base types or define locally
    typedef unsigned long long u64_local;
    u64_local*                 ca = ( u64_local* )pa;
    u64_local*                 cb = ( u64_local* )pb;
    for ( usize i = 0; i < chunks; ++i )
    {
        u64_local tmp = ca[ i ];
        ca[ i ]       = cb[ i ];
        cb[ i ]       = tmp;
    }
    pa = ( u8* )( ca + chunks );
    pb = ( u8* )( cb + chunks );
    for ( usize i = 0; i < remain; ++i )
    {
        u8 tmp  = pa[ i ];
        pa[ i ] = pb[ i ];
        pb[ i ] = tmp;
    }
}

void
mem_reverse( void* buf, usize n )
{
    u8* lo = ( u8* )buf;
    u8* hi = lo + n - 1;
    while ( lo < hi )
    {
        u8 tmp = *lo;
        *lo++  = *hi;
        *hi--  = tmp;
    }
}

/*============================================================================================*/