/*==============================================================================================

    runtime_service/gui/text/gui_text_core.c -- the loaded-font registry + metric readers.

    The pure-data half of the font system: the id-addressed registry (s_fonts[]), the active-font
    pointers (s_active / s_font), the measurement readers, and the selection + fill surface the
    render-side loader drives.  No fs, no atlas, no GPU -- the .orb_font loader and glyph UV
    dispatch live render-side and reach this registry through font_slot_ptr / font_activate.

    Compiled into the gui_text.c leaf unit; included after text/gui_text.h.

==============================================================================================*/
// clang-format off

static font_slot_t      s_fonts     [ GUI_FONT_REGISTRY_MAX ];  // font registry; slot 0 is the default
static font_slot_t*     s_active    = NULL;                     // active slot (s_font == &s_active->metrics)
static u32              s_active_id = 0;                        // active slot id (0 = default, 1..MAX-1 = user-loaded)
static font_metrics_t*  s_font      = NULL;                     // active font's metrics (read by every accessor)

/*==============================================================================================
    Per-glyph advance for a slot.  Out-of-range bytes advance as '?' -- matching what the draw
    path (font_glyph, render-side) renders for them, so measure and draw agree.
==============================================================================================*/

static f32
font_slot_char_advance( const font_slot_t* slot, u8 ch )
{
    if ( ch < ORB_FONT_CP_FIRST || ch > ORB_FONT_CP_LAST ) ch = (u8)'?';
    return (f32)slot->lookup[ ch - ORB_FONT_CP_FIRST ].advance;
}

/*==============================================================================================
    Metric readers -- resolved from s_font / s_active, aimed by font_activate().
==============================================================================================*/

f32  font_char_h      ( void ) { return s_font->type.char_h; }
f32  font_line_h      ( void ) { return s_font->type.line_h; }
f32  font_em          ( void ) { return s_font->type.size;   }   // nominal type size (em) -- layout base

f32
font_char_advance( u8 ch )
{
    return font_slot_char_advance( s_active, ch );
}

/* Width of the first n bytes of str (stops early at a NUL).  Labels measure only their visible
   span this way -- the bytes before a "##" marker -- so reserved label space matches what draws.
   Out-of-range bytes advance as '?', matching what the draw path (font_glyph) renders. */
f32
font_text_w_n( const char* str, u32 n )
{
    /* font_slot_char_advance hand-inlined with the slot hoisted: this is the hottest inner loop
       in the emit path (every label, cell, and value text measures through it), and a debug
       build (/Od) pays a real call per character through the helper form. */
    const font_slot_t* slot = s_active;
    f32                w    = 0.0f;
    for ( u32 i = 0; i < n && str[ i ]; ++i )
    {
        u8 ch = (u8)str[ i ];
        if ( ch < ORB_FONT_CP_FIRST || ch > ORB_FONT_CP_LAST ) ch = (u8)'?';
        w += (f32)slot->lookup[ ch - ORB_FONT_CP_FIRST ].advance;
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
    printf( "[gui] set font [%u] '<loaded>' (char_h=%.1f line_h=%.1f)\n",
            s_active_id, s_font->type.char_h, s_font->type.line_h );
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

/* True once a font has been activated (s_font set by font_activate) and every metrics/glyph
   accessor is safe to call.  False from init() until a preset or the caller's own font_load()
   succeeds -- layout code that needs type metrics must gate on this rather than call in blind. */
bool
font_valid( void )
{
    return s_font != NULL;
}

/* Point the active-font pointers at slot `id`. */
void
font_activate( u32 id )
{
    s_active_id = id;
    s_active    = &s_fonts[ id ];
    s_font      = &s_active->metrics;
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

/* The active slot -- read by the render-side glyph dispatch (font_glyph). */
font_slot_t*
font_active_slot( void )
{
    return s_active;
}

/* Clear the registry and active pointers at shutdown.  Fonts own no GPU resource of their own --
   their glyph pixels live in the shared resource atlas, torn down once render-side -- so this just
   drops the CPU registry. */
void
font_registry_reset( void )
{
    memset( s_fonts, 0, sizeof( s_fonts ) );
    s_active    = NULL;
    s_active_id = 0;
    s_font      = NULL;
}

/* Decentralized memory accounting -- the loaded-font registry (CPU-side; glyph pixels live in the
   shared atlas, counted render-side). */
u32
gui_text_unit_mem_bytes( void )
{
    return (u32)sizeof( s_fonts );
}

// clang-format on
/*============================================================================================*/
