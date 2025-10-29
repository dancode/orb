#ifndef STR_INTERN_H
#define STR_INTERN_H
/*==============================================================================================

    str_intern.h

==============================================================================================*/

#include "orb.h"

/* Interned string entry (a compact representation for quick comparison) */
/* Think of it like an “opaque ID” for a string */

typedef uint32_t sid_t;
#define SID_INVALID 0

uint32_t sid_hash( const char* s );

void  sid_init( void );
sid_t sid_intern( const char* str, int32_t len );
sid_t sid_intern_cstr( const char* str );

/*============================================================================================*/
#endif