/*==============================================================================================

    runtime_service/gui/style/gui_style_core.c -- Style stacks: colors + layout metrics.

    The push-model theme override the widgets draw through, the ImGui PushStyleColor / PushStyleVar
    analogue.  Three layers resolve into the value a widget sees:

        Base   -- the theme default.  Colors: the active theme's palette (s_style.colors, seeded
                  from k_themes in gui_theme.c).  Vars: the font-derived metrics in s_style, read
                  live so a font change updates them.
        Stack  -- push_style_color / _var override a slot until the matching pop (pop takes a
                  count, like ImGui); nests via a saved-previous stack.  Reset empty each frame.
        Next   -- next_style_color / _var override a slot for just the next item, consumed at the
                  per-item resolve seam (no pop), exactly like next_item_flag.

    The seam is shared with the item-flag system (item_flags_resolve calls style_item_commit; the
    chrome reset calls style_chrome_reset), so colors / vars and flags all latch on the same
    once-per-widget boundary -- see the impure wrappers in stock/gui_adornment.c.

    The payoff is reach with no churn: the COL_*, WIDGET_*, and WIN_* vocabulary macros
    (style/gui_style.h) resolve through style_col / style_var, so every existing read site
    honors an override without changing a single widget.

    Included by gui_style.c after gui_theme.c -- s_style and GUI_COLOR (gui.h) are already
    visible.  The frame orchestrator drives the per-frame reset across the unit seam
    (gui_ctx_begin pairs ctx_new_frame with style_new_frame).

==============================================================================================*/
// clang-format off

/* Base value of a style var -- read live from the font-derived metrics so a font_load update flows
   through without re-seeding anything.  The single map from slot to s_style field, grouped by the
   two gui_style_t categories (see gui.h). */

static f32
style_var_base( gui_style_var_t v )
{
    switch ( v )
    {
        /* 1. METRICS */
        case GUI_VAR_LINE_SIZE:       return (f32)s_style.line_size;
        case GUI_VAR_WIDGET_GAP:      return (f32)s_style.widget_gap;
        case GUI_VAR_WIDGET_PAD:      return (f32)s_style.widget_pad;
        case GUI_VAR_MIN_CELL_W:      return (f32)s_style.min_cell_w;
        case GUI_VAR_WIN_BORDER:      return (f32)s_style.win_border;
        case GUI_VAR_WIN_TITLE_H:     return (f32)s_style.win_title_h;
        case GUI_VAR_CHECKBOX_SZ:     return (f32)s_style.checkbox_sz;
        case GUI_VAR_SLIDER_KNOB_W:   return (f32)s_style.slider_knob_w;

        /* 2. SKIN */
        case GUI_VAR_WIN_ROUNDING:    return (f32)s_style.win_rounding;
        case GUI_VAR_WIDGET_ROUNDING: return (f32)s_style.widget_rounding;
        case GUI_VAR_GRAB_ROUNDING:   return (f32)s_style.grab_rounding;
        case GUI_VAR_WIN_FOCUS_BORDER:return (f32)s_style.win_focus_border;
        case GUI_VAR_CHECK_STYLE:     return (f32)s_style.check_style;     /* enum-as-var: 0 tick / 1 disc / 2 cross */
        case GUI_VAR_BULLET_STYLE:    return (f32)s_style.bullet_style;    /* enum-as-var: 0 disc / 1 square */
        case GUI_VAR_ARROW_STYLE:     return (f32)s_style.arrow_style;     /* enum-as-var: 0 triangle / 1 chevron */
        case GUI_VAR_SEPARATOR_STYLE: return (f32)s_style.separator_style; /* enum-as-var: 0 solid / 1 dashed */
        case GUI_VAR_PROGRESS_STYLE:  return (f32)s_style.progress_style;  /* enum-as-var: 0 solid / 1 gradient */
        case GUI_VAR_SLIDER_KNOB:     return (f32)s_style.slider_knob;     /* enum-as-var: 0 bar / 1 circle */
        case GUI_VAR_MENU_CHECK:      return (f32)s_style.menu_check;      /* enum-as-var: 0 plain / 1 box */

        default:                      return 0.0f;
    }
}

