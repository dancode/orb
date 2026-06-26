/*==============================================================================================

    runtime_service/gui/gui_popup.c -- Popups, context menus, and tooltips.

    A popup is a transient window rendered on top of everything that auto-closes on an outside
    click (regular) or blocks input + dims the background (modal).  The popup layer is thin: the
    open set is a stack (s_popups_open in gui_ctx.c), and rendering is delegated wholesale to the
    window path (window_begin_ex / window_end).  Occlusion, z-sort, clipping, scroll, and chrome
    all come from the window machinery unchanged.

    Lifecycle (mirrors Dear ImGui's OpenPopupStack / BeginPopupStack split):
        popup_open(id)         -- record id into the open stack at the current nesting depth
        popup_begin(id)        -- if id is the open popup at this depth, render it; return true
        popup_end()            -- close the window, pop the begin stack
        popup_close_current()  -- truncate the open stack to this popup's depth

    The one invariant that prevents most popup bugs: popup_close_check() runs at the very top of
    the frame (in gui_ctx_begin), BEFORE any user code -- so the click that opens a popup (in
    this frame's user code) can never be the click that closes it (checked next frame, by which
    time the button is released).  No "just opened" grace flags are needed; it falls out of order.

    A popup is begun while a parent window is still open, but it is a *top-level overlay*: it must
    lay out, clip, and paint independent of the parent.  overlay_detach / overlay_reattach snapshot
    and restore the cross-cutting state begin/window_end touch (see gui_overlay_save_t), and a
    root draw-clip is pushed so the popup escapes the parent window's bounds.

    Included by gui.c after gui_widget_window.c (so window_begin_ex is in scope) and before
    gui_api.c (so gui_ctx_begin can call popup_close_check / popup_apply_modal).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Constants
----------------------------------------------------------------------------------------------*/

/* GUI_POPUP_Z_BASE (the reserved high z-band) lives in gui_internal.h so window_is_native can
   read it too; popup_begin rewrites win->z every frame so a stray window_raise_on_press can never
   sink a popup. */

/* Popup window ids are salted off the caller's string so a popup never shares a record with a
   normal window of the same title. */
#define GUI_POPUP_SALT     0x504F5055u    /* 'POPU' */

/* Single reserved id for the (one, non-nesting) tooltip window. */
#define GUI_TOOLTIP_ID     0x700191D0u

/* Off-screen parking for the auto-size appearing frame (measured but invisible; see begin). */
#define GUI_POPUP_OFFSCREEN  (-32000.0f)

/* Seed geometry for a never-seen popup; real size comes from auto-resize after one frame. */
#define GUI_POPUP_SEED_W   120.0f
#define GUI_POPUP_SEED_H    60.0f

/* Tooltip offset from the cursor so the pointer does not cover its own hint. */
#define GUI_TOOLTIP_OFFSET  16.0f

/* Background dim painted behind a modal (semi-transparent black). */
#define GUI_POPUP_MODAL_DIM  GUI_COLOR( 0, 0, 0, 120 )

/* Mandatory popup window behavior: fixed, uncollapsible, hugging its content. */
#define GUI_POPUP_BASE_FLAGS \
    ( GUI_WIN_NOMOVE | GUI_WIN_NORESIZE | GUI_WIN_NOCOLLAPSE | GUI_WIN_ALWAYS_AUTOSIZE )

/*----------------------------------------------------------------------------------------------
    Helpers
----------------------------------------------------------------------------------------------*/

/* Popup window id from the caller's string (salted; equals s_build.win_id / hover_win for it). */
static gui_id_t
popup_id( const char* str )
{
    return id_combine( id_hash( str ), GUI_POPUP_SALT );
}

/* Nudge a placed window fully on-screen along one axis: keep [pos, pos+size) within [0, extent). */
static f32
popup_clamp( f32 pos, f32 size, f32 extent )
{
    if ( pos + size > extent ) pos = extent - size;
    if ( pos < 0.0f )          pos = 0.0f;
    return pos;
}

