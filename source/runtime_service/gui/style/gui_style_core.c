/*==============================================================================================

    runtime_service/gui/style/gui_style_core.c -- Style resolution: one schema, one slot space.

    The push-model theme override the widgets draw through, the ImGui PushStyleColor /
    PushStyleVar analogue.  Three layers resolve into the value a widget sees:

        Base   -- the installed style: the active theme compiled into this style SET's run, then
                  overwritten by whatever that set's source owns.
        Stack  -- push_style_color / _var override a slot until the matching pop (pop takes a
                  count, like ImGui); nests via a saved-previous stack.  Reset empty each frame.
        Next   -- next_style_color / _var override a slot for just the next item, consumed at the
                  per-item resolve seam (no pop), exactly like next_item_flag.

    The seam is shared with the item-flag system (item_flags_resolve calls style_item_commit; the
    chrome reset calls style_chrome_reset), so colors / vars and flags all latch on the same
    once-per-widget boundary -- see the impure wrappers in stock/gui_adornment.c.

    Included by gui_style.c after gui_theme.c -- s_style and GUI_COLOR (gui.h) are already
    visible.  The frame orchestrator drives the per-frame reset across the unit seam
    (gui_ctx_begin pairs ctx_new_frame with style_new_frame).

==============================================================================================*/
// clang-format off

/*==============================================================================================

    Style State -- one installed style per set, one flat working set.

    gui_style_t (gui.h) IS the storage, twice over: the installed layer is an ARRAY of it (one
    whole style per set, so a kit can be handed a typed pointer with no copy and no offset
    table), and the working set is the same struct's u32 image, so an override can address any
    field by slot.  The static assert below is what makes the two views one thing.  Three runs
    inside the struct, all equal citizens -- that equality is the whole point of the schema:

        col    [ role ][ phase ]  -- the 6x4 color grid, THE color vocabulary
        var    [ gui_style_var_t ]-- every scalar the style has, metrics and skin alike
        scales [ gui_scale_t ]    -- the density ramp scale_push reads

    An earlier design split these across three blocks with different instance counts, so a style
    set owned the colors and three metrics while chrome kept the rest.  That asymmetry is what
    made a kit's style a subset of chrome's instead of a peer.  Now a set owns the whole struct:
    chrome is simply the set at index 0.

    Two coordinate systems index the one space, both plain base-plus-offset -- no route table, no
    inversion, no name-to-slot map.  A color is ( role, phase ); a scalar is a gui_style_var_t.
    That is the entire addressing story.

    Working set: the installed layer with the push/pop stacks applied -- the value an unscoped
    read returns.  ONE run, always the current set's, so a read is s_work[ slot ] with a
    COMPILE-TIME slot: no block base to load, no instance arithmetic, nothing to scan.

    Stacks: saved (slot, previous) pairs so pop restores regardless of which slots a push
    touched.  One PUSH is one stack entry even when it spans a whole phase row (GUI_PHASE_ALL), so
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

/* The slot layout -- field order of gui_style_t.  Three runs, laid end to end. */
#define STYLE_COL_BASE    0
#define STYLE_COL_COUNT   ( GUI_ROLE_COUNT * GUI_PHASE_COUNT )
#define STYLE_VAR_BASE    ( STYLE_COL_BASE   + STYLE_COL_COUNT )
#define STYLE_SCALE_BASE  ( STYLE_VAR_BASE   + GUI_VAR_COUNT    )
#define STYLE_SCALE_COUNT ( GUI_SCALE_COUNT * 3 )                  /* row, pad, gap per step */
#define STYLE_SLOT_COUNT  ( STYLE_SCALE_BASE + STYLE_SCALE_COUNT )

#define STYLE_COL_SLOT( role, phase ) ( STYLE_COL_BASE + (u32)( role ) * GUI_PHASE_COUNT + (u32)( phase ) )