/*==============================================================================================

    Style State -- THREE blocks, one flat working set.

      element -- the shared stratum: 4 metrics + the 4x4 role/state palette, laid out to match
                 gui_el_style_t exactly so the installed run IS that struct (gui_el_style()
                 hands back a typed view onto it).  The block that grows instances, so chrome
                 and a kit can each own one.  Poke-able between landings: REFILL_LANDING.
      token   -- chrome's PRIVATE residue: the colors with no role (window body, title bar,
                 caret, resize grip, focus / nav-capture rings) plus the user range.
      var     -- the gui_style_var_t metrics + skin knobs, f32 carried as raw bits.

    gui_col_t stays the PUBLIC push vocabulary over all three: a color name is a handle to one
    or more slots, and s_col_route (built from g_el_slot_map at bootstrap) says which.  Most
    names route to a single slot; the few the element map reuses for more than one cell
    (GUI_COL_TEXT covers TEXT idle/hot/active) route to all of them, so a push keeps reaching
    everything it reaches today.  The element-shaped COL_* macros skip the route entirely --
    they name a role and state, which IS the slot; the token macros go through style_col().

    Working set: the store with the push/pop stacks applied -- the value an unscoped read
    returns.  It lives in s_work (style/gui_style_block.c); nothing here owns storage.

    Stacks: saved (slot, previous) pairs so pop restores regardless of which slots a push
    touched.  One PUSH is one stack entry even when its name routes to several slots, so
    pop_style_color( 1 ) always undoes exactly one push_style_color.

    TWO stacks, one per public pop verb: pop_style_color and pop_style_var each pop their
    own pushes, so an interleaved push_color / push_var sequence unwinds correctly
    (the same reason Dear ImGui keeps two).

    Next-item layer: next_style_* queues (slot, value) pairs in `next`; at the per-item resolve
    seam they are WRITTEN THROUGH into the working set, and `item` remembers what each slot held
    so the write can be taken back when the item ends.  Same shape as a push, different lifetime.

    That write-through is the point: every override -- push, pop, next -- lands in the slot
    itself, so a resolved read is one indexed load with nothing to scan.  The cost moved to the
    writes, which are orders of magnitude rarer than the reads.

==============================================================================================*/

#define GUI_STYLE_STACK_DEPTH 32
#define STYLE_ROUTE_MAX       3    /* widest fan-out in g_el_slot_map (TEXT -> 3 cells) */

/* The element block's slot layout -- field order of gui_el_style_t, asserted below, so the
   installed run can be handed out as that struct with no copy or offset table. */
#define EL_SLOT_PAD       0
#define EL_SLOT_GAP       1
#define EL_SLOT_BORDER_W  2
#define EL_SLOT_LINE_H    3
#define EL_SLOT_COL       4
#define EL_SLOT_COUNT     ( EL_SLOT_COL + GUI_EL_ROLE_COUNT * GUI_EL_STATE_COUNT )
#define EL_COL_SLOT( role, state )  ( EL_SLOT_COL + (u32)( role ) * GUI_EL_STATE_COUNT + (u32)( state ) )

ORB_STATIC_ASSERT( sizeof( gui_el_style_t ) == EL_SLOT_COUNT * sizeof( u32 ),
                   "element block layout must mirror gui_el_style_t field order" );

/* f32 <-> raw bits, so one u32 slot space carries both value types. */
static inline u32 style_f32_bits( f32 f ) { union { f32 f; u32 u; } c = { .f = f }; return c.u; }
static inline f32 style_bits_f32( u32 u ) { union { f32 f; u32 u; } c = { .u = u }; return c.f; }

typedef struct { u16 slot; u32 val; } style_pair_t;   // s_next: the pending value

/* One public push: the slots its name routes to, what each held before, and the value it
   applied.  Keeping `cur` is what makes a set switch exact -- the freshly mirrored instance
   gets the live overrides re-applied over it, and each save is rebased to the new underlying
   value so the eventual pop restores the right thing. */
typedef struct
{
    u16 slot[ STYLE_ROUTE_MAX ];
    u32 prev[ STYLE_ROUTE_MAX ];
    u32 cur;
    u8  n;

} style_save_t;

/* One live next-item override: same three facts, one slot. */
typedef struct { u16 slot; u32 prev; u32 cur; } style_item_t;

typedef struct
{
    style_save_t save[ GUI_STYLE_STACK_DEPTH ];
    u32          sp;                                  // count of pushes, not index

} style_stack_t;

static style_stack_t s_col_stack;                     // push_style_color's stack
static style_stack_t s_var_stack;                     // push_style_var's stack

#define STYLE_NEXT_MAX ( GUI_COL_COUNT + GUI_VAR_COUNT )

static style_pair_t  s_next[ STYLE_NEXT_MAX ];        // queued by next_style_*: (slot, value)
static u32           s_next_n;
static style_item_t  s_item[ STYLE_NEXT_MAX ];        // live for this item
static u32           s_item_n;

/* The style SET stack -- which element instance the UI currently resolves through.  Set 0 is
   chrome's, always present; a kit creates its own and brackets its UI with it.  Depth is small
   because a set is a coarse scope (a window, a panel, a HUD), never a per-widget one. */
#define GUI_STYLE_SET_DEPTH 8

static u16 s_set_stack[ GUI_STYLE_SET_DEPTH ];        // saved instance per push
static u32 s_set_sp;                                  // count of pushes, not index

/* Where each block landed.  Cached at bootstrap; the only bases any read adds. */
static u16  s_el_blk = 0, s_el_base = 0;
static u16  s_tok_base  = 0;
static u16  s_var_base  = 0;
static bool s_blocks_ready = false;

/* gui_col_t -> the work slots it names.  Built at bootstrap by inverting g_el_slot_map, so the
   push vocabulary and the theme projection cannot drift apart. */
typedef struct { u16 slot[ STYLE_ROUTE_MAX ]; u8 n; } style_route_t;

static style_route_t s_col_route[ GUI_COL_COUNT ];