/*----------------------------------------------------------------------------------------------
    Overlay detach / reattach

    A popup window is begun inside another window's open layout / clip / paint state.  detach
    snapshots exactly what begin/window_end will clobber and saves the parent's top layout frame;
    reattach restores it verbatim after the popup closes.  The stack counters are NOT rewound --
    the popup pushes its frame above the parent's and the normal pop balances it, so no slot is
    reused.  The single thing pop would corrupt is the parent's pen advance (layout_pop_region
    advances the enclosing frame past the popup's box, which is meaningless for a floating window);
    restoring the saved parent frame undoes exactly that.
----------------------------------------------------------------------------------------------*/

static gui_overlay_save_t
overlay_detach( void )
{
    gui_overlay_save_t s;

    s.win_id         = s_build.win_id;
    s.win_title      = s_build.win_title;
    s.win_collapsed  = s_build.win_collapsed;
    s.win_flags      = s_build.win_flags;
    s.win_title_h    = s_build.win_title_h;
    s.win_resize_hot = s_build.win_resize_hot;
    s.win_grip_hot   = s_build.win_grip_hot;
    s.cur_win        = s_build.cur_win;
    s.win_x = s_build.win_x; s.win_y = s_build.win_y;
    s.win_w = s_build.win_w; s.win_h = s_build.win_h;
    s.clip_rect      = s_build.clip_rect;
    s.window         = draw_window();
    s.sort_key       = draw_sort_key();
    s.viewport       = draw_viewport();

    /* Save the parent's top layout frame so its pen survives the popup's region pop. */
    s.had_parent = ( s_layout_sp > 0 );
    if ( s.had_parent )
        s.parent_frame = *lf();

    /* Escape the parent window's clip: the popup's own clip then intersects the full display. */
    draw_push_clip_root();

    return s;
}

static void
overlay_reattach( gui_overlay_save_t s )
{
    /* Balance the root clip pushed in detach (the popup's own clip was already popped by
       window_end).  Restore the parent's pen, then its window context + paint cursor. */
    draw_pop_clip_rect();

    if ( s.had_parent )
        *lf() = s.parent_frame;

    s_build.win_id         = s.win_id;
    s_build.win_title      = s.win_title;
    s_build.win_collapsed  = s.win_collapsed;
    s_build.win_flags      = s.win_flags;
    s_build.win_title_h    = s.win_title_h;
    s_build.win_resize_hot = s.win_resize_hot;
    s_build.win_grip_hot   = s.win_grip_hot;
    s_build.cur_win        = s.cur_win;
    s_build.win_x = s.win_x; s_build.win_y = s.win_y;
    s_build.win_w = s.win_w; s_build.win_h = s.win_h;
    s_build.clip_rect      = s.clip_rect;
    draw_set_window( s.window );
    draw_set_sort_key( s.sort_key );
    draw_set_viewport( s.viewport );
}

/*----------------------------------------------------------------------------------------------
    popup_open -- request the popup `str` open at the current nesting depth.

    Opening truncates any sibling/deeper popups (writing at this depth replaces what was there).
    The anchor is the cursor: a regular popup opens where the click landed.
----------------------------------------------------------------------------------------------*/

/* id-based open core: request `id` open at the current nesting depth, anchored at (ax,ay).  The
   string API anchors at the cursor; the combo widget (gui_widget_combo.c) anchors at its box and
   keys the popup off the combo's widget id, so both drive this one routine. */
static void
popup_open_id( gui_id_t id, f32 ax, f32 ay )
{
    u32 depth = s_popup_begin_count;
    if ( depth >= g_ctx->popup_depth ) return;

    gui_popup_t* p = &s_popups_open[ depth ];
    p->id          = id;
    p->modal       = false;                 /* decided at begin; default until then */
    p->anchor_x    = ax;
    p->anchor_y    = ay;
    p->open_frame  = s_retained.frame;
    p->begun_frame = s_retained.frame;       /* guard the stale-close until begin runs */
    p->rect        = ( gui_rect_t ){ 0 };

    s_popup_open_count = depth + 1u;        /* opening closes anything deeper */
}

