/*==============================================================================================

    runtime_service/gui/stock/gui_adornment.c -- Per-item ambient application + the system
    adornments.

    Everything here is styled paint, or the application of style / draw state -- stock material
    by definition, since stock is the first layer astride both servers.  Three groups, in file
    order:

      - per-item ambient application (item_flags_resolve / item_flags_chrome_reset): the style
        and draw consequences over the interact server's pure seams (item_flags_take /
        item_flags_chrome_drop, core/gui_ctx.c);
      - the label paint (label_natural_w + gui_field_row): gui_field_row draws a labeled widget's
        own label per the ambient gui_field_t.  Its geometry half (field_geom_split) lives with
        the composer in flow -- the paint and the WIDGET_PAD self-measure are here;
      - the system adornments (nav ring, focus ring, drop ring, child box, resize highlight):
        the units below decide WHEN one paints, across their documented upward seams
        (core/gui_core.h, interact/gui_interact.h, flow/gui_flow.h), and the paint POLICY --
        color, thickness, extent -- lives here with the rest of the skin.

    The style vocabulary this consumes (WIDGET_* / WIN_* / COL_*) and the state -> color
    projections (col_item_bg & co) both live with their resolver in the style unit; state -> color
    is resolution by nature, and all three take the interact state as a parameter.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Per-item ambient application -- the impure wrappers over the interact server's pure seams.

    The core unit resolves WHAT the flags are (item_flags_take / item_flags_chrome_drop,
    core/gui_ctx.c); these wrappers apply the style and draw consequences -- the per-item style
    commit, the disabled dim, the default rounding -- which the interact server itself must
    never touch.  Style owns the commit, stock the adornment.
==============================================================================================*/

/* Resolve the flags for the item now emitting, then apply their ambient consequences.  Called
   once per item from cell_next_w (the universal emit seam). */
gui_item_flags_t
item_flags_resolve( void )
{
    gui_item_flags_t f = item_flags_take();

    /* Same seam for the style stacks: promote any next_style_* override into the active per-item
       layer so it applies for this widget's whole draw, then clears for the following one. */
    style_item_commit();

    /* The disabled dim, read through the style like every other value: a kit can soften it, and
       push_style_var scopes it (the rest of the dim is the draw list's global alpha). */
    draw_set_alpha( ( f & GUI_ITEM_DISABLED ) ? DISABLED_ALPHA : 1.0f );
    /* Default this widget's rects to the control-frame radius (base + push/pop + next-* override).
       A widget that draws a grab (slider knob, scrollbar) or a squared-off mark (check, bullet)
       overrides draw_set_rounding locally for that sub-element. */
    draw_set_rounding( ROUND_WIDGET );
    return f;
}

/* Clear the per-item state before chrome runs.  Window/child borders, scrollbars, titlebars, and
   the collapse arrow are not items -- they never go through cell_next_w, so without this they
   would inherit whatever the last body widget latched (a disabled trailing widget would dim the
   border and deaden the scrollbar).  Called at the chrome seams (begin/window_end, child_begin,
   layout_pop_region) so chrome always interacts undisabled and paints opaque. */
void
item_flags_chrome_reset( void )
{
    item_flags_chrome_drop();
    draw_set_alpha( 1.0f );
    style_chrome_reset();   /* drop lingering next_style_* overrides; keep the push/pop stack */
    /* Chrome (window / child / dock backgrounds, title bars, borders) defaults to the window radius,
       read after the item override is cleared so a trailing widget's next-* radius cannot leak in. */
    draw_set_rounding( style_var( GUI_VAR_PANEL_ROUND ) );
}

/*==============================================================================================
    Label paint -- the self-measure, and the one caption seam.
==============================================================================================*/

/* The natural width a label-sized widget requests from the composer: the visible span plus the
   standard inset on both sides.  THE self-measurement formula -- button, small_button, menu
   items, and the public gui_button_width all speak it through this one helper.  WIDGET_PAD
   is a style read, so it lives with the styled painters. */
f32  label_natural_w( const char* s )
{
    return label_width( s ) + 2.0f * WIDGET_PAD;
}

/* The caption label seam -- the ONE place a labeled value widget routes its own label through, so
   there is no second "_label" widget set: the label param a widget already carries becomes the
   label, and the ambient gui_field_t (gui_field_get) decides whether and where it renders.  Reads
   the ambient, consumes one WIDGET_H row cell, paints the label into its track, and arms
   next_item_rect with the control track so the widget's own cell_next fills it and keeps the hit --
   the label is passive text.  A widget calls it right before taking its cell:

       bool gui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
       { gui_field_row( label ); ... id = item_id(label); cell = cell_next(WIDGET_H); ... }

   It emits nothing (consumes no cell, arms nothing -- the widget's cell_next then returns a full
   cell) in three cases, so one call handles labeled AND bare: field.hide (the master property-panel
   toggle), skip_label() armed for this one widget, or an empty ("##id") label.  The geometry half
   (field_geom_split) lives with the composer in flow; this owns the PAINT and the pen -- flow
   never colors a pixel. */