/*==============================================================================================

    The Style Mechanism -- read / push / pop / next over the one slot space.

    Over-deep pushes: the value is still written to the working set (so the UI renders correctly
    in all builds), and sp is still counted so push/pop stay paired.  The save record is only
    written when sp is within bounds -- an over-depth push cannot save and therefore cannot
    restore, which is the documented cap behaviour.  An ORB_ASSERT fires in debug builds so
    callers discover the imbalance immediately at the push site rather than on a silent bad
    restore.  pop takes a count, like ImGui.

==============================================================================================*/

/* This is what every COL_* / metric macro ultimately resolves to.  `slot` is ABSOLUTE (the
   block's base already added by the typed face below) -- the working set is one flat space
   across every registered block. */

static u32
style_read( u32 slot )
{
    return s_work[ slot ];   /* the installed value with every live override already applied */
}

/* Put a saved value back.  If a next-item override is live on that slot, the restore belongs
   UNDERNEATH it -- update what the item layer will put back and leave the override visible for
   the rest of the item, rather than letting a pop punch through the more specific scope.  Only
   pops pay for this scan, and pops are rare next to reads. */
static void
style_restore( u16 slot, u32 val )
{
    for ( u32 i = 0; i < s_item_n; ++i )
        if ( s_item[ i ].slot == slot ) { s_item[ i ].prev = val; return; }

    s_work[ slot ] = val;
}

/* Push one public override: every slot the name routes to takes the value, and the entry
   remembers all of them so the matching pop restores the lot. */

static void
style_push( style_stack_t* st, const u16* slot, u8 n, u32 val )
{
    ORB_ASSERT( st->sp < GUI_STYLE_STACK_DEPTH && "style push: stack overflow -- mismatched push/pop" );
    if ( st->sp < GUI_STYLE_STACK_DEPTH )
    {
        style_save_t* sv = &st->save[ st->sp ];
        sv->n   = n;
        sv->cur = val;
        for ( u8 i = 0; i < n; ++i )
        {
            sv->slot[ i ] = slot[ i ];
            sv->prev[ i ] = s_work[ slot[ i ] ];
        }
    }
    ++st->sp;

    for ( u8 i = 0; i < n; ++i ) s_work[ slot[ i ] ] = val;
}

static void
style_pop( style_stack_t* st, u32 count )
{
    while ( count-- && st->sp )
    {
        --st->sp;
        if ( st->sp < GUI_STYLE_STACK_DEPTH )
        {
            const style_save_t* sv = &st->save[ st->sp ];
            for ( u8 i = 0; i < sv->n; ++i ) style_restore( sv->slot[ i ], sv->prev[ i ] );
        }
    }
}

/* Queue a next-item override; replaces a pending entry for the same slot rather than stacking
   duplicates.  Consumed (promoted to the item layer) at the per-item resolve seam -- no pop. */
static void
style_next( u32 slot, u32 val )
{
    for ( u32 i = 0; i < s_next_n; ++i )
        if ( s_next[ i ].slot == (u16)slot ) { s_next[ i ].val = val; return; }
    if ( s_next_n < STYLE_NEXT_MAX )
        s_next[ s_next_n++ ] = ( style_pair_t ){ (u16)slot, val };
}

/*==============================================================================================
    The block accessors -- the ONLY places a base is added.

    A read site says COL_TEXT or WIDGET_PAD; it never sees a base or a route, which is what
    kept this whole split off the 199 chrome read sites.
==============================================================================================*/

/* The element stratum: one indexed load.  No projection, no override test, no fallback chain --
   the installed value and any override live in the SAME slot, which is what the split bought. */
static u32 el_col   ( u8 role, u8 state ) { return style_read( s_el_base + EL_COL_SLOT( role, state ) ); }
static f32 el_metric( u32 local )         { return style_bits_f32( style_read( s_el_base + local ) ); }

/*==============================================================================================
    Typed faces -- the public gui_col_t / gui_style_var_t vocabulary over the blocks.
==============================================================================================*/

/* The generic read: the FIRST slot a name routes to is its canonical value (for the multi-slot
   names every routed slot holds the same value anyway -- they are authored from one theme
   color and pushed together).  Low-frequency by design; the hot paths use el_col / tok_col. */
u32
style_col( gui_col_t slot )
{
    if ( slot >= GUI_COL_COUNT || s_col_route[ slot ].n == 0 ) return 0u;
    return style_read( s_col_route[ slot ].slot[ 0 ] );
}

static void style_push_color( gui_col_t slot, u32 abgr )
{
    if ( slot < GUI_COL_COUNT && s_col_route[ slot ].n )
         style_push( &s_col_stack, s_col_route[ slot ].slot, s_col_route[ slot ].n, abgr );
}

/* WIDGET_PAD / WIDGET_GAP name the ELEMENT metrics, not var-block slots: the element style owns
   the spacing the flow engine applies, so a read and a push have to land on the same slot (this
   is what the old style_el_metric compare-against-base hack was standing in for).  The rest are
   plain var slots, indexed 1:1 by the enum. */
static u16
style_var_slot( gui_style_var_t slot )
{
    if ( slot == GUI_VAR_WIDGET_PAD ) return (u16)( s_el_base + EL_SLOT_PAD );
    if ( slot == GUI_VAR_WIDGET_GAP ) return (u16)( s_el_base + EL_SLOT_GAP );
    return (u16)( s_var_base + (u32)slot );
}

