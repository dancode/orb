// clang-format off
/*============================================================================================*/

#include "orb.h"
/* Include for MSVC intrinsics */
#if defined( _MSC_VER )
#    include <intrin.h>
#    pragma intrinsic( _BitScanReverse )
#endif

/*============================================================================================*/
/* Helper: Count Leading Zeros (portable) */

u32
u32_clz( u32 v )
{
#if defined( _MSC_VER )

    unsigned long index;
    if ( _BitScanReverse( &index, v ) )
        return 31 - index;
    return 32; /* v was 0 */

#elif defined( __GNUC__ ) || defined( __clang__ )

    if ( v == 0 )
        return 32;
    return ( u32 )__builtin_clz( v );

#else

    /* Fallback (slower) */
    u32 n = 0;
    if ( v == 0 ) return 32;
    if ( ( v & 0xFFFF0000 ) == 0 ) { n += 16; v <<= 16; }
    if ( ( v & 0xFF000000 ) == 0 ) { n += 8;  v <<= 8;  }
    if ( ( v & 0xF0000000 ) == 0 ) { n += 4;  v <<= 4;  }
    if ( ( v & 0xC0000000 ) == 0 ) { n += 2;  v <<= 2;  }
    if ( ( v & 0x80000000 ) == 0 ) { n += 1; }
    return n;

#endif
}

u32 
u32_highest_bit( u32 v )
{
    return 31 - u32_clz( v );
}

/*============================================================================================*/
/* Helper: Round up to the next highest power of two */

u32
u32_round_up_pow2( u32 v )
{
    /* Handle 0 explicitly to avoid underflow */
    if ( v == 0 )
        return 0;

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/*============================================================================================*/
// clang-format on