void
gui_popup_open( const char* str )
{
    popup_open_id( popup_id( str ), s_io.mouse_x, s_io.mouse_y );
}

/*----------------------------------------------------------------------------------------------
    popup_is_open -- whether `str` (or an explicit id) is anywhere in the open stack.
----------------------------------------------------------------------------------------------*/

static bool
popup_is_open_id( gui_id_t id )
{
    for ( u32 i = 0; i < s_popup_open_count; ++i )
        if ( s_popups_open[ i ].id == id ) return true;
    return false;
}

bool
gui_popup_is_open( const char* str )
{
    return popup_is_open_id( popup_id( str ) );
}

/* Re-place an open popup's anchor.  A combo re-stamps its box's bottom-left every frame so the
   dropdown tracks the box when the parent window is dragged; a no-op if `id` is not open. */
static void
popup_set_anchor( gui_id_t id, f32 ax, f32 ay )
{
    for ( u32 i = 0; i < s_popup_open_count; ++i )
        if ( s_popups_open[ i ].id == id )
        {
            s_popups_open[ i ].anchor_x = ax;
            s_popups_open[ i ].anchor_y = ay;
            return;
        }
}

/*----------------------------------------------------------------------------------------------
    popup_begin_common -- shared body of popup_begin / popup_modal_begin.

    Returns false (cheaply) when this popup is not open at the current depth, so a closed popup
    costs almost nothing.  When open, detaches the parent context, places + opens the window on
    the popup z-band, and (for an auto-size popup's first frame) parks it off-screen to measure
    invisibly, snapping into place next frame.
----------------------------------------------------------------------------------------------*/