f32 style_var( gui_style_var_t slot ) { return style_bits_f32( style_read( style_var_slot( slot ) ) ); }

void style_push_var( gui_style_var_t slot, f32 value )
{
    if ( slot < GUI_VAR_COUNT )
    {
        u16 s = style_var_slot( slot );
        style_push( &s_var_stack, &s, 1, style_f32_bits( value ) );
    }
}
static void style_pop_color( u32 count ) 
{ 
    style_pop( &s_col_stack, count ); 
}
void style_pop_var  ( u32 count ) 
{ 
    style_pop( &s_var_stack, count );
}

/*==============================================================================================
    Set next style color or var
==============================================================================================*/

static void style_next_color( gui_col_t slot, u32 abgr )
{
    if ( slot >= GUI_COL_COUNT ) return;
    for ( u8 i = 0; i < s_col_route[ slot ].n; ++i )
        style_next( s_col_route[ slot ].slot[ i ], abgr );
}
static void style_next_var( gui_style_var_t slot, f32 value )
{
    if ( slot < GUI_VAR_COUNT ) style_next( style_var_slot( slot ), style_f32_bits( value ) );
}

/*==============================================================================================
    Seam hooks -- driven from OUTSIDE this unit, at the shared per-item boundary.

    style never paints and never reaches up, so it does not call these itself: the impure
    wrappers in stock/gui_adornment.c (item_flags_resolve / item_flags_chrome_reset) run them
    alongside the flag half in core/gui_ctx.c, and the frame orchestrator runs style_new_frame
    alongside ctx_new_frame.  Flags, colors, and vars therefore all latch on one boundary.
==============================================================================================*/

/* Take back the live item overrides, newest first, so each slot ends up holding whatever it held
   before this item began.  Idempotent -- the commit and the chrome reset both lean on that,
   because nothing guarantees a reset between two consecutive items. */

static void
style_item_unwind( void )
{
    while ( s_item_n )
    {
        --s_item_n;
        s_work[ s_item[ s_item_n ].slot ] = s_item[ s_item_n ].prev;
    }
}

/* Write the pending next-item overrides into the working set, remembering what each displaced.
   Called once per widget from item_flags_resolve (stock/gui_adornment.c, cross-unit), so the
   override that next_style_* queued just before this widget applies for this widget's whole
   draw, then is gone for the following one -- the unwind first is what makes "the following
   one" true even when no chrome reset falls between them. */

void
style_item_commit( void )
{
    style_item_unwind();

    for ( u32 i = 0; i < s_next_n; ++i )
    {
        u16 slot = s_next[ i ].slot;

        s_item[ s_item_n++ ] = ( style_item_t ){ slot, s_work[ slot ], s_next[ i ].val };
        s_work[ slot ]       = s_next[ i ].val;
    }
    s_next_n = 0;
}

/* Drop the active per-item overrides before chrome draws.  Chrome (borders, scrollbars, titlebars)
   does not pass through the item seam, so without this it would inherit a lingering next-* override
   from the last body widget.  The push/pop stacks are intentionally left intact -- a push that
   brackets a window_begin / child_begin still applies to the chrome inside it, like ImGui. */

void
style_chrome_reset( void )
{
    style_item_unwind();
}

/*==============================================================================================
    Override replay -- what makes a reseed exact.

    A reseed (a set switch, or a landing) overwrites work slots with freshly installed values,
    wiping any override written over them.  Replay puts the live ones back, in the order they
    were applied, and REBASES each save to the value it now displaces -- without that rebase a
    later pop would restore a value belonging to the instance we just switched away from.

    Order: the two push stacks (no slot can appear in both -- colors and vars route to disjoint
    ranges), then the item layer last, since a next-item override is the most specific scope.
==============================================================================================*/

static void
style_stack_replay( style_stack_t* st, i32 only_blk )
{
    u32 n = ( st->sp < GUI_STYLE_STACK_DEPTH ) ? st->sp : GUI_STYLE_STACK_DEPTH;

    for ( u32 e = 0; e < n; ++e )
    {
        style_save_t* sv = &st->save[ e ];
        for ( u8 i = 0; i < sv->n; ++i )
        {
            if ( only_blk >= 0 && !style_slot_in_block( sv->slot[ i ], (u16)only_blk ) ) continue;

            sv->prev[ i ]        = s_work[ sv->slot[ i ] ];   /* rebase onto the new underlying value */
            s_work[ sv->slot[ i ] ] = sv->cur;
        }
    }
}

/* only_blk >= 0 replays just that block's slots (a set switch touched one run); -1 replays
   everything (a landing reseeded them all). */
static void
style_overrides_replay( i32 only_blk )
{
    style_stack_replay( &s_col_stack, only_blk );
    style_stack_replay( &s_var_stack, only_blk );

    for ( u32 i = 0; i < s_item_n; ++i )
    {
        if ( only_blk >= 0 && !style_slot_in_block( s_item[ i ].slot, (u16)only_blk ) ) continue;

        s_item[ i ].prev        = s_work[ s_item[ i ].slot ];
        s_work[ s_item[ i ].slot ] = s_item[ i ].cur;
    }
}

/*==============================================================================================
    The three blocks -- their layouts, their install fns, and the route that ties the public
    gui_col_t vocabulary to them.
==============================================================================================*/

