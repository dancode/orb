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

#include "base/mem.h"
#include "base/bit.h"
#include "base/math.h"
#include "base/char.h"
#include "base/str.h"
#include "base/str_buf.h"
#include "base/str_arena.h"

/*============================================================================================*/
#endif    // BASE_H