static bool
popup_begin_common_id( gui_id_t id, const char* title, gui_win_flags_t flags, bool modal,
                       f32 fixed_w, f32 cap_h )
{
    u32 depth = s_popup_begin_count;

    /* Open at this depth?  The early-out that makes a closed popup free. */
    if ( depth >= g_ctx->popup_depth || s_popup_open_count <= depth
         || s_popups_open[ depth ].id != id )
        return false;

    gui_popup_t* p = &s_popups_open[ depth ];
    p->modal       = modal;
    p->begun_frame = s_retained.frame;

    /* The popup's window record; force its z onto the reserved band (depth-stacked) every frame. */
    gui_window_t* win = window_get( id, p->anchor_x, p->anchor_y,
                                      GUI_POPUP_SEED_W, GUI_POPUP_SEED_H );
    win->z = GUI_POPUP_Z_BASE + depth;

    /* Pin the popup to the parent's CURRENT surface every frame, not just the one it was first
       seen on.  window_get seeds viewport only at creation, so a popup first opened while its
       parent sat on the main viewport would keep rendering there after the parent detached into
       its own floater surface.  cur_viewport is the surface of the window (or parent popup) that
       opened this one, so re-stamping it here makes the menu follow its parent across detach /
       reattach. */
    win->viewport = s_build.cur_viewport;

    /* Capped popup (combo dropdown): a fixed width with a height that hugs the measured content up
       to cap_h, then scrolls -- the same hug-then-scroll behavior child_begin gets from a max-height
       constraint, expressed for the window path.  Size is known up front (no off-screen premeasure):
       seed from cap_h before any content, then track min(content + chrome, cap_h) each frame.  This
       takes over the autosize path; the caller passes scroll-capable (non-autosize) flags. */
    bool capped = ( fixed_w > 0.0f );
    if ( capped )
    {
        f32 chrome = WIDGET_GAP + WIN_BORDER;   /* body top gap + bottom border (no title bar) */
        f32 want_h = ( win->content_h > 0.0f ) ? win->content_h + chrome : cap_h;
        if ( want_h > cap_h ) want_h = cap_h;
        gui_window_set_next_size( fixed_w, want_h, GUI_COND_ALWAYS );
        win->w = fixed_w;       /* placement clamp below reads win->w/h; reflect the queued size */
        win->h = want_h;
    }

    /* Auto-size appearing frame: size is unknown until the body is measured, so park off-screen
       this frame (still measured, just invisible) and place it for real next frame.  A non-
       auto-size popup (capped or fixed) has a known size and goes straight to its anchor. */
    bool autosize   = ( flags & GUI_WIN_ALWAYS_AUTOSIZE ) != 0;
    bool premeasure = autosize && win->content_h <= 0.0f;

    f32 px, py;
    if ( premeasure )
    {
        px = py = GUI_POPUP_OFFSCREEN;
    }
    else if ( modal )
    {
        px = ( (f32)s_io.display_w - win->w ) * 0.5f;   /* modals center on the display */
        py = ( (f32)s_io.display_h - win->h ) * 0.5f;
    }
    else
    {
        px = popup_clamp( p->anchor_x, win->w, (f32)s_io.display_w );
        py = popup_clamp( p->anchor_y, win->h, (f32)s_io.display_h );
    }
    gui_window_set_next_pos( px, py, GUI_COND_ALWAYS );

    /* Detach from the parent window: from here the popup lays out + clips as a top-level overlay. */
    p->saved = overlay_detach();

    /* Modal: dim the whole display one z below the modal (skipped on the off-screen appearing
       frame so there is no one-frame dim-without-modal flash).  Drawn after detach so it sits
       under the root clip and covers everything. */
    if ( modal && !premeasure )
    {
        draw_set_sort_key( win->z - 1u );
        draw_set_rounding( 0.0f );   /* full-display dim: no rounded screen corners */
        draw_push_rect_filled( 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h,
                               0.0f, 0.0f, 1.0f, 1.0f, 0, GUI_POPUP_MODAL_DIM );
    }

    bool vis = window_begin_ex( id, title, p->anchor_x, p->anchor_y,
                                GUI_POPUP_SEED_W, GUI_POPUP_SEED_H, flags );

    /* Record the on-screen rect for next frame's click-outside test. */
    p->rect = ( gui_rect_t ){ win->x, win->y, win->w, win->h };

    ++s_popup_begin_count;
    return vis;
}

/* String-keyed wrapper: hash + salt the caller's id string, then run the id-based core (auto-size,
   no fixed width / height cap -- the cap path is the combo dropdown's). */
static bool
popup_begin_common( const char* str, const char* title, gui_win_flags_t flags, bool modal )
{
    return popup_begin_common_id( popup_id( str ), title, flags, modal, 0.0f, 0.0f );
}

bool
gui_popup_begin( const char* str, gui_win_flags_t flags )
{
    return popup_begin_common( str, NULL,
                               flags | GUI_WIN_NOTITLEBAR | GUI_POPUP_BASE_FLAGS, false );
}

bool
gui_popup_modal_begin( const char* str, const char* title, gui_win_flags_t flags )
{
    /* Modal keeps a title bar (no NOTITLEBAR) so its heading shows; falls back to the id string. */
    return popup_begin_common( str, title ? title : str,
                               flags | GUI_POPUP_BASE_FLAGS, true );
}

void
gui_popup_end( void )
{
    if ( !s_popup_begin_count ) return;     /* unbalanced popup_end -- ignore */

    u32 depth = s_popup_begin_count - 1u;
    gui_window_end();                     /* finalize the popup window (pops its own clip) */
    overlay_reattach( s_popups_open[ depth ].saved );
    --s_popup_begin_count;
}

/*----------------------------------------------------------------------------------------------
    popup_close_current -- close this popup (and any deeper) from inside its body.

    Inside begin/popup_end, begin_count is this popup's depth + 1, so truncating the open stack
    to begin_count - 1 drops this popup and its children.
----------------------------------------------------------------------------------------------*/