/* THE role x state -> theme color projection: how a theme's flat palette compiles into the
   element stratum.  It runs at INSTALL time only (element_derive below) -- reads go straight
   to the element slots -- and it is inverted once at bootstrap to build the push route, so
   the two directions cannot drift.  Cells that name the same theme color are deliberate: a
   theme has no distinct authored value for them, and a push of that color reaches all of them. */
static const u8 g_el_slot_map[ GUI_EL_ROLE_COUNT ][ GUI_EL_STATE_COUNT ] =
{
    /*             IDLE                HOT                    ACTIVE              DIM                  */
    /* BG     */ { GUI_COL_WIDGET_BG,  GUI_COL_WIDGET_HOT,    GUI_COL_WIDGET_ACT, GUI_COL_CHILD_BG     },
    /* BORDER */ { GUI_COL_BORDER,     GUI_COL_WIDGET_FG,     GUI_COL_WIDGET_FG,  GUI_COL_BORDER       },
    /* TEXT   */ { GUI_COL_TEXT,       GUI_COL_TEXT,          GUI_COL_TEXT,       GUI_COL_TEXT_DIM     },
    /* ACCENT */ { GUI_COL_WIDGET_FG,  GUI_COL_NAV_HIGHLIGHT, GUI_COL_CHECK_MARK, GUI_COL_SLIDER_TRACK },
};

/* The token block: chrome's residue, the colors the element map never claims -- the ones with
   no role, so no kit ever instances them.  The array both DEFINES the block's slot order and
   feeds the route, so a token is added in exactly one place. */
static const u8 k_tok_col[] =
{
    GUI_COL_WINDOW_BG,   GUI_COL_TITLE_BG,  GUI_COL_RESIZE_HOT,  GUI_COL_INPUT_BG,
    GUI_COL_INPUT_FOCUS, GUI_COL_CURSOR,    GUI_COL_NAV_CAPTURE, GUI_COL_FOCUS_BORDER,
};

#define TOK_SLOT_COUNT ( sizeof( k_tok_col ) / sizeof( k_tok_col[ 0 ] ) )

/* Display names for the public color vocabulary, engine-owned so a style editor stops keeping
   a parallel array in step with an enum it does not own.  Designated by slot, so the entries
   cannot slide out of alignment the way a positional list can; a newly added color simply reads
   "?" until it is named here. */
static const char* const k_col_name[ GUI_COL_COUNT ] =
{
    [ GUI_COL_TEXT          ] = "Text",
    [ GUI_COL_TEXT_DIM      ] = "Text Dim",
    [ GUI_COL_WINDOW_BG     ] = "Window BG",
    [ GUI_COL_CHILD_BG      ] = "Child BG",
    [ GUI_COL_TITLE_BG      ] = "Title BG",
    [ GUI_COL_BORDER        ] = "Border",
    [ GUI_COL_WIDGET_BG     ] = "Widget BG",
    [ GUI_COL_WIDGET_HOT    ] = "Widget Hot",
    [ GUI_COL_WIDGET_ACT    ] = "Widget Active",
    [ GUI_COL_WIDGET_FG     ] = "Widget FG",
    [ GUI_COL_CHECK_MARK    ] = "Check Mark",
    [ GUI_COL_SLIDER_TRACK  ] = "Slider Track",
    [ GUI_COL_RESIZE_HOT    ] = "Resize Hot",
    [ GUI_COL_INPUT_BG      ] = "Input BG",
    [ GUI_COL_INPUT_FOCUS   ] = "Input Focus",
    [ GUI_COL_CURSOR        ] = "Cursor",
    [ GUI_COL_NAV_HIGHLIGHT ] = "Nav Highlight",
    [ GUI_COL_NAV_CAPTURE   ] = "Nav Capture",
    [ GUI_COL_FOCUS_BORDER  ] = "Focus Border",
};

const char*
gui_style_color_name( gui_col_t slot )
{
    return ( slot < GUI_COL_COUNT && k_col_name[ slot ] ) ? k_col_name[ slot ] : "?";
}

/* Per-SET owners of the element stratum.  Set 0 is chrome's and exists always; a kit takes a
   set of its own with gui_style_set_create and brackets its UI with it, so the two looks are
   installed side by side instead of overwriting one another.  A NULL owner just leaves the
   theme compile as the whole story for that set. */
static gui_style_source_fn s_set_source[ GUI_STYLE_SET_MAX ];
static void*               s_set_user  [ GUI_STYLE_SET_MAX ];
static u16                 s_set_count = 1;   /* set 0 is chrome's */

/* Which instance gui_el_style() hands out.  Normally the current set; during an install it is
   the set being filled, so a source writes its OWN look and not the one on screen. */
static i32 s_installing = -1;

/* element: compile the active theme into the stratum, then let this set's owner overwrite
   whatever it cares about (it writes through gui_el_style(), which points at this same run).
   Deriving FIRST is what lets a kit install only the row it owns -- the accent, say -- and
   inherit the rest of the theme instead of leaving stale values behind. */
