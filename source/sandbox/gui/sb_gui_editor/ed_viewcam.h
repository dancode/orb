#ifndef ED_VIEWCAM_H
#define ED_VIEWCAM_H
/*==============================================================================================

    sandbox/gui/sb_gui_editor/ed_viewcam.h -- First-class editor viewport camera controller.

    UE-style scene navigation packaged as a reusable, input-agnostic unit: the host viewport
    snapshots whatever input source it has (gui io, raw app events, replayed input) into a
    viewcam_input_t each frame and calls viewcam_update.  The controller owns the full gesture
    grammar and the motion dynamics; it never touches gui(), app(), or editor state, so any
    viewport that can capture the mouse can drive one.

    Gesture grammar (Unreal reference):

        RMB drag              look around (eye pinned, aim turns)
        LMB/RMB held + keys   fly camera-relative, accelerated (Shift = boost, wheel = fly
                              speed) -- any non-Alt capture, chords included
        RMB + LMB drag        pan (track) in the view plane
        MMB drag              pan
        LMB drag              drive: horizontal turns level, vertical walks the ground plane
        LMB click (no drag)   reported via .click / .click_x/y -- the host's "select" hook
        Alt + LMB drag        orbit the focus point
        Alt + RMB drag        dolly toward/away from the focus point
        wheel                 zoom the orbit distance

    Feel: fly velocity eases toward the wish direction (accel) and damps to rest on release
    (decel) instead of stepping, and rotational deltas pass through a light exponential
    smoother -- both frame-rate independent.  All rates/sensitivities are public fields,
    seeded by viewcam_init.

    Pose is the orbit parameterization (target + dist + yaw/pitch); flying and driving move
    the focus point so the offset rides along, which keeps orbit/zoom sensible afterwards.

==============================================================================================*/

#include "orb.h"

// clang-format off

/* One frame of input, resolved by the caller (which keys map to which move axis is host
   policy -- WASD, arrows, or both). */
typedef struct
{
    f32  dt;                      // frame delta seconds
    f32  mouse_x, mouse_y;        // cursor in any consistent space (screen or panel-local)
    f32  wheel;                   // scroll ticks this frame
    bool hovered;                 // cursor over the viewport and interactable
    bool lmb, rmb, mmb;           // buttons held
    bool lmb_clicked, rmb_clicked, mmb_clicked;   // press edges this frame
    bool alt, shift;
    bool key_fwd, key_back, key_left, key_right, key_up, key_down;

} viewcam_input_t;

typedef struct
{
    /* pose -- read these to build the view matrix (viewcam_eye for the eye point) */
    f32  yaw;                     // radians around Y
    f32  pitch;                   // radians; clamped shy of the poles
    f32  dist;                    // orbit distance from the focus point
    f32  target[ 3 ];             // focus point
    f32  fov_deg;                 // field of view in degrees

    /* tuning -- viewcam_init seeds these; hosts override freely */
    f32  look_sens;               // rad per pixel, RMB look
    f32  orbit_sens;              // rad per pixel, Alt+LMB orbit
    f32  drive_sens;              // rad per pixel, LMB drive turn
    f32  drive_move;              // fraction of dist walked per pixel, LMB drive
    f32  pan_scale;               // fraction of dist tracked per pixel
    f32  dolly_scale;             // fractional dist change per pixel
    f32  fly_speed;               // units/sec base fly speed (wheel-tuned while flying)
    f32  fly_speed_min;
    f32  fly_speed_max;
    f32  fly_boost;               // Shift multiplier
    f32  fly_accel;               // 1/sec ease rate toward the wish velocity
    f32  fly_decel;               // 1/sec damp rate back to rest
    f32  look_smooth;             // 1/sec rotational-delta smoothing; 0 = raw
    f32  min_dist, max_dist;      // orbit distance clamp
    bool pan_invert;              // flip pan back to grab-the-world (drag moves the scene)

    /* dynamics */
    f32  vel[ 3 ];                // world-space fly velocity (persists so release glides out)
    f32  sdx, sdy;                // smoothed rotational delta

    /* gesture bookkeeping */
    bool cap_l, cap_r, cap_m;     // buttons captured by the camera
    bool l_moved;                 // LMB exceeded the click threshold (drag, not click)
    bool l_chord;                 // LMB chorded with RMB or flew with keys (suppresses click)
    bool l_alt;                   // alt held at LMB press (orbit gesture, suppresses click)
    f32  press_x, press_y;        // LMB press point (click threshold)
    f32  last_x, last_y;          // previous cursor position while captured

    /* per-frame outputs, valid after viewcam_update */
    bool capturing;               // a camera gesture owns the mouse this frame
    bool flying;                  // fly keys are live this frame (host may hide the cursor, etc.)
    bool click;                   // LMB click-select happened this frame ...
    f32  click_x, click_y;        // ... at this position

} viewcam_t;

void viewcam_init  ( viewcam_t* c );                                  // pose + tuning defaults
void viewcam_update( viewcam_t* c, const viewcam_input_t* in );
void viewcam_eye   ( const viewcam_t* c, f32 out_eye[ 3 ] );
void viewcam_basis ( const viewcam_t* c, f32 fwd[ 3 ], f32 right[ 3 ], f32 up[ 3 ] );

// clang-format on
/*============================================================================================*/
#endif    // ED_VIEWCAM_H
