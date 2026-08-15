/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_volatile.c -- Volatile widgets, chrome half.

    A "volatile" callback contains ordinary UI emit calls (text, colored rects, etc).  It runs
    inline during a real (dirty) frame via gui()->volatile_cb -- its widgets render exactly like
    any other code, no special behavior.  On an idle frame (frame_begin returned false, no
    ctx_begin/emit ran), gui_frame_end calls volatile_update internally; the render server's half
    (render/pipeline/gui_build_volatile.c) re-invokes the same callback standalone, re-tessellates its
    output, and patches it into the padded region reserved for the block inside its window's
    cached geometry (any output that fits is accepted; only outgrowing the reservation costs a
    real frame) -- see gui.h (gui_volatile_fn) for the full contract and gui_render.h for the
    unit-seam declarations shared with the backend half.

    Everything in THIS file is the frontend side of the seam:

        gui_volatile_cb / gui_volatile_begin / gui_volatile_end
            The public API (gui_api.h vtable: volatile_cb / volatile_begin / volatile_end).
            gui_volatile_cb wraps one inline invocation of the caller's callback so the backend
            can bracket the exact command range it produces; the callback itself calls
            volatile_begin/end from inside its own body to stamp the layout cursor position.

        volatile_layout_push / volatile_layout_pop
            A minimal layout-frame push/pop at an explicit (x, y, w) -- lighter than
            layout_push_region (no scrollbar gutter, no clip push, no id_push): the replay scope
            replay_scope_enter/_exit installs around a standalone callback invocation.

        replay_scope_enter / replay_scope_exit
            The reverse half of the unit seam: volatile_update (the render server) calls these
            around each row's replay so the callback's ordinary gui()->text()/rect_filled()/...
            calls have a valid (if minimal) layout frame and id scope to emit into, without
            running ctx_begin/ctx_new_frame or touching anything else about the real frame's UI
            state.  s_replay_mode itself lives in core/gui_ctx.c (ambient state, same tier as
            s_interaction) so item_state (core/gui_item.c) can read it inline; this file
            is the only place that sets it.

    Included by gui_chrome.c after the widget family files.  Its replay scope pushes a bare layout
    frame, so it needs lf() / layout_frame_t / layout_set_default (the flow unit, via
    flow/gui_flow.h) and id_push / id_pop (the interact server, via core/gui_core.h) -- both
    resolved across unit boundaries by header, not by include order.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Volatile widget callback -- see gui.h (gui_volatile_fn) for the full contract.
==============================================================================================*/

/* Layout-footprint probe -- the state gui_volatile_begin arms so gui_volatile_cb can read back
   the extent the block claimed (via the flow unit's claim watcher, layout_claim_begin/_end) and
   hand it to the backend (volatile_footprint).  File scope for the same reason the backend's
   s_open_id is: exactly one gui_volatile_cb invocation is ever in flight, nesting is not
   supported. */
static f32  s_foot_x, s_foot_y;             // cell origin the open block was stamped at
static bool s_foot_open;                    // gui_volatile_begin ran for the block now emitting

/* Margin the measured cell is grown by before it becomes the block's clip -- see gui_volatile_cb.
   Ink can exceed layout by a fraction of a pixel where an advance does not cover a glyph's
   bitmap; nothing DRAWS into the margin, so it relaxes the cut without loosening the cell. */
#define VOL_CELL_INK  2.0f

/* gui_volatile_cb wraps one real-emit invocation of `fn` so the backend can bracket the exact
   command range it produces (volatile_cb_open/_close, gui_render.h); the callback itself
   calls gui_volatile_begin/end from inside its own body, per the caller's own code -- begin
   stamps the layout cursor position the callback started at (needed to reconstruct a matching
   scope on replay), end is a reserved no-op (see gui_volatile_end).  `label` is hashed the same way item_id() hashes a
   widget label (id_combine(id_seed(), id_hash(label))) -- callers pass an ordinary string, same as
   any other widget call, rather than manufacturing their own gui_id_t. */
