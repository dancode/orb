/*==============================================================================================

    runtime_service/gui/render/resource/gui_atlas.h -- Owned GPU-texture asset (type + lifecycle).

    A CPU-authored R8 coverage bitmap uploaded to an owned GPU texture and registered for bindless
    sampling.  RHI itself tracks neither the pairing nor the lifetime -- texture_create /
    register_texture hand back a raw handle + index and leave ownership entirely to the caller (see
    rhi_api.h).  gui_atlas_t is that caller-side pairing, so the create/upload/register/unregister
    sequence is written once.  The shared resource atlas (gui_res_atlas.c) composes one of these for
    its single texture; nothing else owns a gui_atlas_t directly anymore.

    This is deliberately NOT a general asset system: it has no name table, no refcounting, no
    hot-reload.  A real asset pipeline (see the "asset pipeline later" notes in gui_api.h and
    gui_icon.c) is a different, larger problem -- indexing/streaming/dependency tracking across many
    asset kinds -- and should not be backed into this texture-sized helper.

    Included by gui_render.c before resource/gui_res_atlas.h.

==============================================================================================*/
#pragma once

/*==============================================================================================
    gui_atlas_t -- one owned R8 GPU texture + its bindless slot.

    atlas_idx == 0 means "not registered" (RHI reserves 0 as the empty bindless slot); callers
    check this, not rhi_handle_valid( atlas ), since the two are set/cleared together by the
    functions below.
==============================================================================================*/

typedef struct
{
    rhi_texture_t atlas;        // owned GPU texture (R8_UNORM)
    u32           atlas_idx;    // bindless index (0 = not registered)
    u32           atlas_w;      // pixel width  (memory accounting)
    u32           atlas_h;      // pixel height (memory accounting)

} gui_atlas_t;

/* Create an R8 texture of size w x h, upload `pixels` (row-major, w*h bytes), and register it
   bindless.  On success fills every field of `a`; on failure `a` is left zeroed (no partial GPU
   resources leaked -- each step tears down what it created before returning false).
   `debug_name` is forwarded to rhi for GPU-side labeling. */
bool gui_atlas_create( gui_atlas_t* a, u32 w, u32 h, const u8* pixels, const char* debug_name );

/* Re-upload the full atlas (same w/h as gui_atlas_create).  For CPU-side content that changes
   after creation -- the icon atlas's incremental packing -- not a resize. */
void gui_atlas_upload( gui_atlas_t* a, const u8* pixels );

/* Unregister the bindless slot (if any) and destroy the GPU texture (if valid), then zero `a`.
   Safe to call on an already-zeroed / partially-created atlas. */
void gui_atlas_destroy( gui_atlas_t* a );

/*============================================================================================*/
