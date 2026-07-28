/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_tab_bar.c -- In-window tab bar.

    A tabbed content switcher that lives inside an ordinary window / child body (the Dear ImGui
    BeginTabBar / BeginTabItem analogue) -- distinct from the docking tab strips (gui_dock_drag.c),
    which tab whole WINDOWS into a dock node.  This one tabs sections of ONE window's body: only the
    active tab's widgets emit, right below a strip of clickable chips.

        if ( gui()->tab_bar_begin( "settings", GUI_TAB_BAR_NONE ) )
        {
            if ( gui()->tab_item_begin( "General", NULL, GUI_TAB_ITEM_NONE ) )
            {
                gui()->checkbox( "Vsync", &vsync );
                gui()->tab_item_end();
            }
            if ( gui()->tab_item_begin( "Audio", NULL, GUI_TAB_ITEM_NONE ) )
            {
                gui()->slider_float( "Volume", &vol, 0.0f, 1.0f );
                gui()->tab_item_end();
            }
            gui()->tab_bar_end();
        }

    Immediate-mode model, mirroring the dock strip's look (active chip takes the body colour so it
    reads as joined to the content below).  The chips are drawn as absolute rects into the reserved
    strip row -- they do NOT consume layout cells -- so the active tab's body flows normally in the
    window below the strip.  tab_item_begin marches a pen across the strip as each item is emitted;
    the ACTIVE selection persists per bar id in the keyed state pool (core/gui_state.c), the same
    store windows / tree nodes / combos use.  The first tab is the default selection; if the
    selected tab disappears (its item stops being emitted) the bar re-defaults to the first tab.

    Chips opt out of keyboard nav (s_scope.nav.skip, like scrollbars) -- they are drawn at absolute
    positions rather than placed by the layout engine, so they carry no meaningful nav coordinate.
    Tab switching is mouse-driven; the active body's widgets navigate as usual.

    Included by gui_chrome.c after gui_widget_numeric.c; item_id / cell_next / item_state /
    GUI_STATE and the draw + style helpers all resolve through the sub-stack headers.

==============================================================================================*/
// clang-format off

/* Per-bar persisted selection: the id of the active tab.  0 until the first tab adopts it. */
typedef struct { gui_id_t selected; } gui_tabbar_state_t;

/* One open tab bar -- frame scratch, pushed by tab_bar_begin and popped by tab_bar_end.  The pen
   (x) marches across the strip as chips emit; first_seen / sel_seen let tab_bar_end re-default the
   selection when the previously-selected tab is no longer present. */
typedef struct
{
    gui_id_t             id;          // bar id (item_id of id_str, within its pushed scope)
    gui_rect_t           strip;       // the reserved strip row (screen rect)
    f32                  x;           // running pen: left edge of the next chip
    gui_tabbar_state_t*  st;          // persisted selection slot
    gui_id_t             first_seen;  // first tab id emitted this frame (fallback default)
    bool                 sel_seen;    // the selected id matched a tab emitted this frame
    gui_id_t             want;        // tab clicked this frame; committed at tab_bar_end (0 = none)
    gui_tab_bar_flags_t  flags;
    bool                 item_open;   // a tab_item_begin returned true and pushed its id scope --
                                      //   the latch that makes tab_item_end safe to call whether
                                      //   or not the body ran (the one begin/end rule).  Per-bar,
                                      //   so a tab body hosting a nested bar unwinds correctly.

} gui_tabbar_ctx_t;

#define GUI_TABBAR_STACK_MAX 8   // max nested tab bars

static gui_tabbar_ctx_t s_tabbars[ GUI_TABBAR_STACK_MAX ];
static u32              s_tabbar_depth;

/* Every tab_bar_begin counts here, INCLUDING one that overflowed the ctx stack and opened no bar.
   tab_bar_end pairs against this rather than s_tabbar_depth, so an unguarded end after a failed
   begin unwinds its own (empty) level instead of tearing down the enclosing bar -- the uniform
   begin/end rule holding even on the degrade path. */
static u32              s_tabbar_attempt;

/* Salt so a tab's close (x) button gets a distinct widget id from the chip it sits on. */
#define GUI_TAB_CLOSE_SALT 0x7ab0c105u

