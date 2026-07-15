#ifndef PACK_MINIZ_H
#define PACK_MINIZ_H
/*==============================================================================================

    engine/pack/pack_miniz.h -- miniz build configuration for the pack library.

    Both the implementation TU (pack_miniz.c, which compiles vendor/miniz.c) and the caller
    (pack.c, which includes vendor/miniz.h) include this FIRST so the two agree on how miniz
    is configured.  We keep stdio out of miniz -- every byte enters and leaves pack as a
    memory buffer; disk I/O belongs to the caller (the engine goes through sys).

    This is the ONLY miniz configuration in the engine: pack owns the single amalgamation
    copy, so there is no second site to drift against.

==============================================================================================*/

#define MINIZ_NO_STDIO

/*============================================================================================*/
#endif    // PACK_MINIZ_H
