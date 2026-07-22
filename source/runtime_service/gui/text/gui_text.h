#ifndef GUI_TEXT_H
#define GUI_TEXT_H
/*==============================================================================================

    runtime_service/gui/text/gui_text.h -- GUI_TEXT: the leaf font-metrics library.

    Text is a foundational UI element, like rects: measuring a glyph run is sizes-and-math, not
    drawing.  So the loaded-font METRICS -- typography (line height, char height, em) and the
    per-glyph advance table -- live at the bottom of the stack beside rect, readable by BOTH
    servers and every layer above: layout sizes cells to labels, the text-edit mechanism maps
    clicks to byte offsets, the tessellator positions glyph quads.  Only the glyph DRAWING --
    atlas pixels, UV dispatch (font_glyph), the .orb_font loader that registers pixels into the
    shared atlas -- stays render-side (render/gui_render.h + the render unit's font files); this
    leaf never touches a GPU resource.

    The loaded-font registry lives in this unit (text/gui_text_core.c).  The render-side loader
    parses the .orb_font, registers glyph pixels into the shared atlas, and fills a slot through
    font_slot_ptr() + font_activate(); the metric readers below then serve everyone.  .orb_font is
    the only source format, so its record type is pulled here.

==============================================================================================*/

#include "tools/font_tool/orb_font.h"   /* orb_font_glyph_t + the .orb_font on-disk record */

// clang-format off

/* Capacity of the loaded-font registry.  Slot 0 is the default; loaded fonts occupy 1..MAX-1. */
#define GUI_FONT_REGISTRY_MAX 16

/* Pure type metrics -- what layout / measurement code reads.  Nothing here names a GPU resource. */
typedef struct
{
    f32  line_h;   // total line advance
    f32  char_h;   // pixel height of the glyph box (ascent + descent)
    f32  size;     // nominal type size (em) in pixels -- the base for layout proportions

} font_typography_t;

/* Everything the active-font accessors read, resolved once at load.  Only typography lives here;
   the atlas-sampling parameters belong to the shared resource atlas and are read render-side. */
typedef struct
{
    font_typography_t   type;

} font_metrics_t;

/* One registry entry: a loaded proportional .orb_font.  Pure data -- atlas_tenant is a bare handle
   into the shared resource atlas (0 = none), filled render-side; no GPU type appears here. */
typedef struct
{
    font_metrics_t      metrics;            // resolved metrics; the active pointer aims here
    bool                used;               // slot occupied
    u32                 atlas_tenant;       // handle into the shared resource atlas (render-filled)
    i32                 ascent;             // pixels above baseline (positive)
    i32                 descent;            // pixels below baseline (negative)
    orb_font_glyph_t    lookup[ ORB_FONT_CP_COUNT ];  // codepoints 32..126; advance == 0 = missing

} font_slot_t;

/*==============================================================================================
    Metric readers -- the active font's measurement surface.  Pure sizes + math over the loaded
    tables; callable from anywhere (layout, the interact text-edit mechanism, the tessellator).
==============================================================================================*/

f32  font_char_h      ( void );                   // glyph-box height of the active font (ascent+descent)
f32  font_line_h      ( void );                   // line advance of the active font
f32  font_em          ( void );                   // nominal type size (em) -- the layout proportion base
f32  font_char_advance( u8 ch );                  // horizontal advance of one glyph
f32  font_text_w      ( const char* str );        // pixel width of a NUL-terminated run
f32  font_text_w_n    ( const char* str, u32 n ); // pixel width of the first n characters
void font_print_active( void );                   // log the active font's id + metrics

/*==============================================================================================
    Registry selection + management.  font_use / active_id / valid select and query.
    font_activate / font_alloc_slot / font_slot_ptr / font_active_slot are the write + fill surface
    the render-side loader drives (it parses the .orb_font, registers glyph pixels into the atlas,
    then fills the slot through these).  font_registry_reset clears the registry at shutdown.
==============================================================================================*/

void         font_use         ( u32 id );         // make an already-loaded id active
u32          font_active_id   ( void );           // id of the active slot (save/restore)
bool         font_valid       ( void );           // true once a font is installed (gates glyph reads)
void         font_activate    ( u32 id );         // point the active pointers at slot id
u32          font_alloc_slot  ( void );           // first free id in 1..MAX-1, or 0 if full
font_slot_t* font_slot_ptr    ( u32 id );         // registry slot by id (loader fill target); NULL if OOR
font_slot_t* font_active_slot ( void );           // active slot (render's glyph dispatch reads it)
void         font_registry_reset( void );         // clear the registry + active pointers (shutdown)

/* Decentralized memory accounting -- the registry, summed into cpu_frontend_bytes. */
u32 gui_text_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_TEXT_H
