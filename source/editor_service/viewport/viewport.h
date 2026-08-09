#ifndef ED_VIEWPORT_H
#define ED_VIEWPORT_H
/*==============================================================================================

    editor_service/viewport/viewport.h -- Editor SCENE VIEWPORT (ed_viewport).

    The docked window that displays the running game: an offscreen render target owned by
    the render module (render()->target_*), shown through gui()->image_texture.  This is
    the play-in-editor surface -- the host hands the target id to the project as
    run_view_t.render_ctx in place of the main window's swapchain context.

    Naming: "scene viewport" (this) is distinct from gui's window viewport (a vp handle,
    the OS window gui owns for docking/floaters).  Never bare "viewport" in new identifiers.

    Plain static lib, direct-call -- no vtable, no module identity.  It talks to static
    service gateways (gui) and the shared render pointer the runtime host populates, so
    there is nothing to bind or fetch.  The editor service drives it:

        ed_viewport_update( live )   host on_update, AFTER game()->tick fills the target
                                     bucket, BEFORE the gui emit bakes the texture index
        ed_viewport_panel( &open )   inside the gui build
        ed_viewport_render_ctx()     the id for run_view_t.render_ctx; -1 until sized

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

/* Maintain the target between frames: lazy create once the panel publishes its first
   size, settle-debounced recreate on resize, and -- on frames that will draw (live) --
   flip the write buffer so in-flight frames keep sampling the previous one.
   Returns true when the target was (re)created this frame: the caller must force a gui
   rebuild so the panel re-bakes the new bindless index -- a clean-frame replay of the
   retained draw list would keep sampling the old one past its deferred destruction. */
bool ed_viewport_update( bool live );

/* The scene viewport window: publishes its content size as the wanted target size and
   displays the target's current buffer.  Visibility is the caller's (Window menu). */
void ed_viewport_panel( void );

/* Target id for run_view_t.render_ctx; -1 while no target exists (panel unsized). */
i32  ed_viewport_render_ctx( void );

/* Current target size in pixels for run_view_t.surface_w/h; (0,0) while none. */
void ed_viewport_surface( i32* w, i32* h );

/* Release the target (render()->target_destroy).  Editor service exit path. */
void ed_viewport_shutdown( void );

/*============================================================================================*/
#endif    // ED_VIEWPORT_H