/*==============================================================================================
    tab_bar_begin -- reserve one strip row and open a bar.  Returns true always (guard-and-pair
    like child_begin): always call tab_bar_end.  The strip band is painted here; the chips paint
    over it in tab_item_begin.
==============================================================================================*/

bool
gui_tab_bar_begin( const char* id_str, gui_tab_bar_flags_t flags )
{
    ++s_tabbar_attempt;          /* counted before any early-out: tab_bar_end pairs against this */

    gui_push_id( id_str );

    gui_rect_t strip = cell_next( WIDGET_H );   /* one row for the tab strip; body flows below */

    if ( s_tabbar_depth >= GUI_TABBAR_STACK_MAX )
    {
        /* Out of nesting slots -- keep the id scope balanced and degrade to "no bar". */
        gui_pop_id();
        return false;
    }

    gui_id_t          id  = item_id( "##tab_bar" );
    gui_tabbar_ctx_t* ctx = &s_tabbars[ s_tabbar_depth++ ];

    ctx->id         = id;
    ctx->strip      = strip;
    ctx->x          = strip.x;
    ctx->st         = GUI_STATE( gui_tabbar_state_t, id );
    ctx->first_seen = GUI_ID_NONE;
    ctx->sel_seen   = false;
    ctx->want       = GUI_ID_NONE;
    ctx->flags      = flags;
    ctx->item_open  = false;

    /* Flat strip band + a thin seam line along its bottom edge; the active chip overpaints the seam
       in the body colour so it reads as joined to the content below. */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );
    draw_face( strip, GUI_ROLE_TITLE, GUI_PHASE_IDLE );
    draw_push_rect_filled( strip.x, strip.y + strip.h - WIN_BORDER, strip.w, WIN_BORDER,
                           0, 0, 1, 1, 0, COL_BORDER_IDLE );
    draw_set_rounding( save_round );

    return true;
}

/* Safe to call whether tab_bar_begin returned true or false (it degrades to false only when the
   nesting cap is hit).  Pairs against s_tabbar_attempt so a failed begin's end unwinds nothing. */
void
gui_tab_bar_end( void )
{
    if ( s_tabbar_attempt == 0 )
        return;

    bool overflowed = ( s_tabbar_attempt > GUI_TABBAR_STACK_MAX );
    --s_tabbar_attempt;
    if ( overflowed || s_tabbar_depth == 0 )
        return;                  /* this end belongs to a begin that opened no bar */

    gui_tabbar_ctx_t* ctx = &s_tabbars[ s_tabbar_depth - 1 ];

    /* Commit a click made this frame: the pending selection becomes the visible tab NEXT frame, so
       every frame emits exactly one body.  wants_redraw so an idle UI repaints with the change. */
    if ( ctx->want != GUI_ID_NONE )
    {
        if ( ctx->want != ctx->st->selected )
        {
            ctx->st->selected            = ctx->want;
            redraw_request();
        }
    }
    /* No click, and the selected tab vanished this frame (its item stopped being emitted -- e.g. it
       was closed): fall back to the first tab so the bar is never left showing nothing. */
    else if ( !ctx->sel_seen && ctx->first_seen != GUI_ID_NONE
              && ctx->st->selected != ctx->first_seen )
    {
        ctx->st->selected            = ctx->first_seen;
        redraw_request();
    }

    s_tabbar_depth--;
    gui_pop_id();
}

/*==============================================================================================
    tab_item_begin -- emit one tab chip and report whether it is the selected tab.  The return
    gates the BODY only; tab_item_end is called unconditionally, like every other pair:

        if ( gui()->tab_item_begin( "Log", NULL, GUI_TAB_ITEM_NONE ) ) { ...body... }
        gui()->tab_item_end();

    p_open (optional): when non-NULL a close (x) sits at the chip's right edge; clicking it sets
    *p_open = false (the caller stops emitting the item next frame).  The click does not switch tabs.
==============================================================================================*/

