#ifndef STR_INTERN_H
#define STR_INTERN_H
/*==============================================================================================

    str_intern.h


==============================================================================================*/
#include "orb.h"

typedef struct sid_s    // String interning handle (32-bit offset into global pool)
{
    uint32_t hash;    // Precomputed hash of the string
    uint32_t off;     // Offset into the arena where the string is stored

} sid_t;

uint32_t    sid_hash( const char* s );

void        str_intern_init();
sid_t       str_intern( const char* s );
const char* str_from_sid( sid_t sid );
const char* str_from_off( uint32_t off );

/*============================================================================================*/
#endif