void
gui_popup_close_current( void )
{
    if ( s_popup_begin_count && s_popup_open_count >= s_popup_begin_count )
        s_popup_open_count = s_popup_begin_count - 1u;
}

/*----------------------------------------------------------------------------------------------
    Context menus -- open a popup on a right-click.

    _item binds to the previous widget (its id is latched in s_build.last_item_id by widget_behavior);
    _window binds to empty space in the current window.  Both then render through popup_begin.
----------------------------------------------------------------------------------------------*/

bool
gui_popup_context_item_begin( const char* str )
{
    gui_id_t want = s_build.last_item_id;
    if ( want != GUI_ID_NONE && s_interaction.hover_id == want && s_io.mouse_pressed[ 1 ] )
        gui_popup_open( str );
    return gui_popup_begin( str, GUI_WIN_NONE );
}

bool
gui_popup_context_window_begin( const char* str )
{
    if ( s_build.win_id == s_interaction.hover_win && s_interaction.hover_id == GUI_ID_NONE
         && s_io.mouse_pressed[ 1 ] )
        gui_popup_open( str );
    return gui_popup_begin( str, GUI_WIN_NONE );
}

/*----------------------------------------------------------------------------------------------
    Tooltips -- a non-interactive overlay at the cursor, shown only while live.

    A tooltip carries no open state (it is purely "is the previous item hovered this frame"), so
    it does not use the popup stack; it is just a top-level overlay window on the band above every
    popup.  It still detaches like a popup so it can be raised from inside another window.
----------------------------------------------------------------------------------------------*/

static gui_overlay_save_t s_tooltip_save;   /* tooltips do not nest, so one save slot suffices */

bool
gui_tooltip_begin( void )
{
    gui_window_t* win = window_get( GUI_TOOLTIP_ID, s_io.mouse_x, s_io.mouse_y,
                                      GUI_POPUP_SEED_W, GUI_POPUP_SEED_H );
    win->z = GUI_POPUP_Z_BASE + g_ctx->popup_depth;   /* above every popup */
    win->viewport = s_build.cur_viewport;               /* track the parent's current surface (see popup_begin) */

    bool premeasure = win->content_h <= 0.0f;
    f32  px, py;
    if ( premeasure )
    {
        px = py = GUI_POPUP_OFFSCREEN;
    }
    else
    {
        px = popup_clamp( s_io.mouse_x + GUI_TOOLTIP_OFFSET, win->w, (f32)s_io.display_w );
        py = popup_clamp( s_io.mouse_y + GUI_TOOLTIP_OFFSET, win->h, (f32)s_io.display_h );
    }
    gui_window_set_next_pos( px, py, GUI_COND_ALWAYS );

    s_tooltip_save = overlay_detach();
    return window_begin_ex( GUI_TOOLTIP_ID, NULL, s_io.mouse_x, s_io.mouse_y,
                            GUI_POPUP_SEED_W, GUI_POPUP_SEED_H,
                            GUI_WIN_NOTITLEBAR | GUI_POPUP_BASE_FLAGS );
}

void
gui_tooltip_end( void )
{
    gui_window_end();
    overlay_reattach( s_tooltip_save );
}

void
gui_set_item_tooltip( const char* text )
{
    if ( s_build.last_item_id == GUI_ID_NONE || s_interaction.hover_id != s_build.last_item_id )
        return;

    if ( gui_tooltip_begin() )
    {
        gui_stack();          /* tooltip body lays out like any region: declare a stack first */
        gui_text( text );
    }
    gui_tooltip_end();
}

/*----------------------------------------------------------------------------------------------
    help_marker -- a dim "(?)" hint that reveals `text` in a tooltip on hover (no click).

    The Dear ImGui idiom: emit it on the same line after a control to footnote it.

        gui()->checkbox( "No mouse", &flag );
        gui()->same_line( 0.0f );
        gui()->help_marker( "Disable mouse inputs and interactions." );

    It is a leaf item like text(), but registers an id so the tooltip can bind to it -- the
    glyphs are the hover area.  It never captures the click (purely visual): widget_behavior is
    used only to drive the hover, and the mark brightens from dim to full text while pointed at.
----------------------------------------------------------------------------------------------*/

