#ifndef BASE_H
#define BASE_H
/*==============================================================================================

    base.h -- engine (stateless) standard library

        rules: cannot allocate memory. cannot depend on the OS. included by everything.

==============================================================================================*/
#include "orb.h"

#define ORB_USE_CONTAINERS 0    /* currently no macro-based containers used */
/*==============================================================================================
    standard library includes
==============================================================================================*/

#include "base/mem.h"
#include "base/bit.h"
#include "base/math.h"
#include "base/math_rng.h"
#include "base/char.h"
#include "base/str.h"
#include "base/str_buf.h"
#include "base/str_arena.h"

/* only if we ever really need them -- keep ununsed for now */
#if ORB_USE_CONTAINERS
    #include "base/container.h"
#endif 

/*============================================================================================*/
#endif    // BASE_H