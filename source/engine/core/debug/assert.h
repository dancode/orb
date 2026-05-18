/*==============================================================================================

    engine/core/debug/assert.h

    Replaces the orb.h ORB_ASSERT / ORB_ASSERT_MSG stubs with a full-featured handler.

    After core.h is included (which pulls this in after core_api.h), assertions:
      - emit a VS-clickable diagnostic: file(line): assertion failed: <cond>
      - route the same text to OutputDebugStringA for the VS Output pane
      - support skip mode so test suites can continue past failures
      - call ORB_TRAP() unless skip mode is active

    Call sites expand to: one branch + one indirect call + one conditional trap.
    The report body is ORB_NOINLINE — no code bloat at call sites.

    Do not include this directly. Include core.h.

==============================================================================================*/
#pragma once

#ifndef CORE_API_H
    #error "assert.h must not be included directly; include core.h"
#endif

/*==============================================================================================
    Skip mode — for test suites that need to run past assertion failures
==============================================================================================*/

/* When enabled, ORB_ASSERT prints the diagnostic but does not call ORB_TRAP(). */
void core_assert_set_skip( bool skip );

/*==============================================================================================
    Override orb.h stubs
==============================================================================================*/
// clang-format off

#undef ORB_ASSERT
#undef ORB_ASSERT_MSG

#if DEBUG

#define ORB_ASSERT( cond )                                                              \
    do                                                                                  \
    {                                                                                   \
        if ( ORB_UNLIKELY( !( cond ) ) )                                                \
        {                                                                               \
            if ( !core()->assert_report( #cond, NULL, __FILE__, __LINE__ ) )        \
                ORB_TRAP();                                                             \
        }                                                                               \
    } while ( 0 )

#define ORB_ASSERT_MSG( cond, msg )                                                     \
    do                                                                                  \
    {                                                                                   \
        if ( ORB_UNLIKELY( !( cond ) ) )                                                \
        {                                                                               \
            if ( !core()->assert_report( #cond, ( msg ), __FILE__, __LINE__ ) )     \
                ORB_TRAP();                                                             \
        }                                                                               \
    } while ( 0 )

#else    /* RELEASE */
#define ORB_ASSERT( cond )          ( ( void )0 )
#define ORB_ASSERT_MSG( cond, msg ) ( ( void )0 )

#endif

/* Lowercase aliases — undef standard assert in case <assert.h> was included */
#undef assert
#undef assert_msg

#define assert( cond )          ORB_ASSERT( cond )
#define assert_msg( cond, msg ) ORB_ASSERT_MSG( cond, msg )

/*============================================================================================*/
// clang-format on