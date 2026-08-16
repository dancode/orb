/*==============================================================================================

    runtime_service/gui/font/gui_font_core.c -- the loaded-font registry + metric readers.

    The registry half of the font resource: the id-addressed registry (s_fonts[]), the active-font
    selection (s_active / s_active_id), the measurement readers, and the fill surface the
    loader (font/gui_font_load.c) and the render side share.  No fs, no atlas, no GPU -- glyph UV
    dispatch and atlas upload live render-side and reach a slot's pixels through font_slot_ptr.

    Compiled into the gui_font.c resource unit; included after font/gui_font.h.

==============================================================================================*/
// clang-format off

static font_slot_t      s_fonts     [ GUI_FONT_REGISTRY_MAX ];  // font registry; slot 0 is the default
static font_slot_t*     s_active    = NULL;                     // active slot, or NULL before the first activate
static u32              s_active_id = 0;                        // active slot id (0 = default, 1..MAX-1 = user-loaded)

/*==============================================================================================
    The internal fallback default -- what every reader resolves against when no loaded font is
    active.  A host that never loads a font (or whose load failed) used to NULL-deref in the metric
    readers; now it lays out against these nominal 16px metrics and a uniform advance, so the UI
    keeps its shape and only the glyphs are invisible (w/h 0 -- the tessellator emits no quads and
    the slot has no atlas tenant).  The ctx_begin contract still names the missing font loudly in
    asserting builds; this is what "keeps running" looks like everywhere else.
==============================================================================================*/

static font_slot_t s_fallback;   // built on first use; never registered, never freed

static font_slot_t*
font_fallback_slot( void )
{
    if ( !s_fallback.used )
    {
        s_fallback.used    = true;
        s_fallback.ascent  = 12;
        s_fallback.descent = -4;
        s_fallback.metrics = ( font_metrics_t ){ .char_h = 16.0f, .line_h = 18.0f, .size = 16.0f };
        for ( u32 i = 0; i < ORB_FONT_CP_COUNT; ++i )
            s_fallback.lookup[ i ].advance = 8;   // half the em: reads as text-shaped space
    }
    return &s_fallback;
}

/* The slot readers actually measure against: the active slot when it holds a LOADED font, else the
   fallback.  `used` is the test, not NULL -- font_activate can legally aim at a slot nothing was
   ever loaded into (the render side restores fonts by id), and a zeroed slot measures everything
   0 x 0, which is the invisible-UI failure the fallback exists to prevent. */
static font_slot_t*
font_live_slot( void )
{
    return ( s_active && s_active->used ) ? s_active : font_fallback_slot();
}

/*==============================================================================================
    font_slot_cp -- glyph record for a codepoint.  The ONE lookup rule: ASCII 32..126 indexes the
    dense lookup[] directly (the fast path every label takes -- returned as-is, so an ASCII slot a
    -range-only bake left empty answers a zeroed record: advance 0, invisible), anything above
    binary-searches the sorted ext[] records, and an ext miss resolves to the dense '?' slot.
    Measure and draw both come through here, so they agree in every case, including the misses.
==============================================================================================*/

const orb_font_glyph_t*
font_slot_cp( const font_slot_t* slot, u32 cp )
{
    if ( cp - ORB_FONT_CP_FIRST < ORB_FONT_CP_COUNT )
        return &slot->lookup[ cp - ORB_FONT_CP_FIRST ];

    u32 lo = 0, hi = slot->ext_count;
    while ( lo < hi )
    {
        u32 mid = ( lo + hi ) >> 1;
        u32 c   = slot->ext[ mid ].codepoint;
        if ( c == cp ) return &slot->ext[ mid ];
        if ( c < cp )  lo = mid + 1;
        else           hi = mid;
    }
    return &slot->lookup[ (u32)'?' - ORB_FONT_CP_FIRST ];
}

/*==============================================================================================
    Metric readers -- every one resolves through font_live_slot(), aimed by font_activate().
==============================================================================================*/

f32  font_char_h      ( void ) { return font_live_slot()->metrics.char_h; }
f32  font_line_h      ( void ) { return font_live_slot()->metrics.line_h; }
f32  font_em          ( void ) { return font_live_slot()->metrics.size;   }   // nominal type size (em) -- layout base

f32
font_char_advance( u32 cp )
{
    return (f32)font_slot_cp( font_live_slot(), cp )->advance;
}

/* Width of the first n bytes of str (stops early at a NUL).  Labels measure only their visible
   span this way -- the bytes before a "##" marker -- so reserved label space matches what draws.
   Bytes decode as UTF-8: a multi-byte sequence measures as its one codepoint, and anything the
   font lacks (or a malformed byte) advances as '?', matching what the draw path renders. */
