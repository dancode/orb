#ifndef GUI_FONT_H
#define GUI_FONT_H
/*==============================================================================================

    runtime_service/gui/font/gui_font.h -- GUI_FONT: the font resource (metrics + pixels).

    A low-level resource the GUI depends on -- not a GUI feature: it holds no widgets and draws
    nothing.  It parses a baked .orb_font into the two things the GUI needs and holds them at the
    bottom of the stack beside rect, readable by every layer above:
      * TYPE METRICS -- typography (line height, char height, em) and the per-glyph advance table.
        Layout sizes cells to labels, the text-edit mechanism maps clicks to byte offsets, the
        tessellator positions glyph quads -- all sizes-and-math over these, no drawing.
      * GLYPH PIXELS -- the resident R8 bitmap (slot->pixels).  Kept here, not just in the atlas,
        so a font can be loaded now and uploaded to the GPU later -- lazily, on first use.

    This leaf never touches a GPU resource.  The render side is the one CONSUMER of the pixels: it
    reads a slot's pixels, packs them into the shared atlas, and writes the opaque tenant handle
    back into the slot (font_slot_ptr()->atlas_tenant).  So UV dispatch (font_glyph) and atlas
    upload stay render-side (draw/gui_glyph.c); nothing here calls the atlas.  .orb_font is the
    only source format, so its record type is pulled here.

==============================================================================================*/

#include "tools/font_tool/orb_font.h"   /* orb_font_glyph_t + the .orb_font on-disk record */
#include "runtime_service/gui/gui.h"    /* gui_builtin_font_t -- the built-in preset enum   */

// clang-format off

/* Capacity of the loaded-font registry.  Slot 0 is the default; loaded fonts occupy 1..MAX-1. */
#define GUI_FONT_REGISTRY_MAX 16

/* The active-font measurement surface, resolved once at load -- pure type metrics, what layout and
   measurement code read.  Nothing here names a GPU resource (atlas-sampling parameters belong to
   the shared resource atlas and are read render-side). */
typedef struct
{
    f32  line_h;   // total line advance
    f32  char_h;   // pixel height of the glyph box (ascent + descent)
    f32  size;     // nominal type size (em) in pixels -- the base for layout proportions

} font_metrics_t;

/* One registry entry: a loaded proportional .orb_font.  Pure data -- it owns its resident glyph
   pixels; atlas_tenant is a bare handle into the shared resource atlas (0 = none), filled
   render-side after the pixels are uploaded; no GPU type appears here. */
typedef struct
{
    font_metrics_t      metrics;            // resolved metrics; the active pointer aims here
    bool                used;               // slot occupied
    u32                 atlas_tenant;       // handle into the shared resource atlas (render-filled)
    i32                 ascent;             // pixels above baseline (positive)
    i32                 descent;            // pixels below baseline (negative)
    orb_font_glyph_t    lookup[ ORB_FONT_CP_COUNT ];  // codepoints 32..126; advance == 0 = missing

    u8*                 pixels;             // resident R8 glyph bitmap (owned here); NULL until loaded
    u32                 atlas_w;            // pixel width  of `pixels` (the packed .orb_font atlas)
    u32                 atlas_h;            // pixel height of `pixels`
    bool                needs_upload;       // pixels (re)loaded; the render side must pack them into the atlas

    /* What the bytes in `pixels` MEAN: 0 = coverage, > 0 = a distance field with that spread in
       pixels (orb_font.h, sdf_range).  It is a property of the LOADED FILE, so it is resolved once
       here at parse time; the render side reads it to pick which atlas the glyphs pack into and
       which sampling model their draws carry.  Metrics are identical either way -- an SDF glyph is
       a larger bitmap with a compensating bearing, and the advance never moves -- so layout,
       measurement and hit-testing never learn this exists. */
    u32                 sdf_range;          // 0 = coverage bitmap; > 0 = distance field, spread px

} font_slot_t;

/*==============================================================================================
    Font (re)load -- parse a baked .orb_font into a registry slot.  Pure resource work: fills the
    slot's metrics + advance table and stores its resident R8 glyph pixels, then flags it
    needs_upload for the render side to pack into the atlas.  No GPU, no atlas call here; metrics
    are ready on return, the pixels reach the GPU at the render side's next frame_begin sync.
==============================================================================================*/

u32             font_load          ( const char* path );         // parse into a new id + activate; 0 = fail
bool            font_load_into      ( u32 id, const char* path ); // parse into an existing id (0 = default); false = bad id / load fail

/*==============================================================================================
    Built-in font presets -- a convenience over font_load_into: map a gui_builtin_font_t (gui.h) to
    its baked .orb_font asset and load it into the default slot.  Picking and loading a file is
    resource work, so it lives with the resource, not render-side.
==============================================================================================*/

bool            font_load_builtin      ( gui_builtin_font_t font ); // load a built-in preset into slot 0; true no-op for GUI_FONT_NONE
const char*     font_builtin_rel_path  ( gui_builtin_font_t font ); // preset's asset path relative to the root; NULL for NONE / out of range

/*==============================================================================================
    Metric readers -- the active font's measurement surface.  Pure sizes + math over the loaded
    tables; callable from anywhere (layout, the interact text-edit mechanism, the tessellator).
==============================================================================================*/

f32             font_char_h        ( void );                   // glyph-box height of the active font (ascent+descent)
f32             font_line_h        ( void );                   // line advance of the active font
f32             font_em            ( void );                   // nominal type size (em) -- the layout proportion base
f32             font_char_advance  ( u8 ch );                  // horizontal advance of one glyph
f32             font_text_w        ( const char* str );        // pixel width of a NUL-terminated run
f32             font_text_w_n      ( const char* str, u32 n ); // pixel width of the first n characters
void            font_print_active  ( void );                   // log the active font's id + metrics

/*==============================================================================================
    Registry selection + management.  font_use / active_id / valid select and query.
    font_activate / font_alloc_slot / font_slot_ptr / font_active_slot are the slot surface the
    loader (font_load, above) and the render side share: the loader fills a slot's metrics + pixels,
    the render side reads slot->pixels via font_slot_ptr to upload.  font_registry_reset clears the
    registry (and frees resident pixels) at shutdown.
==============================================================================================*/

void            font_use                ( u32 id );     // make an already-loaded id active
u32             font_active_id          ( void );       // id of the active slot (save/restore)
bool            font_valid              ( void );       // true once a font is installed (gates glyph reads)
void            font_activate           ( u32 id );     // point the active pointers at slot id
u32             font_alloc_slot         ( void );       // first free id in 1..MAX-1, or 0 if full
font_slot_t*    font_slot_ptr           ( u32 id );     // registry slot by id (loader fill target); NULL if OOR
font_slot_t*    font_active_slot        ( void );       // active slot (render's glyph dispatch reads it)
void            font_registry_reset     ( void );       // clear the registry + active pointers (shutdown)

/* Decentralized memory accounting -- the registry, summed into cpu_frontend_bytes. */
u32 font_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_FONT_H
