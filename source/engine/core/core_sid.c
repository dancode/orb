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
/* unity compiled module */ 

// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <stdarg.h>
// 
// #include "orb.h"
// #include "debug/assert.h"

#include "sid/sid.h"
#include "sid/sid.c"

/*============================================================================================*/
/* optional tests */

// #include "test/test_core_sid.c"

/*============================================================================================*/