static void
element_install( void* user, u32* dst, u16 count, u16 instance )
{
    UNUSED( user );
    UNUSED( count );

    const gui_style_t* s = style_active();   /* the ACTIVE (font-scaled) style, not the em=12 base */

    dst[ EL_SLOT_PAD      ] = style_f32_bits( (f32)s->widget_pad );
    dst[ EL_SLOT_GAP      ] = style_f32_bits( (f32)s->widget_gap );
    dst[ EL_SLOT_BORDER_W ] = style_f32_bits( (f32)s->win_border );
    dst[ EL_SLOT_LINE_H   ] = style_f32_bits( 0.0f );          /* live active-font basis */

    for ( u32 role = 0; role < GUI_EL_ROLE_COUNT; ++role )
        for ( u32 state = 0; state < GUI_EL_STATE_COUNT; ++state )
            dst[ EL_COL_SLOT( role, state ) ] = s->colors[ g_el_slot_map[ role ][ state ] ];

    if ( instance < GUI_STYLE_SET_MAX && s_set_source[ instance ] )
    {
        i32 saved    = s_installing;
        s_installing = (i32)instance;                          /* el_style() -> THIS set's run */
        s_set_source[ instance ]( s_set_user[ instance ] );
        s_installing = saved;
    }
}

static void
token_install( void* user, u32* dst, u16 count, u16 instance )
{
    UNUSED( user );
    UNUSED( count );
    UNUSED( instance );

    for ( u32 i = 0; i < TOK_SLOT_COUNT; ++i )
        dst[ i ] = s_style.colors[ k_tok_col[ i ] ];
}

static void
var_install( void* user, u32* dst, u16 count, u16 instance )
{
    UNUSED( user );
    UNUSED( count );
    UNUSED( instance );

    for ( u32 i = 0; i < GUI_VAR_COUNT; ++i )
        dst[ i ] = style_f32_bits( style_var_base( (gui_style_var_t)i ) );
}

/* Invert g_el_slot_map into the push route: every gui_col_t the map names collects the element
   slots that compiled from it; the rest take their token slot.  Derived, never authored, so a
   change to the map moves the reads and the pushes together. */
static void
style_route_build( void )
{
    for ( u32 role = 0; role < GUI_EL_ROLE_COUNT; ++role )
        for ( u32 state = 0; state < GUI_EL_STATE_COUNT; ++state )
        {
            style_route_t* r = &s_col_route[ g_el_slot_map[ role ][ state ] ];
            ORB_ASSERT( r->n < STYLE_ROUTE_MAX && "raise STYLE_ROUTE_MAX: a color names more cells" );
            if ( r->n < STYLE_ROUTE_MAX )
                r->slot[ r->n++ ] = (u16)( s_el_base + EL_COL_SLOT( role, state ) );
        }

    for ( u32 i = 0; i < TOK_SLOT_COUNT; ++i )
    {
        style_route_t* r = &s_col_route[ k_tok_col[ i ] ];
        ORB_ASSERT( r->n == 0 && "a color cannot be both an element cell and a chrome token" );
        r->slot[ 0 ] = (u16)( s_tok_base + i );
        r->n         = 1;
    }

    /* Every public name must land somewhere: an unrouted color reads as transparent and pushes
       nowhere, which is the one silent failure this two-table split can produce. */
    for ( u32 c = 0; c < GUI_COL_COUNT; ++c )
        ORB_ASSERT( s_col_route[ c ].n
                 && "gui_col_t with no slot -- add it to g_el_slot_map or k_tok_col" );
}

/* One-time registration, driven from style_new_frame rather than an init entry point: the unit
   has none, and new_frame is guaranteed to run before any read (gui_theme_set at gui_init calls
   it through gui_theme_reset).  Promote it to a real style_init when the orchestrator grows one. */

static void
style_blocks_bootstrap( void )
{
    if ( s_blocks_ready ) return;
    s_blocks_ready = true;

    s_el_blk  = style_block_register( &( style_block_desc_t ){
        .name = "element", .count = EL_SLOT_COUNT, .instances = GUI_STYLE_SET_MAX,
        .refill = STYLE_REFILL_LANDING, .install = element_install } );

    u16 tok_blk = style_block_register( &( style_block_desc_t ){
        .name = "token", .count = (u16)TOK_SLOT_COUNT, .instances = 1,
        .refill = STYLE_REFILL_FRAME, .install = token_install } );

    u16 var_blk = style_block_register( &( style_block_desc_t ){
        .name = "var", .count = GUI_VAR_COUNT, .instances = 1,
        .refill = STYLE_REFILL_FRAME, .install = var_install } );

    s_el_base  = style_block_work_base( s_el_blk );
    s_tok_base = style_block_work_base( tok_blk );
    s_var_base = style_block_work_base( var_blk );

    style_route_build();
}

/* A style LANDING: theme / font / scale changed, so every block re-derives -- including the
   element block, whose installed values are otherwise left alone so ad-hoc pokes survive.
   Driven across the unit seam by gui_style_apply (frame/gui_frame_font.c), after the metrics
   rescale, and by gui_style_source_set when an owner is registered or cleared.

   Exact mid-frame: the reseed wipes every live override, so the replay puts them back and
   rebases their saved values onto the freshly installed ones -- a push outstanding across a
   theme change still pops back to the right value. */