void
gui_field_row( const char* label )
{
    gui_field_t* fld   = gui_field_get();   /* the one field authority (set-once, like a style) */
    f32          vis_w = label_width( label );
    bool         skip  = field_skip_take();  /* always consume the one-shot, even when hidden */
    if ( fld->hide || skip || vis_w == 0.0f ) return;   /* bare: control fills its cell */

    gui_rect_t cell = cell_next( WIDGET_H );

    /* Natural label track = the measured width; an explicit field.label (LEFT/RIGHT column) wins. */
    f32 label_track = ( fld->side != GUI_LABEL_NONE && fld->label > 0.0f ) ? fld->label : vis_w;

    gui_rect_t label_r, control_r;
    field_geom_split( cell, (gui_label_side_t)fld->side, fld->control > 0.0f ? fld->control : 1.0f,
                      label_track, WIDGET_MIN_W, WIDGET_PAD, &label_r, &control_r );

    draw_label_fit( label_r.x, text_center_y( cell.y, cell.h ), COL_TEXT_PRIMARY_IDLE, label, label_r.w );
    gui_next_item_rect( control_r );
}

/*==============================================================================================
    System adornments -- the uniform highlight rings and edge markers the interaction services
    invoke.  Behavior (interact/) decides WHEN one paints (the protocol point); the paint
    policy -- color, thickness, extent -- lives here with the rest of the skin, so the behavior
    tier never reads a style value to adorn an item.
==============================================================================================*/

/* THE RING -- the one shape both keyboard signals wear.

   Two things can say "the keyboard is here": the nav cursor (ring_mark_nav) and text/value focus
   (ring_mark_focus).  They are different states and carry different colours, but they are the same
   geometry -- a FOCUS_RING-thick stroke ON the item rect, drawn inward from its edge.  Both are
   declared in core/gui_core.h beside their seams.

   It rests on the rect rather than standing off it because an item's rect is the only space it
   owns.  A stroke offset outward has to be paid for in whitespace around every focusable thing:
   it bleeds past a title bar whose buttons sit flush to the edge, it is clipped where a full-width
   field meets a region edge or a table cell, and a dense editor panel has no gap to spare for it.
   Inward, the geometry is unconditional -- the ring cannot be clipped by anything that was not
   already clipping the item, and it costs no layout.  Widgets carry their own interior padding
   (WIDGET_PAD), so the stroke lands on the frame and the outermost pixels of fill, not on
   content.

   Resting inward is only possible because a ring PAINTS LATE (rings_paint below).  Drawn where it
   is decided -- mid-item, before the widget fills itself -- the fill would cover it, which is what
   pushed the old cursor ring outside the rect to begin with. */

/* The stroke itself.  `round` is the item's own corner radius; the ring shares it exactly, since
   it lies on the same boundary.  The ambient radius is saved and restored -- the caller is
   mid-paint with its own. */
static void
draw_ring( gui_rect_t r, f32 round, u32 col )
{
    f32 t = FOCUS_RING;
    if ( t <= 0.0f ) return;   /* theme opt-out: no ring at any thickness */

    f32 keep = draw_rounding();

    draw_set_rounding( round );
    draw_push_rect_outline( r.x, r.y, r.w, r.h, t, col );
    draw_set_rounding( keep );
}

/* A ring is MARKED where it is decided and PAINTED at the end of the window body, not in place.

   Both are decided mid-item -- before the widget has drawn itself, and before the neighbours,
   region grounds and row fills that reach over its edge.  Emitted at the decision point, a ring on
   the item rect goes under all of it and disappears.  So the decision only stamps the rect, the
   interaction clip it was made under, and the item's radius, and rings_paint (gui_window_end, just
   before the body region closes) lays both down last, each back under its own clip so a ring on an
   item inside a scrolled child is still bounded by that child's view.

   One of each per frame is all there can be: one nav cursor, one focused item.  A mark is always
   consumed by the end of the window that made it, since an item can only be marked between a
   window_begin and its window_end. */

typedef struct
{
    gui_rect_t r;         // item rect the ring lies on
    gui_rect_t clip;      // s_scope.clip where it was marked -- re-pushed to paint it
    f32        round;     // the item's own corner radius, which the ring shares
    bool       set;       // a mark is pending
    bool       captured;  // nav only: the keyboard is captured for value editing

} ring_mark_t;

static ring_mark_t s_ring_nav;
static ring_mark_t s_ring_focus;

