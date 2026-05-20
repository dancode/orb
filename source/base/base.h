#ifndef BASE_H
#define BASE_H
/*==============================================================================================

    base.h -- engine (stateless) standard library

        rules: cannot allocate memory. cannot depend on the OS. included by everything.

==============================================================================================*/
#include "orb.h"

/*==============================================================================================
    standard library includes
==============================================================================================*/

#include "mem.h"
#include "bit.h"
#include "math.h"
#include "char.h"
#include "str.h"
#include "str_buf.h"
#include "str_arena.h"

/*============================================================================================*/
#endif    // BASE_H