/* The load-bearing equivalence: the struct a theme is authored as and the flat run an override
   indexes are the SAME bytes.  Break the field order and this fires at compile time. */
ORB_STATIC_ASSERT( sizeof( gui_style_t ) == STYLE_SLOT_COUNT * sizeof( u32 ),
                   "style slot layout must mirror gui_style_t field order" );

/*  The two layers, and there are only two.

    s_store -- INSTALLED.  One complete gui_style_t per set: what the theme compiled plus what
               that set's source overwrote.  Written only by style_install; never by a push.
               Typed, so gui_style_edit() hands a kit &s_store[set] with no cast.
    s_work  -- RESOLVED.  The CURRENT set's installed values with every push / next override
               already written in.  The only array a read touches, and it is a file static of
               known size -- so COL_TEXT compiles to a load from a fixed address, which is the
               whole reason the sets live behind an index instead of the reads living behind a
               base pointer.

    Capacity is the array bound: a fifth set is a compile error, not a first-frame assert. */

static gui_style_t s_store[ GUI_STYLE_SET_MAX ];   // installed, one per set
static u32         s_work [ STYLE_SLOT_COUNT ];    // resolved -- what every read indexes
static u16         s_set_cur;                      // which set s_work currently mirrors

/* The widest fan-out one public push can have: a full phase row (GUI_PHASE_ALL). */
#define STYLE_FAN_MAX GUI_PHASE_COUNT

/* f32 <-> raw bits, so one u32 slot space carries both value types. */
static inline u32 style_f32_bits( f32 f ) { union { f32 f; u32 u; } c = { .f = f }; return c.u; }
static inline f32 style_bits_f32( u32 u ) { union { f32 f; u32 u; } c = { .u = u }; return c.f; }

typedef struct { u16 slot; u32 val; } style_pair_t;   // s_next: the pending value

/* One public push: the slots it spans, what each held before, and the value it applied.
   Keeping `cur` is what makes a set switch exact -- the freshly mirrored instance gets the live
   overrides re-applied over it, and each save is rebased to the new underlying value so the
   eventual pop restores the right thing. */
