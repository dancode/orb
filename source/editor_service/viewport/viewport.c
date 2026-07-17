/*==============================================================================================

    editor_service/viewport/viewport.c -- Editor scene viewport implementation.

    Ported from the sandbox proof (sb_gui_editor/ed_viewport.c) minus the editor camera
    and picking -- the game renders itself; this unit only owns the panel/target plumbing.
    The render module owns the GPU resources (render()->target_*); this unit owns the
    want-size/settle protocol and the gui window.

    Resize protocol: the panel publishes want_w/h each build; update() recreates the
    target once the request has been stable for ED_VIEWPORT_SETTLE frames, so a live
    resize drag stretches the old image instead of thrashing GPU allocations.

==============================================================================================*/

#include "orb.h"
#define LOG_CH "ed_viewport"

#include "engine/core/core_api.h"
#include "runtime_service/gui/gui_api.h"
#include "runtime_modules/render/render_api.h"
#include "editor_service/viewport/viewport.h"

/*============================================================================================*/

#define ED_VIEWPORT_MIN    16      /* below this the panel is collapsed/hidden: keep target */
#define ED_VIEWPORT_MAX    4096
#define ED_VIEWPORT_SETTLE 8       /* frames the wanted size must hold before recreating    */

typedef struct ed_viewport_state_s
{
    i32 target_id;        /* render target id, or -1                    */
    i32 want_w, want_h;   /* panel content size published by the panel  */
    i32 stable_frames;    /* settle counter toward a pending resize     */

} ed_viewport_state_t;

static ed_viewport_state_t s_vp = { .target_id = -1 };

/*============================================================================================*/

bool
ed_viewport_update( bool live )
{
    bool recreated = false;

    if ( !render() )
        return false;

    i32 w = s_vp.want_w, h = s_vp.want_h;
    if ( w >= ED_VIEWPORT_MIN && h >= ED_VIEWPORT_MIN )
    {
        if ( w > ED_VIEWPORT_MAX ) w = ED_VIEWPORT_MAX;
        if ( h > ED_VIEWPORT_MAX ) h = ED_VIEWPORT_MAX;

        if ( s_vp.target_id < 0 )
        {
            s_vp.target_id = render()->target_create( w, h );
            if ( s_vp.target_id >= 0 )
            {
                LOG_INFO( "scene target created %dx%d", w, h );
                recreated = true;
            }
        }
        else
        {
            i32 tw = 0, th = 0;
            render()->target_size( s_vp.target_id, &tw, &th );

            if ( w == tw && h == th )
                s_vp.stable_frames = 0;
            else if ( ++s_vp.stable_frames >= ED_VIEWPORT_SETTLE )
            {
                /* target_resize waits for the device to idle; the settle gate above
                   keeps that hitch off live drag frames. */
                if ( render()->target_resize( s_vp.target_id, w, h ) )
                {
                    LOG_INFO( "scene target resized %dx%d", w, h );
                    recreated = true;
                }
                else
                    s_vp.target_id = -1;
                s_vp.stable_frames = 0;
            }
        }
    }

    /* Flip only on frames whose scene pass will write (a live session submitted this
       frame): in-flight frames keep sampling the untouched previous buffer, and on idle
       frames the panel's command hash stays stable so the gui retained cache can clean. */

    if ( live && s_vp.target_id >= 0 )
        render()->target_flip( s_vp.target_id );

    return recreated;
}

void
ed_viewport_panel( void )
{
    /* DOCK_MAXIMIZE: while docked, the tab strip offers a maximize button (double-click the
       strip's empty band too) that pins the pane over the whole dockspace -- fullscreen scene
       view vs the tiled editor layout.  The panel body is size-agnostic (it publishes whatever
       content_avail hands it), so the target resize protocol below rides the transition as it
       would any resize. */
    if ( !gui()->window_begin( "Viewport", GUI_WIN_NOSCROLL | GUI_WIN_DOCK_MAXIMIZE | GUI_WIN_CLOSEABLE ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    gui_vec2_t avail = gui()->content_avail();
    s_vp.want_w = ( i32 )avail.x;
    s_vp.want_h = ( i32 )avail.y;

    u32 bindless = ( render() && s_vp.target_id >= 0 ) ? render()->target_texture( s_vp.target_id ) : 0;
    if ( bindless && s_vp.want_w >= ED_VIEWPORT_MIN && s_vp.want_h >= ED_VIEWPORT_MIN )
    {
        /* Sample the buffer this frame's scene pass writes (flipped in update); the
           other one belongs to the still-in-flight previous frame. */
        gui()->image_texture( bindless, avail.x, avail.y, 0 );
    }
    else
    {
        gui()->text_disabled( "scene target initializing..." );
    }

    gui()->window_end();
}

i32
ed_viewport_render_ctx( void )
{
    return s_vp.target_id;
}

void
ed_viewport_surface( i32* w, i32* h )
{
    if ( render() && s_vp.target_id >= 0 )
        render()->target_size( s_vp.target_id, w, h );
    else
    {
        if ( w ) *w = 0;
        if ( h ) *h = 0;
    }
}

void
ed_viewport_shutdown( void )
{
    if ( render() && s_vp.target_id >= 0 )
        render()->target_destroy( s_vp.target_id );
    s_vp.target_id = -1;
}

/*============================================================================================*/