void
style_landing( void )
{
    style_blocks_bootstrap();
    style_store_refill( true );
    style_work_reseed();
    style_overrides_replay( -1 );
}

/* Reset the per-frame style state: refresh the blocks that mirror live state, re-seed the
   working set from the installed layer (so an unbalanced push cannot leak across frames), empty
   the stacks, and drop both next-item layers.  Called from gui_ctx_begin
   (frame/gui_frame_loop.c), paired with ctx_new_frame, and from gui_theme_reset.

   Order matters now that overrides are written through: the reseed has already overwritten
   every slot, so the live records are stale and their counts can simply be zeroed -- unwinding
   them afterwards would write last frame's values back over the fresh ones. */

void
style_new_frame( void )
{
    style_blocks_bootstrap();

    /* Back to chrome's set before the reseed, so an unbalanced style_set_push cannot carry a
       kit's look into the next frame any more than an unbalanced push_style_color can. */
    s_set_sp = 0;
    style_block_set_current( s_el_blk, 0 );

    style_store_refill( false );   /* live-sourced blocks only -- element pokes survive */
    style_work_reseed();           /* working set <- installed layer                    */

    s_col_stack.sp = s_var_stack.sp = 0;
    s_next_n = s_item_n = 0;
}

/*==============================================================================================
    The style SET -- which element instance the UI resolves through.

    Set 0 is chrome's, installed from the theme.  A kit takes one of its own and brackets its
    UI with it, so its look and chrome's are both installed and neither clobbers the other --
    the co-existence the single installed element style could not express.

    Switching is a mirror plus a replay, NOT a re-resolve: reads stay one indexed load, and the
    cost lands on the switch, which happens per window / panel rather than per widget.
==============================================================================================*/

gui_style_set_t
gui_style_set_create( gui_style_source_fn fn, void* user )
{
    style_blocks_bootstrap();

    bool have_room = s_set_count < style_block_instances( s_el_blk );

    ORB_ASSERT( have_room && "style sets exhausted -- raise GUI_STYLE_SET_MAX" );
    if ( !have_room )
        return GUI_STYLE_SET_DEFAULT;   /* chrome's set: a wrong look beats a corrupt index */

    gui_style_set_t set = (gui_style_set_t)s_set_count++;

    s_set_source[ set ] = fn;
    s_set_user  [ set ] = user;

    style_landing();                    /* fill the new instance now, alongside every other */
    if ( g_ctx ) gui_request_redraw();

    return set;
}

static void
style_set_activate( u16 inst )
{
    if ( inst == style_block_current( s_el_blk ) ) return;

    style_block_set_current( s_el_blk, inst );
    style_block_reseed( s_el_blk );                 /* work run <- that set's installed values */
    style_overrides_replay( (i32)s_el_blk );        /* live overrides back on top, rebased     */
}

void
gui_style_set_push( gui_style_set_t set )
{
    style_blocks_bootstrap();

    if ( (u32)set >= s_set_count ) set = 0;

    ORB_ASSERT( s_set_sp < GUI_STYLE_SET_DEPTH && "style set push: too deep -- mismatched push/pop" );
    if ( s_set_sp < GUI_STYLE_SET_DEPTH )
        s_set_stack[ s_set_sp ] = style_block_current( s_el_blk );
    ++s_set_sp;

    style_set_activate( (u16)set );
}

void
gui_style_set_pop( void )
{
    if ( !s_set_sp ) return;

    --s_set_sp;
    if ( s_set_sp < GUI_STYLE_SET_DEPTH )
        style_set_activate( s_set_stack[ s_set_sp ] );
}

gui_style_set_t gui_style_set_current( void ) { return (gui_style_set_t)style_block_current( s_el_blk ); }

/* Set-stack depth + unwind-to-depth: the containment pair a region uses, mirroring how it
   restores the id scope.  A region inherits the ambient set rather than choosing one -- that is
   what lets a caller bracket a whole window from outside it -- but an unbalanced push inside
   the region cannot escape it and restyle everything drawn after. */

u32
style_set_depth( void )
{
    return s_set_sp;
}

void
style_set_unwind( u32 depth )
{
    while ( s_set_sp > depth ) gui_style_set_pop();
}

/*==============================================================================================
    The element bridge -- the stratum's public reads.

    Nothing to reconcile any more: the installed element value and any push / next override
    occupy the SAME slot, so a read is one load.  The compare-resolved-against-the-theme-base
    guesswork these used to do (and its metric twin) is what the block split deleted -- along
    with its failure mode, where a push whose value happened to equal the theme's was silently
    dropped.

    (The vocabulary macros these back -- COL_*, WIDGET_*, ROUND_* -- are defined in
    style/gui_style.h, beside the cross-unit declarations, since every tier above reads them.)
==============================================================================================*/

u32 style_el_col( u8 role, u8 state ) { return el_col( role, state ); }

/* The metric twins, for the two spacing floats the rect dispatcher applies (cell_next_w's
   inter-cell gap + the region / label pad).  GUI_VAR_WIDGET_PAD / _GAP push onto these very
   slots (style_var_slot), so a scale_push and a kit's installed spacing meet in one place and
   zero means zero. */
f32 style_el_pad( void ) { return el_metric( EL_SLOT_PAD ); }
f32 style_el_gap( void ) { return el_metric( EL_SLOT_GAP ); }

