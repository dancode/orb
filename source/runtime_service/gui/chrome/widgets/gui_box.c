/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_box.c -- the BOX decorator.

    A surface behind a RUN of widgets, sized to what they turned out to be.  Everything the
    library could put a styled surface under before this was a THING -- a widget's own face, a
    window body, a child region -- because a surface needs a rect and only a thing has one.  A
    group of widgets is not a thing; it is whatever the caller emitted between two calls, and
    its rect does not exist until the second one.

    So a caller who wanted a card behind three widgets had two bad options: hand-compute the
    rect (which breaks the moment a label changes), or open a child_begin -- a scroll region,
    with a scroll link, a clip, an id scope, and a full layout frame -- to decorate three
    widgets.  box_begin / box_end is the decorator that was missing between them: it owns no
    region, no clip, and no scroll, and costs one keyed float.

    WHY IT PAINTS BEFORE IT KNOWS ITS SIZE.  Painting order is emit order, so the surface must
    be pushed BEFORE the content it sits behind -- at which point the content has not been
    emitted and its height is unknown.  Three of the four numbers are known anyway: the box
    spans the content column, so x and w are exact, and the top is the pen.  Only the HEIGHT
    comes from measured feedback, exactly as the region content extent, autosize windows and
    natural tracks already do.  The alternative -- reserving command slots and patching them at
    box_end -- buys one frame of accuracy and costs an emit-redirection mode in the draw list,
    which is a large machine to own for a single scalar.

    That one frame is what GUI_VAR_ANIM_SIZE is for: the box paints the damped height, so a box
    whose content grows is seen GROWING rather than seen wrong once.  The layout always reserves
    max(painted, measured), so a box mid-glide never overlaps what follows it in either
    direction.  Rate 0 restores the plain one-frame lag with no branch.

    Compiled in the CHROME unit.  STACK and COLUMNS only: a grid carries a pre-resolved matrix
    that a mid-iteration inset would invalidate, and a pack is a horizontal print run whose
    content end is not a pen_y at all -- "how tall did that turn out" is not a question either
    one answers.  Both degenerate to a no-op scope rather than measuring something false.

==============================================================================================*/
// clang-format off

#define GUI_BOX_DEPTH  8    /* nested decorators; deeper than any real card-in-card design */

/* Anim channel tag for the height damper, kept off the measure's own id. */
#define BOX_ANIM_TAG   0xB0C1u

/* The whole retained state of a box: last completed frame's height, keyed by the box's id.  A
   box that stops being emitted ages out of the pool on its own. */
typedef struct { f32 h; } box_measure_t;

typedef struct
{
    gui_id_t id;        // keys the measure + the damper
    f32      box_x;     // the box's own column (the content column as it was at begin)
    f32      box_w;
    f32      top;       // CANVAS: the box's top edge
    f32      pad;       // inset applied on all four sides
    f32      used_h;    // the height actually PAINTED this frame (damped)
    bool     live;      // false when the scope degenerated -- end restores nothing

} box_frame_t;

static box_frame_t s_box[ GUI_BOX_DEPTH ];
static u32         s_box_sp;

/*==============================================================================================
    box_begin / box_end
==============================================================================================*/

