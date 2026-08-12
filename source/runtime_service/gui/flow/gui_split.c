/*==============================================================================================

    runtime_service/gui/flow/gui_split.c -- Side-by-side split panels.

    split_begin / split_next / split_end -- two panels side by side, sharing a Y-level.

    A split is a block-level widget from the parent flow's perspective: split_begin reserves a
    rect of height max(left_h, right_h) (from the previous frame's cache) and advances the
    parent cursor past it.  Inside, each panel runs its own independent flow via the transient
    sub-layout mechanism (split_push_panel, same pattern as push_layout -- sublayout_open lives
    in gui_sublayout.c, included just before this file).

    State: gui_split_entry_t in the keyed pool records the actual content height of each side
    last frame.  First frame seeds to WIDGET_H; settles correctly on frame 2.

    Nesting: up to GUI_SPLIT_DEPTH splits may be open simultaneously (independent of the
    layout stack depth since push/pop happen within begin/next/end, not across them).

    Included by gui_flow.c right after gui_sublayout.c, whose sublayout_open it pushes panels
    through.

==============================================================================================*/
// clang-format off

#define GUI_SPLIT_DEPTH 4

typedef struct
{
    gui_id_t   id;
    gui_rect_t left_rect;
    gui_rect_t right_rect;
    f32        measured_left_h;   /* actual height emitted in left panel (recorded at split_next) */
    f32        measured_right_h;  /* actual height emitted in right panel (recorded at split_end)  */
} gui_split_frame_t;

static gui_split_frame_t s_split_stack[ GUI_SPLIT_DEPTH ];
static u32               s_split_sp;

/* Push a transient sub-layout frame whose content area is rect -- the explicit-rect sub-layout
   (sublayout_open), shared with push_layout / push_layout_overlay.  split panels never scroll. */
static void
split_push_panel( gui_rect_t rect )
{
    sublayout_open( rect );
}

/* Pop the current panel and return the content height it actually emitted: commit the open line
   and measure through the anchor seam -- under gap-before, high_y is the exact content end, so the
   stored height feeds back stably (a button_fill that fills to it reclaims the same size next
   frame).  content_extent_y rather than a hand-rolled `high_y - origin_y`: a split panel opens
   through sublayout_open and never scrolls today, so the two agree, but the hand-rolled form is a
   cross-anchor subtraction that would silently measure short the day a panel does scroll. */
static f32
split_pop_panel( void )
{
    layout_frame_t* f = lf();
    layout_row_break( f );   /* close any partially-filled multi-column row */
    f32 h = content_extent_y( f );
    id_scope_unwind( f->id_restore );
    s_scope.clip = f->parent_clip;
    layout_frame_pop();
    return h;
}

void
gui_split_begin( const char* id_str, f32 right_w )
{
    layout_frame_t* parent = lf();
    layout_row_break( parent );            /* close any open row before the split */

    gui_id_t           id = id_combine( id_seed(), id_hash( id_str ) );
    DBG_NAME( id, id_str );
    gui_split_entry_t* se = GUI_STATE( gui_split_entry_t, id );

    /* Resolved height: max of both sides last frame.  Seed to one row on first appearance, then
       route the target through the size-animate seam (inert today, the animation hook for later). */
    f32 target_h = se->left_h > se->right_h ? se->left_h : se->right_h;
    if ( target_h < WIDGET_H ) target_h = WIDGET_H;
    f32 resolved_h = size_animate( target_h, GUI_ID_NONE, 0.0f );

    f32 gap    = WIDGET_GAP;
    f32 left_w = parent->content_w - right_w - gap;
    if ( left_w < 0.0f ) left_w = 0.0f;

    f32 x = parent->content_x;
    f32 y = layout_next_y( parent );   /* gap-before: the split band opens below prior content */

    gui_rect_t left_rect  = { x,                y, left_w,  resolved_h };
    gui_rect_t right_rect = { x + left_w + gap, y, right_w, resolved_h };

    /* Advance the parent past the split now so it can continue below after split_end: the pen
       lands at the band's exact bottom (the gap below is owed, not appended), and line.prev_item /
       the line record are stamped so same_line() after split_end anchors to the band. */
    parent->pen_y   = y + resolved_h;   /* pen past the band */
    if ( y + resolved_h > parent->high_y )
        parent->high_y = y + resolved_h;   /* highwater climbs to the band bottom (x unchanged) */
    parent->gap_pending = true;
    parent->line.prev_item   = ( gui_rect_t ){ x, y, parent->content_w, resolved_h };
    parent->line.cross  = y;
    parent->line.ext    = resolved_h;
    parent->line.open   = false;

    /* Push the split frame and open the left panel. */
    ORB_ASSERT( s_split_sp < GUI_SPLIT_DEPTH );
    if ( s_split_sp < GUI_SPLIT_DEPTH )
    {
        gui_split_frame_t* sf = &s_split_stack[ s_split_sp++ ];
        sf->id               = id;
        sf->left_rect        = left_rect;
        sf->right_rect       = right_rect;
        sf->measured_left_h  = 0.0f;
        sf->measured_right_h = 0.0f;
        split_push_panel( left_rect );
    }
}

void
gui_split_next( void )
{
    if ( s_split_sp == 0 ) return;
    gui_split_frame_t* sf = &s_split_stack[ s_split_sp - 1 ];

    sf->measured_left_h = split_pop_panel();
    split_push_panel( sf->right_rect );
}

void
gui_split_end( void )
{
    if ( s_split_sp == 0 ) return;
    gui_split_frame_t* sf = &s_split_stack[ --s_split_sp ];

    sf->measured_right_h = split_pop_panel();

    /* Persist both heights into the keyed pool for the next frame's pre-allocation. */
    gui_split_entry_t* se = GUI_STATE( gui_split_entry_t, sf->id );
    se->left_h  = sf->measured_left_h;
    se->right_h = sf->measured_right_h;
}

// clang-format on
/*============================================================================================*/