/* border_w / line_h -- the other two element metrics, for the renders that inset by the frame
   line or measure against the text basis. */
f32 style_el_border_w( void ) { return el_metric( EL_SLOT_BORDER_W ); }

/*==============================================================================================
    The installed element style -- storage, the owner hook, and the typed view onto it.
==============================================================================================*/

/* Mutable access to the installed style: a typed view straight onto the element block's store
   run, which is laid out as this struct (asserted at the top of this file).  Writes land in the
   INSTALLED layer, so they survive the per-frame reseed and are re-derived at the next landing --
   the documented ad-hoc-poke contract.  A poke outside a landing becomes visible on the next
   frame's reseed; a source registered through gui_style_source_set lands immediately, because
   registering runs a landing. */
gui_el_style_t*
gui_el_style( void )
{
    style_blocks_bootstrap();   /* callable before the first frame */

    u16 inst = ( s_installing >= 0 ) ? (u16)s_installing : style_block_current( s_el_blk );
    return ( gui_el_style_t* )style_block_instance( s_el_blk, inst );
}

/* Own the DEFAULT set.  Set 0 is what chrome and any unbracketed UI resolve through, so a
   source installed here restyles the whole application -- the original meaning of this call,
   unchanged.  A kit that wants its look BESIDE chrome's takes gui_style_set_create instead. */
void
gui_style_source_set( gui_style_source_fn fn, void* user )
{
    style_blocks_bootstrap();

    s_set_source[ 0 ] = fn;
    s_set_user  [ 0 ] = user;

    style_landing();            /* promotion / restoration lands immediately */
    if ( g_ctx )                /* guard: callable before any context exists */
        gui_request_redraw();   /* the restyle must survive an idle frame    */
}

/*==============================================================================================
    State -> color projections -- style resolution proper.  The interact state arrives as a
    PARAMETER (the purity rule): these never query the interact server, so they resolve
    identically with no server present.
==============================================================================================*/

/* Frame-background tint for a "framed field" widget (checkbox box, slider track, drag box, input):
   hover / nav / active lift it to the shared hot / active palette entries -- one at a time, since
   hover and nav-highlight are mutually exclusive -- over a caller-supplied idle_color_enum base
   so each field keeps its own resting colour, matching how Dear ImGui's FrameBgHovered lifts every
   framed control, not just buttons. */

u32
col_frame_bg( gui_item_state_t st, u32 idle_color_enum )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;   /* nav cursor lights the body like a hover */
    return idle_color_enum;
}

/* Common case background color for a pushbutton / knob style widget.
   col_frame_bg with the plain widget background as the idle base. */
u32 col_item_bg( gui_item_state_t st )
{
    return col_frame_bg( st, COL_WIDGET_BG );
}

/* Animated background for a pushbutton-like widget: col_item_bg with the hover/active
   transitions smoothed through the animation service (core/gui_anim.c) -- the ONE projection
   that rides the interact server, explicitly: the damper is
   keyed retained state, so the caller passes the id and core owns the storage.  Two damper
   channels in one gui_anim4 slot -- a hot layer (hover / nav focus) at speed 10 and an active
   layer (pressed) at speed 20 -- both rest at 0 so they ramp up from the palette base; the
   spare two channels sit unused (0/0/0) and are free for a widget-specific flourish later.
   Composite over the palette: BG -> HOT by the hot channel, then that -> ACT by the active
   one.  The primitive owns all storage, settle, and wants_redraw bookkeeping in a single peek;
   an idle widget with no history lands on COL_WIDGET_BG. */

#define ANIM_TAG_BG  0xA501u   /* id_combine salt: keeps this slot distinct from all other per-widget state */

u32
col_item_bg_anim( gui_id_t id, gui_item_state_t st )
{
    gui_anim4_t rest   = { 0.0f, 0.0f, 0.0f, 0.0f };
    gui_anim4_t target = { ( st.hover || st.nav ) ? 1.0f : 0.0f, st.active ? 1.0f : 0.0f, 0.0f, 0.0f };
    gui_anim4_t speed  = { 10.0f, 20.0f, 0.0f, 0.0f };
    gui_anim4_t a      = gui_anim4( id_combine( id, ANIM_TAG_BG ), rest, target, speed );
    return col_lerp( col_lerp( COL_WIDGET_BG, COL_WIDGET_HOT, a.x ), COL_WIDGET_ACT, a.y );
}

/* True while NO ambient style scope is open -- the volatile-replay precondition check
   (chrome/widgets/gui_volatile.c) reads it through this predicate so the stack types stay
   private.  An idle-frame replay re-runs the callback outside whatever scope surrounded the
   original emit, so any of these would silently re-colour the block: a push, a live next-item
   override, or a style set the replay will not be inside.

   The set is the reason this is now the whole list rather than just the push stacks: a volatile
   widget emitted inside a kit's set would replay in chrome's, which is exactly the class of
   wrong the assert exists to catch. */
bool
style_stacks_empty( void )
{
    return s_col_stack.sp == 0 && s_var_stack.sp == 0 && s_item_n == 0 && s_set_sp == 0;
}

// clang-format on
/*============================================================================================*/