f32
font_text_w_n( const char* str, u32 n )
{
    /* font_slot_cp's ASCII fast path hand-inlined with the slot hoisted: this is the hottest
       inner loop in the emit path (every label, cell, and value text measures through it), and a
       debug build (/Od) pays a real call per character through the helper form.  Only a lead byte
       >= 0x80 takes the decode + helper road; ASCII text never leaves the dense table. */
    const font_slot_t* slot = font_live_slot();
    f32                w    = 0.0f;
    u32                i    = 0;
    while ( i < n && str[ i ] )
    {
        u8 b = (u8)str[ i ];
        if ( b < 0x80u )
        {
            u32 idx = (u32)b - ORB_FONT_CP_FIRST;
            if ( idx >= ORB_FONT_CP_COUNT ) idx = (u32)'?' - ORB_FONT_CP_FIRST;
            w += (f32)slot->lookup[ idx ].advance;
            ++i;
        }
        else
        {
            u32 adv_b;
            u32 cp = utf8_decode( &str[ i ], &adv_b );
            if ( i + adv_b > n )
            {
                /* n splits the sequence.  The draw path copies exactly n bytes and NUL-terminates,
                   so it sees the truncated lead (and each stranded continuation byte) as one
                   1-byte U+FFFD -- measure the same bytes the same way or the seam disagrees at a
                   mid-sequence cut.  No live caller cuts there ("##" markers are ASCII), but the
                   contract holds for any n. */
                cp    = UTF8_REPLACEMENT;
                adv_b = 1u;
            }
            w += (f32)font_slot_cp( slot, cp )->advance;
            i += adv_b;
        }
    }
    return w;
}

/* Pixel width of a NUL-terminated run. */
f32
font_text_w( const char* str )
{
    return font_text_w_n( str, 0xFFFFFFFFu );
}

/* Log the active font (id, metrics). */
void
font_print_active( void )
{
    const font_metrics_t* m = &font_live_slot()->metrics;
    gui_log( GUI_LOG_INFO, "set font [%u] '<loaded>' (char_h=%.1f line_h=%.1f)",
             s_active_id, m->char_h, m->line_h );
}

/*==============================================================================================
    Registry selection + management.
==============================================================================================*/

/* Make an already-loaded id the active font.  Ignored if the id is empty or out of range. */
void
font_use( u32 id )
{
    if ( id >= GUI_FONT_REGISTRY_MAX || !s_fonts[ id ].used )
        return;
    font_activate( id );
}

/* Id of the active font slot -- callers save/restore this to push and pop fonts. */
u32
font_active_id( void )
{
    return s_active_id;
}

/* True once the active slot holds a LOADED font.  False from init() until a preset or the caller's
   own font_load() succeeds.  The metric readers are safe either way (they resolve to the internal
   fallback), so this gates "is real type installed", not "is it safe to call" -- hosts still check
   it to catch a missing font load rather than ship invisible text. */
bool
font_valid( void )
{
    return s_active != NULL && s_active->used;
}

/* Point the active-font pointers at slot `id`. */
void
font_activate( u32 id )
{
    s_active_id = id;
    s_active    = &s_fonts[ id ];
}

/* First free slot id in 1..MAX-1, or 0 when the registry is full (0 is reserved for the default). */
u32
font_alloc_slot( void )
{
    for ( u32 i = 1; i < GUI_FONT_REGISTRY_MAX; ++i )
        if ( !s_fonts[ i ].used )
            return i;
    return 0;
}

/* Registry slot by id -- the render-side loader's fill target.  NULL for an out-of-range id. */
font_slot_t*
font_slot_ptr( u32 id )
{
    return id < GUI_FONT_REGISTRY_MAX ? &s_fonts[ id ] : NULL;
}

/* The active slot -- read by the render-side glyph dispatch (font_glyph).  Resolves through the
   fallback like the metric readers, so dispatch and measurement can never disagree about which
   slot a frame's text is shaped by. */
font_slot_t*
font_active_slot( void )
{
    return font_live_slot();
}

/* Clear ONE registry slot: free its resident pixels + extended records, zero it.  Slot 0 (the
   default) and out-of-range ids are refused.  Releasing the ACTIVE slot re-aims at slot 0 --
   the active id must never keep naming a corpse (the DPI lineage guard reads it, and the metric
   readers would silently fall back while it lied).  The atlas half of a release lives render-side
   (font_slot_release, draw/gui_glyph.c): the tenant must go first, then this. */
void
font_slot_clear( u32 id )
{
    if ( id == 0 || id >= GUI_FONT_REGISTRY_MAX || !s_fonts[ id ].used )
        return;
    free( s_fonts[ id ].pixels );
    free( s_fonts[ id ].ext );
    s_fonts[ id ] = ( font_slot_t ){ 0 };
    if ( s_active_id == id )
        font_activate( 0 );
}

/* Clear the registry and active pointers at shutdown.  Frees each slot's resident glyph pixels;
   the atlas copy of those pixels is a separate GPU resource torn down once render-side
   (res_atlas_shutdown), so this just drops the CPU registry. */
void
font_registry_reset( void )
{
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
    {
        free( s_fonts[ i ].pixels );
        free( s_fonts[ i ].ext );
    }
    memset( s_fonts, 0, sizeof( s_fonts ) );
    s_active    = NULL;
    s_active_id = 0;
}

/* Decentralized memory accounting -- the registry plus each loaded font's resident R8 glyph pixels
   (the atlas holds its own GPU copy, counted render-side). */
u32
font_unit_mem_bytes( void )
{
    u32 b = (u32)sizeof( s_fonts );
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
        if ( s_fonts[ i ].used )
            b += s_fonts[ i ].atlas_w * s_fonts[ i ].atlas_h
               + s_fonts[ i ].ext_count * (u32)sizeof( orb_font_glyph_t );
    return b;
}

// clang-format on
/*============================================================================================*/