void
gui_volatile_cb( const char* label, gui_volatile_fn fn )
{
    gui_id_t id = id_combine( id_seed(), id_hash( label ) );
    volatile_cb_open( id );
    s_foot_open = false;   /* armed only by this callback's own gui_volatile_begin -- a replay
                              that ran between real emits must not leave a stale probe behind */
    fn( id, false );

    /* Close the footprint probe gui_volatile_begin opened: read the extent this block CLAIMED
       from the flow unit's claim watcher -- the same terms replay_scope_measure reports a replay
       in, so the backend can compare the two and buy a real frame when they diverge.  The watcher,
       not the region highwater: a cell that fills its track (a canvas, a separator) deliberately
       grows high_x by nothing (line_place_cell's scrollbar rule), so a highwater-based measure
       reads a track-filling block as 0 wide and the confinement below cuts it to its ink margin.
       A callback that never called gui_volatile_begin reports nothing; the row has no cursor
       stamp either, so it never replays. */
    gui_rect_t  cell;
    gui_rect_t* cellp = NULL;

    if ( s_foot_open )
    {
        layout_frame_t* f = lf();
        f32             fw, fh;
        layout_claim_end( &fw, &fh );

        volatile_footprint( fw, fh );
        s_foot_open = false;

        /* The cell the block just claimed, cut to the region's VIEW -- the clip the block hands
           the backend to confine its command range with (volatile_cb_close explains what that
           buys).  The view is what makes it scroll-safe: a window body shares one clip rect with
           its title bar and border and resolves scroll-out by overpainting (window_open_body), so
           the cell ALONE would still reach the chrome once the block scrolls under it.  f->view is
           that region's visible box, already inset by the border and by any reserved scrollbar
           gutter, and every frame a real emit can run in sets it (layout_push_region,
           sublayout_open).
             The footprint measured above is the extent the block CLAIMED, which is exactly what
           the fixed-footprint contract entitles it to paint in -- so confining it here enforces
           that contract rather than narrowing it.  VOL_CELL_INK of margin absorbs the one honest
           way ink exceeds layout: a glyph whose bitmap overhangs its advance (italics, heavy side
           bearings) would otherwise lose a column at the cell's right edge.  Widening the clip
           cannot widen what the block DRAWS, so the margin costs the confinement argument nothing
           -- the block's geometry still lands in its own cell, which is what makes drawing it
           after its neighbours safe. */
        cell  = rect_intersect( ( gui_rect_t ){ s_foot_x - VOL_CELL_INK, s_foot_y - VOL_CELL_INK,
                                                fw + 2.0f * VOL_CELL_INK,
                                                fh + 2.0f * VOL_CELL_INK },
                                f->view );
        cellp = &cell;
    }

    volatile_cb_close( fn, cellp );
}

void
gui_volatile_begin( void )
{
    /* s_build.item_flags (the begin_disabled/end_disabled stack) and the style push/pop stacks
       (s_col_sp/s_var_sp, gui_style.c) are NOT part of the seam replay_scope_enter reconstructs
       on an idle-frame replay -- only the id scope and a minimal layout frame are.  ctx_new_frame
       resets both to empty at the start of every REAL frame (style_new_frame, gui_ctx.c's
       frame_begin), so by the time an idle frame's volatile_update re-invokes this callback
       standalone, they read back as "nothing pushed" regardless of what ancestor
       begin_disabled()/push_style_color()/push_style_var() scope this volatile_cb call was
       actually nested in at real emit.  draw_set_alpha/draw_set_rounding (item_flags_resolve) and
       style_col/style_var would then silently resolve to the wrong value on replay -- and nothing
       downstream can catch it, since the patched geometry is valid either way.  A callback is only safe to
       replay if it does not depend on inherited disabled/style-push state from its call site;
       assert here so a violation is caught at the call site immediately instead of shipping a
       widget that quietly un-dims or re-colors itself on idle frames. */
    ORB_ASSERT( s_build.item_flags == GUI_ITEM_NONE
             && "gui_volatile_cb: callback runs under an ambient begin_disabled() scope -- "
                "not reproduced on idle-frame replay" );
    ORB_ASSERT( style_stacks_empty()
             && "gui_volatile_cb: callback runs under an ambient push_style_color/var() scope -- "
                "not reproduced on idle-frame replay" );

    /* The replay scope is a minimal single-column stack at the stamped (x, y, w) -- so the stamp
       must be the rect of the CELL this block is about to claim, not the frame cursor.  In a
       multi-column row the frame cursor is the whole row (content_x / full content_w at the pen),
       so every block of the row would replay full-width and mutually overlapping at the same y --
       correct on real frames, garbage on idle ones.  For stack / columns flow, peek the pending
       track the way line_place_cell will resolve it: a row start (col 0) applies the owed gap via
       layout_next_y, a mid-row cell reuses the open row's top (line.cross).  Grid and pack modes
       keep the frame-cursor stamp -- their walk state is not reproducible in the minimal replay
       scope, so volatile blocks there remain flow-only by contract. */
    layout_frame_t* f = lf();
    f32 x = f->content_x, y = f->pen_y, w = f->content_w;
    if ( ( f->mode == GUI_MODE_STACK || f->mode == GUI_MODE_COLUMNS ) && f->tmpl.nrows == 0 )
    {
        u32 c = f->line.col < f->tmpl.ncols ? f->line.col : 0;
        x = f->tmpl.cellx[ c ];
        w = f->tmpl.cellw[ c ];
        y = ( c == 0 ) ? layout_next_y( f ) : f->line.cross;
    }
    volatile_stamp( x, y, w, &f->view, f->pad );

    /* Open the footprint probe: the flow unit's claim watcher folds every cell the callback
       claims into a running max from this origin -- including cells that FILL their track, which
       the region highwater deliberately does not count (line_place_cell).  On an idle replay this
       re-begin is redundant but harmless: replay_scope_enter already opened the watcher at the
       identical stamped origin (the minimal replay frame reproduces this cell resolution). */
    s_foot_x = x;
    s_foot_y = y;
    layout_claim_begin( x, y );
    s_foot_open = true;
}