/* The radius comes from the ambient at MARK time -- the item's own (item_flags_resolve set it to
   ROUND_WIDGET); by paint time chrome has reset the ambient to the panel radius. */
static void
ring_mark( ring_mark_t* m, gui_rect_t r, bool captured )
{
    m->r        = r;
    m->clip     = s_scope.clip;
    m->round    = draw_rounding();
    m->captured = captured;
    m->set      = true;
}

/* Keyboard-nav cursor ring (nav_item_register invokes it across the core unit's one paint seam --
   see the upward-seam block in core/gui_core.h).  captured selects the colour: plain
   nav-highlight (COL_MARK_HOT) vs. a value widget that has captured the keyboard for Left/Right
   editing (COL_MARK_ACTIVE) -- this is the one adornment every widget passes through, so it is
   the single place that makes "input captured" read as a real, theme-wide-consistent state change
   instead of looking identical to plain nav focus.  Opaque, unlike the focus ring: a cursor is
   something you hunt for on screen. */
void
ring_mark_nav( gui_rect_t r, bool captured )
{
    ring_mark( &s_ring_nav, r, captured );
}

/* Keyboard-focus ring: the ring around the item that owns the keyboard right now -- the caret's
   field, a drag box in text-entry mode.  item_state marks it the moment focus resolves
   (core/gui_item.c), so every focusable widget gets the same signal without spelling it.

   BORDER[ACTIVE] is the hue, held back to FOCUS_RING_ALPHA so a stroke twice the frame's width
   does not land with twice its weight.  The alpha is a fixed signal constant, not a theme ramp,
   for the BAKE_DROP_WASH reason: a theme is free to run its ramps near zero, and a cue that says
   where typing goes has to survive that. */
#define FOCUS_RING_ALPHA 0.70f

void
ring_mark_focus( gui_rect_t r )
{
    ring_mark( &s_ring_focus, r, false );
}

static void
ring_paint( ring_mark_t* m, u32 col )
{
    if ( !m->set ) return;
    m->set = false;

    draw_push_clip_rect( m->clip.x, m->clip.y, m->clip.w, m->clip.h );
    draw_ring( m->r, m->round, col );
    draw_pop_clip_rect();
}

/* Lay down whatever this window marked.  Focus first, cursor over it: when one item is both, the
   cursor is the louder signal and wins the pixels. */
void
rings_paint( void )
{
    u32 focus = COL_BORDER_ACTIVE;
    focus = ( focus & 0x00FFFFFFu )
          | ( (u32)( (f32)( focus >> 24 ) * FOCUS_RING_ALPHA ) << 24 );

    ring_paint( &s_ring_focus, focus );
    ring_paint( &s_ring_nav, s_ring_nav.captured ? COL_MARK_ACTIVE : COL_MARK_HOT );
}

/* Drag-and-drop accept ring: a bolder outline around an open target whose type matched the
   payload, so the drop reads as "accepted here".  gui_drag_payload_accept (the interact unit)
   invokes it across that unit's one documented upward paint seam (interact/gui_interact.h) --
   the drop decision and its feedback land in the same call, like core's draw_nav_ring. */
void
draw_drop_ring( gui_rect_t r )
{
    draw_push_rect_outline( r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 2.0f, COL_MARK_HOT );
}

/* Drag-and-drop candidate hint: a thin outline around EVERY potential target the moment a
   compatible payload starts dragging, not just the one under the cursor -- so a user can see
   every place a drag could land before their cursor gets there.  Flush with the rect and one
   frame-line wide (WIN_BORDER) rather than draw_drop_ring's bold offset outline, and washed in
   the same GUI_EXT_DROP hue PANEL/PANEL_CHILD/TITLE's own drop-target HOT reads toward
   (gui_bake.c), so "you can drop here" is one signal at every scale, graduating from this thin
   candidate ring to draw_drop_ring's bold accepted one as the cursor arrives.  gui_drag_hint
   (interact/gui_drag.c) invokes it across that unit's drop-paint seam. */
void
draw_drop_hint( gui_rect_t r )
{
    draw_push_rect_outline( r.x, r.y, r.w, r.h, WIN_BORDER, style_ext( GUI_EXT_DROP ) );
}

/* Child box chrome (flow/gui_layout_child.c invokes these around its region): the body
   fill under the region clips at child_begin, the border over the bar tracks at child_end. */
void draw_child_bg( gui_rect_t r, u8 phase ) { draw_face( r, GUI_ROLE_PANEL_CHILD, phase ); }