void
gui_help_marker( const char* text )
{
    const char* mark = "(?)";
    gui_id_t  id   = id_combine( id_seed(), id_hash( text ) );

    /* Carve a natural-width cell for the mark and place it per the region alignment, like text(). */
    f32          mw = font_text_w( mark );
    f32          mh = font_char_h();
    gui_rect_t r  = widget_next_rect_w( mw, mh );
    gui_rect_t tr = rect_align( r, mw, mh, lf()->lay_align );

    /* Hoverable but inert: the returned click is ignored, only st.hover drives the brighten. */
    widget_state_t st = widget_behavior( id, tr, WIDGET_KIND_BUTTON );
    draw_push_text( tr.x, tr.y, st.hover ? COL_TEXT : COL_TEXT_DIM, mark );
    widget_track_width( tr.x + mw );

    /* Bind the tooltip to the mark just emitted (last_item_id / hover_id were set above). */
    gui_set_item_tooltip( text );
}

/*----------------------------------------------------------------------------------------------
    popup_close_check -- stale-close + click-outside, run at frame top before any user code.

    Stale: an open popup whose popup_begin was not called last frame (the caller stopped emitting
    it) is dropped from the top down, so a removed parent takes its children with it.

    Click-outside: on a press, keep the deepest popup whose rect holds the cursor and close
    everything deeper; a press fully outside closes all -- but never at or below the topmost modal
    (whose outside clicks are swallowed, not closing it).  Containment alone resolves nesting: a
    click in a child keeps the chain, a click in a parent body closes just the child.
----------------------------------------------------------------------------------------------*/

static void
popup_close_check( void )
{
    if ( !s_popup_open_count ) return;

    /* Stale-close: not begun last frame nor this one (begun_frame + 1 < frame_counter). */
    while ( s_popup_open_count
            && s_popups_open[ s_popup_open_count - 1u ].begun_frame + 1u < s_retained.frame )
        --s_popup_open_count;

    if ( !s_popup_open_count ) return;
    if ( !s_io.mouse_pressed[ 0 ] && !s_io.mouse_pressed[ 1 ] ) return;

    /* The topmost modal pins [0, floor) open -- nothing at or below it auto-closes. */
    u32 floor = 0;
    for ( u32 i = 0; i < s_popup_open_count; ++i )
        if ( s_popups_open[ i ].modal ) floor = i + 1u;

    u32 keep = floor;
    for ( u32 i = s_popup_open_count; i-- > floor; )
        if ( rect_hit( s_popups_open[ i ].rect ) ) { keep = i + 1u; break; }

    s_popup_open_count = keep;
}

/*----------------------------------------------------------------------------------------------
    popup_apply_modal -- fence interaction behind the topmost open modal.

    When a modal is open, anything not over the modal (or a popup opened on top of it) must be
    inert.  Stealing hover_win is the whole fence: widget_behavior gates all hover on
    s_build.win_id == hover_win, so pointing hover_win at the modal freezes every window behind it
    with no per-widget code -- the window-scale analogue of active_id drag-modality.
----------------------------------------------------------------------------------------------*/

static void
popup_apply_modal( void )
{
    i32 m = -1;
    for ( u32 i = 0; i < s_popup_open_count; ++i )
        if ( s_popups_open[ i ].modal ) m = (i32)i;
    if ( m < 0 ) return;

    /* Allow the modal and any deeper (later-opened, on-top) popup to keep interacting. */
    gui_id_t hw = s_interaction.hover_win;
    for ( u32 i = (u32)m; i < s_popup_open_count; ++i )
        if ( s_popups_open[ i ].id == hw ) return;

    s_interaction.hover_win = s_popups_open[ m ].id;
}

// clang-format on
/*============================================================================================*/