typedef struct
{
    u16 slot[ STYLE_FAN_MAX ];
    u32 prev[ STYLE_FAN_MAX ];
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

#define STYLE_NEXT_MAX ( STYLE_COL_COUNT + GUI_VAR_COUNT )

static style_pair_t  s_next[ STYLE_NEXT_MAX ];        // queued by next_style_*: (slot, value)
static u32           s_next_n;
static style_item_t  s_item[ STYLE_NEXT_MAX ];        // live for this item
static u32           s_item_n;

/* The style SET stack -- which instance the UI currently resolves through.  Set 0 is chrome's,
   always present; a kit creates its own and brackets its UI with it.  Depth is small because a
   set is a coarse scope (a window, a panel, a HUD), never a per-widget one. */
#define GUI_STYLE_SET_DEPTH 8

static u16 s_set_stack[ GUI_STYLE_SET_DEPTH ];        // saved set per push
static u32 s_set_sp;                                  // count of pushes, not index

/*==============================================================================================

    The Style Mechanism -- read / push / pop / next over the one slot space.

    Over-deep pushes: the value is still written to the working set (so the UI renders correctly
    in all builds), and sp is still counted so push/pop stay paired.  The save record is only
    written when sp is within bounds -- an over-depth push cannot save and therefore cannot
    restore, which is the documented cap behaviour.  An ORB_ASSERT fires in debug builds so
    callers discover the imbalance immediately at the push site rather than on a silent bad
    restore.  pop takes a count, like ImGui.

==============================================================================================*/

/* This is what every COL_* / metric macro ultimately resolves to: one indexed load of a static
   array at a slot the caller knew at compile time. */

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

/* Push one public override: every slot it spans takes the value, and the entry remembers all of
   them so the matching pop restores the lot. */

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
    The typed faces -- coordinate to slot, and nothing else.

    A read site says COL_TEXT or WIDGET_PAD; it never sees a slot, which is what kept the schema
    rewrites off the 230-odd chrome read sites.
==============================================================================================*/

static u16 style_col_slot( u8 role, u8 phase ) { return (u16)STYLE_COL_SLOT( role, phase ); }
static u16 style_var_slot( u32 var )           { return (u16)( STYLE_VAR_BASE + var ); }

/* One indexed load.  No projection, no override test, no fallback chain -- the installed value
   and any override live in the SAME slot, which is what the flat space bought. */
u32 style_col( u8 role, u8 phase ) { return style_read( style_col_slot( role, phase ) ); }

f32 style_var( gui_style_var_t var )
{
    if ( (u32)var >= GUI_VAR_COUNT ) return 0.0f;
    return style_bits_f32( style_read( style_var_slot( (u32)var ) ) );
}

/* The ramp, read from the working set like everything else so a kit's DENSE is a kit's own.
   scale_push turns a step into three var pushes; sz_scale_row reads one without pushing. */
f32
style_scale( gui_scale_t s, u32 field )
{
    if ( (u32)s >= GUI_SCALE_COUNT ) s = GUI_SCALE_STD;
    return style_bits_f32( style_read( STYLE_SCALE_BASE + (u32)s * 3u + field ) );
}

/* Collect the slots a public color push spans: one cell, or a whole phase row for GUI_PHASE_ALL. */
static u8
style_col_fan( u8 role, u8 phase, u16* out )
{
    if ( role >= GUI_ROLE_COUNT ) return 0;

    if ( phase == GUI_PHASE_ALL )
    {
        for ( u8 s = 0; s < GUI_PHASE_COUNT; ++s ) out[ s ] = style_col_slot( role, s );
        return GUI_PHASE_COUNT;
    }
    if ( phase < GUI_PHASE_COUNT ) { out[ 0 ] = style_col_slot( role, phase ); return 1; }

    return 0;
}

static void
style_push_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr )
{
    u16 slot[ STYLE_FAN_MAX ];
    u8  n = style_col_fan( (u8)role, (u8)phase, slot );
    if ( n ) style_push( &s_col_stack, slot, n, abgr );
}

void style_push_var( gui_style_var_t var, f32 value )
{
    if ( (u32)var < GUI_VAR_COUNT )
    {
        u16 s = style_var_slot( (u32)var );
        style_push( &s_var_stack, &s, 1, style_f32_bits( value ) );
    }
}

static void style_pop_color( u32 count ) { style_pop( &s_col_stack, count ); }
void        style_pop_var  ( u32 count ) { style_pop( &s_var_stack, count ); }

/*==============================================================================================
    Set next style color or var
==============================================================================================*/

static void
style_next_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr )
{
    u16 slot[ STYLE_FAN_MAX ];
    u8  n = style_col_fan( (u8)role, (u8)phase, slot );
    for ( u8 i = 0; i < n; ++i ) style_next( slot[ i ], abgr );
}