/* The child's own border, one of three reads (child_end passes back the SAME phase child_begin's
   body fill used, plus whether GUI_WIN_DRAG_TARGET was set -- see backdrop_phase,
   flow/gui_flow.h): phase HOT bolds it into the drop-accepted ring (draw_drop_ring's geometry, but
   the GUI_EXT_DROP hue instead of MARK -- this is the same "you can drop here" signal PANEL_CHILD's
   own HOT wash is already carrying on the fill, not the widget-level accept ring's family); not
   hot but still a live drag candidate gets the thin ambient hint (draw_drop_hint) so a
   GUI_WIN_DRAG_TARGET child reads as a candidate the moment a compatible drag starts, not only
   once hovered; otherwise the plain idle frame. */
void
draw_child_border( gui_rect_t r, u8 phase, bool drag_candidate )
{
    if ( phase == GUI_PHASE_HOT )
        draw_push_rect_outline( r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 2.0f, style_ext( GUI_EXT_DROP ) );
    else if ( drag_candidate )
        draw_drop_hint( r );
    else
        draw_outline( r, WIN_BORDER, COL_BORDER_IDLE );
}

/* Paint a bold line over each hot edge of an outline so it is obvious that the border is
   grabbable and which side will move.  Drawn just inside the rect, over the thin border.
   `edges` is the GUI_RESIZE_* mask from the edge-resize service (interact/gui_resize.c).

   A single-edge drag (L, R, T, or B alone) highlights only the STRAIGHT run of that side, inset
   `radius` from each end so it never enters a corner arc it has no business curving through.  A
   corner drag holds two adjacent edges at once (e.g. L+T for the top-left grip); there the two
   straight runs stop short at the same inset, and the gap between them is bridged by the corner
   arc itself, so the highlight reads as one continuous line sweeping around the grabbed corner
   instead of two separate strips pointing at it.

   The bridge reuses the border ring's own shape (draw_push_rect_outline, same ambient radius as
   the window) rather than drawing an arc by hand: clipped down to just that corner's radius x
   radius box, the only part of the ring inside the box IS the corner's quarter-circle, so the
   clip picks it out precisely -- no separate curve math to keep in sync with the border's. */
void
draw_resize_highlight( gui_rect_t r, u8 edges )
{
#if !GUI_RESIZE_HIGHLIGHT

    (void)r; (void)edges;

#else

    const f32 t      = WIN_BORDER * 2.0f + 1.0f;   /* bold relative to the 1px frame */
    const f32 radius = draw_rounding();
    const f32 inset  = ( 2.0f * radius < r.w && 2.0f * radius < r.h ) ? radius : 0.0f;

    bool l = ( edges & GUI_RESIZE_L ) != 0, rt = ( edges & GUI_RESIZE_R ) != 0;
    bool tp = ( edges & GUI_RESIZE_T ) != 0, b = ( edges & GUI_RESIZE_B ) != 0;

    /* Straight runs: strictly between the two corner insets, so they never invade an arc. */
    if (  l ) draw_push_rect_filled( r.x,           r.y + inset,   t,           r.h - 2.0f * inset, 0,0,1,1, 0, COL_BORDER_HOT );
    if ( rt ) draw_push_rect_filled( r.x + r.w - t, r.y + inset,   t,           r.h - 2.0f * inset, 0,0,1,1, 0, COL_BORDER_HOT );
    if ( tp ) draw_push_rect_filled( r.x + inset,   r.y,           r.w - 2.0f * inset, t,           0,0,1,1, 0, COL_BORDER_HOT );
    if (  b ) draw_push_rect_filled( r.x + inset,    r.y + r.h - t, r.w - 2.0f * inset, t,           0,0,1,1, 0, COL_BORDER_HOT );

    /* Corner bridges: only where both adjoining edges are hot, and only when there is an arc
       to trace (a flush window's inset collapses to 0, leaving the straight runs meet square). */
    if ( inset > 0.0f )
    {
        if ( l  && tp ) { draw_push_clip_rect( r.x,             r.y,             radius, radius ); draw_push_rect_outline( r.x, r.y, r.w, r.h, t, COL_BORDER_HOT ); draw_pop_clip_rect(); }
        if ( rt && tp ) { draw_push_clip_rect( r.x + r.w - radius, r.y,             radius, radius ); draw_push_rect_outline( r.x, r.y, r.w, r.h, t, COL_BORDER_HOT ); draw_pop_clip_rect(); }
        if ( l  &&  b ) { draw_push_clip_rect( r.x,             r.y + r.h - radius, radius, radius ); draw_push_rect_outline( r.x, r.y, r.w, r.h, t, COL_BORDER_HOT ); draw_pop_clip_rect(); }
        if ( rt &&  b ) { draw_push_clip_rect( r.x + r.w - radius, r.y + r.h - radius, radius, radius ); draw_push_rect_outline( r.x, r.y, r.w, r.h, t, COL_BORDER_HOT ); draw_pop_clip_rect(); }
    }

#endif
}

// clang-format on
/*============================================================================================*/