void
gui_box_begin( const char* label, gui_style_role_t role )
{
    /* Cap the write slot at the top of the stack (the layout-frame rule) so an over-deep nesting
       aliases the deepest frame rather than writing past the array; sp still counts true, so the
       matching ends still pair up. */
    u32 slot = ( s_box_sp < GUI_BOX_DEPTH ) ? s_box_sp : GUI_BOX_DEPTH - 1;
    ++s_box_sp;

    box_frame_t* b = &s_box[ slot ];
    *b = ( box_frame_t ){ 0 };

    if ( s_box_sp > GUI_BOX_DEPTH )
    {
        GUI_WARN_ONCE( "box_begin nested past GUI_BOX_DEPTH (%u) -- '%s' draws no surface\n",
                       (u32)GUI_BOX_DEPTH, label ? label : "?" );
        return;
    }

    layout_frame_t* f = lf();
    if ( f->mode != GUI_MODE_STACK && f->mode != GUI_MODE_COLUMNS )
        return;   /* see the banner: neither a grid nor a pack has a height to be measured */

    layout_row_break( f );

    gui_id_t id   = item_id( label );
    f32      pad  = WIDGET_PAD;
    f32      top  = layout_next_y( f );
    f32      rate = style_var( GUI_VAR_ANIM_SIZE );

    /* Last frame's measure is the target; the damper is armed only when the theme asks for one.
       anim_TRACK, not anim_f32: a box height sits still for thousands of frames between edits,
       and the resting damper drops its slot -- which would leave nothing to ease from at the
       one moment that matters (see gui_anim_track in core/gui_anim.c). */
    f32 want = GUI_STATE( box_measure_t, id )->h;
    f32 used = ( rate > 0.0f ) ? gui_anim_track( id_combine( id, BOX_ANIM_TAG ), want, rate ) : want;

    /* Nothing measured yet -- a first appearance paints no surface for exactly one frame, then
       the box_end below stores a height and the next frame has one. */
    if ( used > 0.0f )
        draw_face_frame( ( gui_rect_t ){ f->content_x, top, f->content_w, used },
                         (u8)role, GUI_PHASE_IDLE,
                         style_col( GUI_ROLE_BORDER, GUI_PHASE_IDLE ), WIN_BORDER );

    b->id     = id;
    b->box_x  = f->content_x;
    b->box_w  = f->content_w;
    b->top    = top;
    b->pad    = pad;
    b->used_h = used;
    b->live   = true;

    /* The content starts one pad inside the box, in a column narrowed by a pad on each side.
       pen_jump also clears gap_pending: the box consumed the gap it was owed. */
    layout_pen_jump( f, top + pad );
    layout_inset( pad, pad );

    id_push( id );   /* the box is a naming scope, like a child -- two boxes may hold an "ok" */
}

void
gui_box_end( void )
{
    if ( s_box_sp == 0 )
    {
        GUI_WARN_ONCE( "box_end with no open box -- unbalanced begin/end\n" );
        return;
    }
    --s_box_sp;

    box_frame_t* b = &s_box[ ( s_box_sp < GUI_BOX_DEPTH ) ? s_box_sp : GUI_BOX_DEPTH - 1 ];
    if ( !b->live )
        return;

    id_pop();

    layout_frame_t* f = lf();
    layout_row_break( f );

    /* pen_y is carried at the exact content end (no trailing gap), so the measure is the span
       from the box top to it, plus the bottom pad.  An empty box is still two pads tall -- a
       decorator that collapses to nothing is a decorator that looks broken. */
    f32 measured = ( f->pen_y - b->top ) + b->pad;
    f32 floor_h  = b->pad * 2.0f;
    if ( measured < floor_h ) measured = floor_h;

    GUI_STATE( box_measure_t, b->id )->h = measured;

    layout_inset( -b->pad, -b->pad );

    /* Reserve the LARGER of what was painted and what was measured.  Growing, the layout is
       already at the final size while the surface catches up; shrinking, the space stays held
       until the surface has left it.  Either way nothing below the box is ever drawn over. */
    f32 bottom = b->top + ( ( b->used_h > measured ) ? b->used_h : measured );
    layout_pen_jump( f, bottom );
    extent_track( f, b->box_x + b->box_w, bottom );
    f->gap_pending = true;   /* the next line owes a gap after the box, as after any line */

    /* The surface painted a stale size -- come back and paint the right one.  The damper already
       pins redraw while it is moving; this is what makes the UNDAMPED path (rate 0) converge,
       since nothing else would ask for the frame that corrects it. */
    if ( fabsf( measured - b->used_h ) >= 0.5f )
        g_ctx->retained.wants_redraw = true;
}

// clang-format on
/*============================================================================================*/