static void
style_next_var( gui_style_var_t var, f32 value )
{
    if ( (u32)var < GUI_VAR_COUNT ) style_next( style_var_slot( (u32)var ), style_f32_bits( value ) );
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

    Order: the two push stacks, then the item layer last, since a next-item override is the most
    specific scope.
==============================================================================================*/

static void
style_stack_replay( style_stack_t* st )
{
    u32 n = ( st->sp < GUI_STYLE_STACK_DEPTH ) ? st->sp : GUI_STYLE_STACK_DEPTH;

    for ( u32 e = 0; e < n; ++e )
    {
        style_save_t* sv = &st->save[ e ];
        for ( u8 i = 0; i < sv->n; ++i )
        {
            sv->prev[ i ]           = s_work[ sv->slot[ i ] ];   /* rebase onto the new value */
            s_work[ sv->slot[ i ] ] = sv->cur;
        }
    }
}

/* One run, so a set switch and a landing reseed exactly the same slots -- there is nothing to
   filter by, which is what the block layer used to need. */
static void
style_overrides_replay( void )
{
    style_stack_replay( &s_col_stack );
    style_stack_replay( &s_var_stack );

    for ( u32 i = 0; i < s_item_n; ++i )
    {
        s_item[ i ].prev           = s_work[ s_item[ i ].slot ];
        s_work[ s_item[ i ].slot ] = s_item[ i ].cur;
    }
}

/*==============================================================================================
    Display names -- engine-owned, so a style editor walks the schema instead of keeping a
    parallel table in step with enums it does not own.  Designated by index, so an entry cannot
    slide out of alignment the way a positional list can; a newly added role / phase / var simply
    reads "?" until it is named here.
==============================================================================================*/

static const char* const k_role_name[ GUI_ROLE_COUNT ] =
{
    [ GUI_ROLE_PANEL  ] = "Panel",
    [ GUI_ROLE_TITLE  ] = "Title",
    [ GUI_ROLE_BG     ] = "Control",
    [ GUI_ROLE_BORDER ] = "Border",
    [ GUI_ROLE_TEXT   ] = "Text",
    [ GUI_ROLE_ACCENT ] = "Accent",
    [ GUI_ROLE_GRAB   ] = "Grab",
};

static const char* const k_phase_name[ GUI_PHASE_COUNT ] =
{
    [ GUI_PHASE_IDLE   ] = "Idle",
    [ GUI_PHASE_HOT    ] = "Hot",
    [ GUI_PHASE_ACTIVE ] = "Active",
    [ GUI_PHASE_DIM    ] = "Dim",
};

/* The var axis is described once, in gui_theme.c's k_var table (name + class together), because
   metrics_compute needs the class and is included above this file.  These are the read doors. */

const char* gui_style_role_name ( gui_style_role_t r )  { return ( (u32)r < GUI_ROLE_COUNT  && k_role_name [ r ]       ) ? k_role_name [ r ]       : "?"; }
const char* gui_style_phase_name( gui_style_phase_t p ) { return ( (u32)p < GUI_PHASE_COUNT && k_phase_name[ p ]       ) ? k_phase_name[ p ]       : "?"; }
const char* gui_style_var_name  ( gui_style_var_t v )   { return ( (u32)v < GUI_VAR_COUNT   && k_var       [ v ].name  ) ? k_var       [ v ].name  : "?"; }
const char* gui_style_class_name( gui_style_class_t c ) { return ( (u32)c < GUI_CLASS_COUNT && k_class_name[ c ]       ) ? k_class_name[ c ]       : "?"; }

/* What KIND of number a var holds -- what the em rescale and the lattice snap branch on, and
   what lets a style editor group its sliders without a table of its own.  An out-of-range var
   reads as a SHAPE, the class that is neither scaled nor snapped, so a bad index changes nothing. */
gui_style_class_t
gui_style_var_class( gui_style_var_t v )
{
    return ( (u32)v < GUI_VAR_COUNT ) ? (gui_style_class_t)k_var[ v ].cls : GUI_CLASS_SHAPE;
}

/*==============================================================================================
    The installed layer -- how a set gets filled, and the two refresh steps over it.
==============================================================================================*/

/* Per-SET owners.  Set 0 is chrome's and exists always; a kit takes a set of its own with
   gui_style_set_create and brackets its UI with it, so the two looks are installed side by side
   instead of overwriting one another.  A NULL owner just leaves the theme as the whole story
   for that set. */
static gui_style_source_fn s_set_source[ GUI_STYLE_SET_MAX ];
static void*               s_set_user  [ GUI_STYLE_SET_MAX ];
static u16                 s_set_count = 1;   /* set 0 is chrome's */

/* Which instance gui_style_edit() hands out.  Normally the current set; during an install it is
   the set being filled, so a source writes its OWN look and not the one on screen. */
static i32 s_installing = -1;

/* Seed one set from the active theme, then let its owner overwrite whatever it cares about (the
   owner writes through gui_style_edit(), which points at this same struct).  The seed is one
   struct assignment because the theme and the installed layer are the same type.

   Seeding FIRST is what lets a kit install only the part it owns -- the accent row, say -- and
   inherit the rest of the theme instead of leaving stale values behind. */
static void
style_install( u16 set )
{
    s_store[ set ] = *style_active();   /* the ACTIVE (font-scaled) style, not the em=12 base */

    if ( s_set_source[ set ] )
    {
        i32 saved    = s_installing;
        s_installing = (i32)set;        /* style_edit() -> THIS set's struct */
        s_set_source[ set ]( s_set_user[ set ] );
        s_installing = saved;
    }
}

/* Mirror the current set into the working run, discarding whatever overrides sat over it.
   Every caller re-applies the live ones after (style_overrides_replay). */
static void
style_work_reseed( void )
{
    const u32* src = (const u32*)&s_store[ s_set_cur ];
    for ( u32 i = 0; i < STYLE_SLOT_COUNT; ++i ) s_work[ i ] = src[ i ];
}

/* A style LANDING: theme / font / scale changed, so EVERY set re-derives -- including ones not
   currently on screen, so a style_set_push later in the frame finds fresh values.  Driven across
   the unit seam by gui_style_apply (frame/gui_frame_font.c), after the metrics rescale, and by
   gui_style_source_set when an owner is registered or cleared.

   Exact mid-frame: the reseed wipes every live override, so the replay puts them back and
   rebases their saved values onto the freshly installed ones -- a push outstanding across a
   theme change still pops back to the right value. */

void
style_landing( void )
{
    for ( u16 set = 0; set < GUI_STYLE_SET_MAX; ++set ) style_install( set );

    style_work_reseed();
    style_overrides_replay();
}

/* Reset the per-frame style state: re-seed the working set from the installed layer (so an
   unbalanced push cannot leak across frames), empty the stacks, and drop both next-item layers.
   Called from gui_ctx_begin (frame/gui_frame_loop.c), paired with ctx_new_frame, and from
   gui_theme_reset.

   Order matters now that overrides are written through: the reseed has already overwritten
   every slot, so the live records are stale and their counts can simply be zeroed -- unwinding
   them afterwards would write last frame's values back over the fresh ones.

   The INSTALLED layer is deliberately left alone: only a landing re-derives it, which is what
   lets an ad-hoc gui_style_edit() poke survive to be picked up by the reseed below. */

void
style_new_frame( void )
{
    /* Back to chrome's set before the reseed, so an unbalanced style_set_push cannot carry a
       kit's look into the next frame any more than an unbalanced push_style_color can. */
    s_set_sp  = 0;
    s_set_cur = 0;

    style_work_reseed();           /* working set <- installed layer */

    s_col_stack.sp = s_var_stack.sp = 0;
    s_next_n = s_item_n = 0;
}

/*==============================================================================================
    The style SET -- which instance the UI resolves through.

    Set 0 is chrome's, installed from the theme.  A kit takes one of its own and brackets its
    UI with it, so its look and chrome's are both installed and neither clobbers the other.

    Switching is a mirror plus a replay, NOT a re-resolve: reads stay one indexed load, and the
    cost lands on the switch, which happens per window / panel rather than per widget.
==============================================================================================*/

gui_style_set_t
gui_style_set_create( gui_style_source_fn fn, void* user )
{
    bool have_room = s_set_count < GUI_STYLE_SET_MAX;

    ORB_ASSERT( have_room && "style sets exhausted -- raise GUI_STYLE_SET_MAX" );
    if ( !have_room )
        return GUI_STYLE_SET_DEFAULT;   /* chrome's set: a wrong look beats a corrupt index */

    gui_style_set_t set = (gui_style_set_t)s_set_count++;

    s_set_source[ set ] = fn;
    s_set_user  [ set ] = user;

    style_landing();                    /* fill the new set now, alongside every other */
    if ( g_ctx ) gui_request_redraw();

    return set;
}

static void
style_set_activate( u16 set )
{
    if ( set == s_set_cur ) return;

    s_set_cur = set;
    style_work_reseed();             /* work run <- that set's installed values */
    style_overrides_replay();        /* live overrides back on top, rebased     */
}

void
gui_style_set_push( gui_style_set_t set )
{
    if ( (u32)set >= s_set_count ) set = 0;

    ORB_ASSERT( s_set_sp < GUI_STYLE_SET_DEPTH && "style set push: too deep -- mismatched push/pop" );
    if ( s_set_sp < GUI_STYLE_SET_DEPTH )
        s_set_stack[ s_set_sp ] = s_set_cur;
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

gui_style_set_t gui_style_set_current( void ) { return (gui_style_set_t)s_set_cur; }

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
    The installed style -- the typed view a kit writes its look through.
==============================================================================================*/

/* Mutable access to the installed style: this set's struct, directly.  Writes land in the
   INSTALLED layer, so they survive the per-frame reseed and are re-derived at the next landing --
   the documented ad-hoc-poke contract.  A poke outside a landing becomes visible on the next
   frame's reseed; a source registered through gui_style_source_set lands immediately, because
   registering runs a landing.  Callable before the first frame: the store is zero-initialised
   static storage, so there is nothing to bootstrap. */
gui_style_t*
gui_style_edit( void )
{
    u16 set = ( s_installing >= 0 ) ? (u16)s_installing : s_set_cur;
    return &s_store[ set ];
}

/* Own the DEFAULT set.  Set 0 is what chrome and any unbracketed UI resolve through, so a
   source installed here restyles the whole application.  A kit that wants its look BESIDE
   chrome's takes gui_style_set_create instead. */
void
gui_style_source_set( gui_style_source_fn fn, void* user )
{
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
   hover / nav / active lift it to the shared hot / active cells -- one at a time, since hover and
   nav-highlight are mutually exclusive -- over a caller-supplied idle base so each field keeps its
   own resting colour, matching how Dear ImGui's FrameBgHovered lifts every framed control, not
   just buttons. */

u32
col_frame_bg( gui_item_state_t st, u32 idle_color )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;   /* nav cursor lights the body like a hover */
    return idle_color;
}

/* Common case background color for a pushbutton style widget.
   col_frame_bg with the plain widget background as the idle base. */
u32 col_item_bg( gui_item_state_t st )
{
    return col_frame_bg( st, COL_WIDGET_BG );
}

/* The movable part of a track control -- slider knob, scrollbar thumb -- off the GRAB row.
   Its own role rather than col_item_bg because a knob has TWO lifting neighbours: the track under
   it rides BG and the value fill beside it rides ACCENT, so a knob on either row matches one of
   them exactly in some phase (on BG it vanishes into the hovered track; on ACCENT into the fill).
   GRAB is authored per theme as the contrast anchor, opposite in polarity to the theme, which is
   what keeps the knob readable against both at once.  There is no DIM step here: a phase is
   selected from live interaction, and DIM is the deliberate inert face a render picks itself. */
u32 col_grab( gui_item_state_t st )
{
    return style_col( GUI_ROLE_GRAB, st.active           ? GUI_PHASE_ACTIVE
                                   : st.hover || st.nav  ? GUI_PHASE_HOT
                                                         : GUI_PHASE_IDLE );
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
   override, or a style set the replay will not be inside. */
bool
style_stacks_empty( void )
{
    return s_col_stack.sp == 0 && s_var_stack.sp == 0 && s_item_n == 0 && s_set_sp == 0;
}

// clang-format on
/*============================================================================================*/
