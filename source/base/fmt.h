#ifndef BASE_FMT_H
#define BASE_FMT_H
/*==============================================================================================

    fmt.h -- fast CRT-free printf-family formatting (vendored stb_sprintf, source/vendor).

    WHY: the CRT's snprintf pays locale plumbing on every call, and the debug CRT (/MDd) adds
    per-call validation on top -- measured as the dominant term of the gui emit path in Debug
    (sb_gui_stress table test) and a 3-8x tax in Release.  stb_sprintf formats the same C99
    surface with none of that; floats ("%.3f") are where the gap is widest.

    This header is DECLARATIONS ONLY.  The implementation compiles exactly once, into base.lib
    (base/fmt.c) -- consumers add `dep base` in orb.targets rather than each unit carrying its
    own copy.  base stays shared-state-free: stb_sprintf's only global is an immutable period /
    comma default (stbsp_set_separators is never called), safe to link into every module.

    fmt_snprintf / fmt_vsnprintf are C99-shaped: bounded by the buffer size, always
    NUL-terminate, return the would-be length (so `n >= size` means truncated).  Differences
    from the CRT: no locale, no wide strings (%ls), plus stb extensions (%b binary, $ metric
    suffixes) -- see vendor/stb_sprintf.h.

==============================================================================================*/

#include "vendor/stb_sprintf.h"   /* prototypes only -- STB_SPRINTF_IMPLEMENTATION lives in fmt.c */

/* Engine-facing names; call sites read fmt_, the stb spelling stays an implementation detail. */
#define fmt_snprintf  stbsp_snprintf
#define fmt_vsnprintf stbsp_vsnprintf

/*============================================================================================*/
#endif    // BASE_FMT_H
