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
#include "runtime_service/gui/gui.h"    /* GUI_FONT_FAMILY_MAX, the public font types      */

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
    i32                 ascent;             // pixels above baseline (positive)
    i32                 descent;            // pixels below baseline (negative)

    u16                 atlas_tenant;       // handle into the shared resource atlas (render-filled) --
                                            // capped at GUI_RES_ATLAS_MAX_TENANTS (320)
    u16                 sdf_range;          // 0 = coverage bitmap; > 0 = distance field, spread px --
                                            // bounded by ORB_FONT_PAGE_MAX_W_SDF at load (font_header_read)
    u16                 atlas_w;            // pixel width  of `pixels` (the packed .orb_font atlas) --
                                            // bounded by ORB_FONT_PAGE_MAX_W_SDF at load
    u16                 atlas_h;            // pixel height of `pixels` -- bounded by 4096 at load
    u16                 ext_count;          // records in ext[] -- bounded by ORB_FONT_MAX_GLYPHS (2048)

    /* Identity for debug readouts (the font overlay): the resource name the bake was loaded
       under ("font/cascadiamono/16"), pool-interned (font_slot_name reads it back).  Purely
       informational -- nothing resolves or compares against it. */
    u16                 name_off;

    bool                used;               // slot occupied
    bool                tenant_sdf;         // atlas the tenant was created in (render-filled) -- a
                                            // handle only indexes its own atlas, so a reload that
                                            // changes kind must release before re-adding
    bool                needs_upload;       // pixels (re)loaded; the render side must pack them into the atlas
    bool                upload_failed;      // the upload could not place the page (atlas growth capped):
                                            // glyphs draw invisible (no tenant) and the render side stops
                                            // retrying until a reload clears this

    orb_font_glyph_t    lookup[ ORB_FONT_CP_COUNT ];  // codepoints 32..126; advance == 0 = missing

    /* Extended glyph records -- everything a -range bake carries beyond ASCII.  Sorted by codepoint
       for binary search (font_slot_cp); owned here like `pixels` (malloc'd by the loader, freed on
       reload / registry reset).  NULL/0 for an ASCII-only font, which keeps the dense lookup[] the
       entire fast path. */

    orb_font_glyph_t*   ext;                // sorted extended records (owned here); NULL if none

    u8*                 pixels;             // resident R8 glyph bitmap (owned here); NULL until loaded

} font_slot_t;

/*==============================================================================================
    Font (re)load -- parse a baked .orb_font into a registry slot.  Pure resource work: fills the
    slot's metrics + advance table and stores its resident R8 glyph pixels, then flags it
    needs_upload for the render side to pack into the atlas.  No GPU, no atlas call here; metrics
    are ready on return, the pixels reach the GPU at the render side's next frame_begin sync.

    The by-name forms read `name` + ".orb_font" through the fs mounts (gui_res.h); the _mem
    forms parse bytes the caller holds (the runtime baker's output, a test fixture) and take
    `name` only as the slot's identity for the debug overlay.
==============================================================================================*/

u32             font_load          ( const char* name );          // parse into a new id + activate; 0 = fail
u32             font_load_mem      ( const void* data, u32 size, const char* name );
bool            font_load_into     ( u32 id, const char* name );  // parse into an existing id (0 = default); false = bad id / load fail
bool            font_load_into_mem ( u32 id, const void* data, u32 size, const char* name );

/*==============================================================================================
    Families -- a family is the directory under content/font a bake lives in ("cascadiamono").
    The resolver composes "font/<family>/<size>" from it; the runtime baker needs the typeface
    behind it, which for the curated families is a different spelling (an OS face name or a TTF
    under assets/font_source).  Until recipes carry the face (RESOURCE_ID_PLAN Phase 6), this
    small table maps the curated directories to their faces; an unknown family passes through
    unchanged, which is right for a directory named after a face dev_font can already resolve.
==============================================================================================*/

const char*        font_family_face( const char* family );   // baker source for a family directory; never NULL

/* Memo keying: lowercase alphanumeric-only copy, so "Cascadia Mono" == "CascadiaMono" ==
   "cascadia_mono" (unit-tested in sb_gui_test). */

void               font_name_normalize ( const char* s, char* out, int out_size );

/*==============================================================================================
    Metric readers -- the active font's measurement surface.  Pure sizes + math over the loaded
    tables; callable from anywhere (layout, the interact text-edit mechanism, the tessellator).
    Always safe: with no loaded font they resolve to an internal fallback (nominal metrics, uniform
    advance, invisible glyphs), so a missing font degrades to blank text rather than a crash.
==============================================================================================*/

f32             font_char_h        ( void );                   // glyph-box height of the active font (ascent+descent)
f32             font_line_h        ( void );                   // line advance of the active font
f32             font_em            ( void );                   // nominal type size (em) -- the layout proportion base
f32             font_char_advance  ( u32 cp );                 // horizontal advance of one codepoint
f32             font_text_w        ( const char* str );        // pixel width of a NUL-terminated run
f32             font_text_w_n      ( const char* str, u32 n ); // pixel width of the first n BYTES (UTF-8 decoded)
void            font_print_active  ( void );                   // log the active font's id + metrics

/*==============================================================================================
    Registry selection + management.  font_use / active_id / valid select and query.
    font_activate / font_alloc_slot / font_slot_ptr / font_active_slot are the slot surface the
    loader (font_load, above) and the render side share: the loader fills a slot's metrics + pixels,
    the render side reads slot->pixels via font_slot_ptr to upload.  font_registry_reset clears the
    registry (and frees resident pixels) at shutdown.
==============================================================================================*/

/* Glyph record for a codepoint in a slot: dense lookup[] for ASCII, binary search in ext[] beyond,
   and a miss resolves to '?' -- the one lookup rule measure (font_char_advance) and draw
   (font_slot_glyph, render-side) both go through, so they can never disagree. */
const orb_font_glyph_t* font_slot_cp( const font_slot_t* slot, u32 cp );

void            font_use                ( u32 id );     // make an already-loaded id active
u32             font_active_id          ( void );       // id of the active slot (save/restore)
bool            font_valid              ( void );       // true once a LOADED font is active (readers are fallback-safe either way)
void            font_activate           ( u32 id );     // point the active pointers at slot id
u32             font_alloc_slot         ( void );       // first free id in 1..MAX-1, or 0 if full
font_slot_t*    font_slot_ptr           ( u32 id );     // registry slot by id (loader fill target); NULL if OOR
font_slot_t*    font_active_slot        ( void );       // active slot (render's glyph dispatch reads it)
void            font_slot_clear         ( u32 id );     // free + zero one slot (active re-aims at 0); id 0 refused
void            font_registry_reset     ( void );       // clear the registry + active pointers (shutdown)

/* The name pool behind font_slot_t.name_off (gui_font_load.c interns into it; the font overlay
   reads back through font_slot_name).  A dedicated pair rather than exposing the pool itself,
   matching font_alloc_slot / font_slot_ptr. */
u16             font_name_intern        ( const char* s );    // intern a family-root name; returns its offset
const char*     font_slot_name          ( u32 id );           // "" for an unused or out-of-range slot

/* Decentralized memory accounting -- the registry, summed into cpu_frontend_bytes. */
u32 font_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_FONT_H
