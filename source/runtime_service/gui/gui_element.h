#ifndef GUI_ELEMENT_H
#define GUI_ELEMENT_H
/*==============================================================================================

    runtime_service/gui/gui_element.h -- GUI_ELEMENT types: the slim element style.

    The S1 style stratum: the minimum a rect-consuming
    building block (el_*) needs -- 4 metrics + a 4x4 role/state palette.  Deliberately NO
    per-widget slots (btn_bg_hover, slot_border_hot, ...): per-widget color is either a call
    parameter (el_meter's fill) or a token in the kit above.  Elements read ONLY this struct;
    themes never reach them directly -- the chrome theme system (S2) COMPILES down into an
    installed gui_el_style_t whenever a theme lands, and an app kit (S3) may overwrite the
    installed values after that to own the look (see gui()->el_style).

==============================================================================================*/

#include "runtime_service/gui/rect/gui_rect.h"

// clang-format off

/* What the color is FOR.  Four roles cover the element vocabulary. */
typedef enum
{
    GUI_EL_BG = 0,        // surface fill (button body, check box, cycle end caps)
    GUI_EL_BORDER,        // frame line
    GUI_EL_TEXT,          // glyphs
    GUI_EL_ACCENT,        // emphasis: marks, value fills, highlights
    GUI_EL_ROLE_COUNT

} gui_el_role_t;

/* Which interaction state selects the color.  DIM doubles as the inert variant: a panel's
   backdrop fill is BG[DIM], an empty value track is ACCENT[DIM], secondary text is TEXT[DIM]. */
typedef enum
{
    GUI_EL_IDLE = 0,      // at rest
    GUI_EL_HOT,           // cursor over / keyboard nav on the item
    GUI_EL_ACTIVE,        // pressed / captured
    GUI_EL_DIM,           // inert, disabled, de-emphasized
    GUI_EL_STATE_COUNT

} gui_el_state_t;

/* The whole element style.  Metrics are px; line_h <= 0 means "the live active-font line
   height" (the ui_u basis), so a font swap rescales text-holding elements with no reinstall. */
typedef struct gui_el_style_t
{
    f32 pad;        // interior pad, all sides
    f32 gap;        // space BETWEEN slots (square-spacing rule: defaults equal to pad)
    f32 border_w;   // frame line width
    f32 line_h;     // text basis; <= 0 = active font line height

    u32 col[ GUI_EL_ROLE_COUNT ][ GUI_EL_STATE_COUNT ];

} gui_el_style_t;

// clang-format on
/*============================================================================================*/
#endif    // GUI_ELEMENT_H