void
gui_volatile_end( void )
{
    /* FUTURE: per-command-type stamping; currently a no-op (paired bookend to gui_volatile_begin). */
}

/*==============================================================================================
    volatile_layout_push / volatile_layout_pop -- a minimal layout frame at an explicit (x, y, w),
    used only by replay_scope_enter/_exit below.  Unlike layout_push_region this reserves no
    scrollbar gutter, pushes no clip, and calls no id_push -- the caller handles id scoping
    itself.  layout_set_default installs a plain single-column stack and resets the
    modifier/template state, so a widget can be placed immediately without tripping the
    emit-before-header guard in cell_next_w.  band_bottom is set far below y since a
    replay frame never opens a grid.
==============================================================================================*/

/* Zeroed scroll link for the replay frame: the stamped (x, y) already carries the real emit's
   scroll bias, so the replay is anchored at scroll 0, and every f->scroll deref stays valid. */
static gui_scroll_link_t s_replay_scroll;

static void
volatile_layout_push( f32 x, f32 y, f32 w, const gui_rect_t* view, gui_pad_t pad )
{
    layout_frame_t* f = layout_frame_push();

    /* The stack slot is reused memory -- at this depth it still holds whatever region last
       occupied it (typically the LAST window of the previous real frame).  Zero the whole frame
       rather than fill a subset: any field a widget reads mid-emit -- view/pad for text's
       self-fit ellipsis (gui_view_avail), the scroll link, gap_pending -- must be THIS scope's,
       or the replay lays out against another window's geometry (a narrow debug overlay's view
       once ellipsized a replayed text run mid-string). */
    *f = ( layout_frame_t ){ 0 };

    f->content_x   = x;
    f->pen_y       = y;
    f->content_w   = w;
    f->high_x      = x;
    f->high_y      = y;
    f->band_bottom = y + 1.0e6f;   /* far below: a replay frame never opens a grid */
    f->view        = *view;        /* the region view stamped at real emit (volatile_stamp) */
    f->outer       = *view;
    f->pad         = pad;
    f->origin_x    = x;
    f->origin_y    = y;
    f->scroll      = &s_replay_scroll;

    layout_set_default( f );
}

/* Pop a scope opened by volatile_layout_push -- no measurement, no scrollbar draw, just unwind
   the stack pointer (the replay path never scrolls or reports content size). */
static void
volatile_layout_pop( void )
{
    layout_frame_pop();
}

/*==============================================================================================
    replay_scope_enter / _exit -- the reverse half of the volatile-widget seam (see
    gui_render.h).  volatile_update (render/pipeline/gui_build_volatile.c) calls these around each
    row's standalone replay invocation so the callback's ordinary gui()->text()/rect_filled()/...
    calls have a valid (if minimal) layout frame and id scope to emit into, without running
    ctx_begin/ctx_new_frame or touching anything else about the real frame's UI state.
==============================================================================================*/

void
replay_scope_enter( gui_id_t id, f32 x, f32 y, f32 w, const gui_rect_t* view, gui_pad_t pad )
{
    id_push( id );
    volatile_layout_push( x, y, w, view, pad );
    layout_claim_begin( x, y );
    replay_mode_enter();
}

/* Read back the layout extent the replayed callback just claimed, in the same terms
   gui_volatile_cb reports a real emit in (the claim watcher, opened at the block's cell origin).
   Called by volatile_update after the callback returns and before _exit pops the frame; the
   backend compares it against the real emit's footprint and forces one real frame when they
   diverge, since the neighbours a grown block now overlaps are frozen cached geometry. */
void
replay_scope_measure( f32* out_w, f32* out_h )
{
    layout_claim_end( out_w, out_h );
}

void
replay_scope_exit( bool force_redraw )
{
    volatile_layout_pop();
    id_pop();
    replay_mode_exit();
    if ( force_redraw )
        redraw_request();
}

// clang-format on
/*============================================================================================*/
