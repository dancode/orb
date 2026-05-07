#ifndef BASE_H
#define BASE_H
/*==============================================================================================

    base.h -- engine standard library

        Fundamental types, platform macros, compiler helpers.

        No OS headers. No stdlib (except stdint/stdbool/stddef).
        Every other module includes this.

==============================================================================================*/
#include "orb.h"

/*==============================================================================================
    standard library includes
==============================================================================================*/
/* for now just include what a module actually uses for AI assistance to see full code */

#include "mem.h"
#include "char.h"
#include "bit.h"
#include "math.h"
#include "str.h"
#include "fmt.h"

/*============================================================================================*/
#endif    // BASE_H