bool
gui_tab_item_begin( const char* label, bool* p_open, gui_tab_item_flags_t flags )
{
    (void)flags;

    if ( s_tabbar_depth == 0 )
        return false;

    gui_tabbar_ctx_t* ctx = &s_tabbars[ s_tabbar_depth - 1 ];

    gui_id_t tid = item_id( label );

    /* First tab seen is the fallback default; the first-ever bar adopts it as the live selection. */
    if ( ctx->first_seen == GUI_ID_NONE )
        ctx->first_seen = tid;
    if ( ctx->st->selected == GUI_ID_NONE )
        ctx->st->selected = tid;

    /* Chip footprint: label + side pad, plus a square close cell when p_open is offered. */
    f32 close_w = p_open ? WIDGET_H : 0.0f;
    f32 tw      = font_text_w( label ) + 2.0f * WIDGET_PAD + close_w;

    gui_rect_t tr = { ctx->x, ctx->strip.y, tw, ctx->strip.h };
    ctx->x += tw;

    /* The VISIBLE tab is the committed selection, fixed for the whole frame -- so exactly one tab
       body emits per frame.  A click only records a PENDING selection (ctx->want) that tab_bar_end
       commits, taking effect next frame; committing mid-frame would double-emit (the previously
       active tab, already emitted above, plus this one). */
    bool is_active = ( ctx->st->selected == tid );
    if ( is_active )
        ctx->sel_seen = true;

    /* Two non-overlapping hit rects: the chip minus the close cell selects the tab; the close cell
       (when p_open) is its own target -- otherwise a click on the x would just reselect the tab. */
    gui_rect_t sel_r   = { tr.x, tr.y, tr.w - close_w, tr.h };
    gui_rect_t close_r = { tr.x + tr.w - close_w, tr.y, close_w, tr.h };

    /* Chip select.  nav.skip: the chip is not laid out by the layout engine, so it is no
       keyboard-nav target (mirrors the scrollbar / dock drag strip). */
    s_scope.nav.skip = true;
    gui_item_state_t st = item_state( tid, sel_r, ITEM_BUTTON );
    if ( st.clicked )
    {
        ctx->want                    = tid;
        redraw_request();
    }

    /* Active chip takes the body colour (joined to the content below); the rest sit on the title
       band and lift to the hover colour under the cursor.  Square, like the dock tabs. */
    u32 bg   = is_active ? COL_TITLE_ACTIVE : ( st.hover ? COL_TITLE_HOT : COL_TITLE_IDLE );
    u32 tcol = ( is_active || st.hover ) ? COL_TEXT_IDLE : COL_TEXT_DIM;

    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );
    draw_push_rect_filled( tr.x, tr.y, tr.w, tr.h, 0, 0, 1, 1, 0, bg );

    draw_text_fit_n( tr.x + WIDGET_PAD, text_center_y( tr.y, tr.h ), tcol, label,
                     (u32)strlen( label ), tw - 2.0f * WIDGET_PAD - close_w );

    /* Close (x): a square cell at the chip's right edge, its own click target. */
    if ( p_open )
    {
        gui_id_t         cid = id_combine( tid, GUI_TAB_CLOSE_SALT );
        s_scope.nav.skip     = true;
        gui_item_state_t cst = item_state( cid, close_r, ITEM_BUTTON );
        if ( cst.hover || cst.active )
            draw_push_rect_filled( close_r.x, close_r.y, close_r.w, close_r.h, 0, 0, 1, 1, 0,
                                   COL_BG_HOT );
        gui_draw_close( close_r, col_btn_glyph( cst ) );
        if ( cst.clicked )
        {
            *p_open                      = false;
            redraw_request();
        }
    }

    draw_set_rounding( save_round );

    if ( !is_active )
        return false;

    /* Active: open a per-tab id scope so two tabs' bodies with same-named widgets never alias.
       Latch it so tab_item_end knows whether there is a scope to pop. */
    gui_push_id( label );
    ctx->item_open = true;
    return true;
}

/* Safe to call whether tab_item_begin returned true or false -- the uniform begin/end rule: the
   bool gates the BODY, never the end call.  Only the tab that actually opened an id scope pops
   one, so an unguarded end on an inactive tab cannot unbalance the id stack. */
void
gui_tab_item_end( void )
{
    if ( s_tabbar_depth == 0 )
        return;

    gui_tabbar_ctx_t* ctx = &s_tabbars[ s_tabbar_depth - 1 ];
    if ( !ctx->item_open )
        return;

    ctx->item_open = false;
    gui_pop_id();
}

// clang-format on
/*============================================================================================*/
