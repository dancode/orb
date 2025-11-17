/*==============================================================================================

    sid_system.c : string interning system

    - Hash Table: Dynamic hash table with linear probing and automatic resizing
    - String Storage: Dynamic arena allocator that grows as needed
    - String ID: A sid_t (uint32_t) that is an offset into the string pool
    - Key Feature: Case-insensitive lookups, but case-preserving storage
    - Dynamic resizing, load factor management, debug metrics

    NOTE: Strings > 255 are rejected -- all strings must be less than 256 characters.
    NOTE: this is not currenttly thread-safe.

==============================================================================================*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "orb.h"

/*============================================================================================*/
/* unity compiled module */ 

#include "sid/sid.h"
#include "sid/sid.c"

/*============================================================================================*/
/* optional tests */

#include "test/test_core_sid.c"

/*============================================================================================*/