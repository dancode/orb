/*==============================================================================================

    gui/style/gui_style_core.c -- Style resolution: one schema, one slot space.

    The push-model theme override the widgets draw through, the ImGui PushStyleColor /
    PushStyleVar analogue.  Three layers resolve into the value a widget sees:

        Base   -- the installed style: the active theme compiled into this style SET's run, then
                  overwritten by whatever that set's source owns.

        Stack  -- push_style_color / _var override a slot until the matching pop (pop takes a
                  count, like ImGui); nests via a saved-previous stack.  Reset empty each frame.

        Next   -- next_style_color / _var override a slot for just the next item, consumed at the
                  per-item resolve seam (no pop), exactly like next_item_flag.
    
    The seam is shared with the item-flag system (item_flags_resolve calls style_item_commit; 
    the chrome reset calls style_chrome_reset), so colors / vars and flags all latch on the 
    same once-per-widget boundary -- see the impure wrappers in stock/gui_adornment.c.

    Included by gui_style.c after gui_theme.c -- s_style and GUI_COLOR (gui.h) are already
    visible.  The frame orchestrator drives the per-frame reset across the unit seam
    (gui_ctx_begin pairs ctx_new_frame with style_new_frame).

==============================================================================================*/
// clang-format off

/*==============================================================================================

    Style State -- one installed style per set, one flat working set.

    gui_style_t (gui.h) IS the storage, twice over: the installed layer is an ARRAY of it 
    (one whole style per set, a kit can be handed a typed pointer with no copy and no offset table), 
    and the working set is the same struct's u32 image, so an override can address any
    field by slot.  The static assert below is what makes the two views one thing.  Four runs
    inside the struct, all equal citizens -- that equality is the whole point of the schema:

        palette                     -- The AUTHORED colour: seven seeds, a six-number ramp, and
                                       the four reserved severity colours
        col [ role ][ phase ]       -- The 10x4 grid DERIVED from it, THE color vocabulary
        face[ role ][ phase ]       -- The same grid again, as brush HANDLES: art that replaces
                                       the flat fill for a cell (0 = none, the default)
        var    [ gui_style_var_t ]  -- Every scalar the style has, metrics and skin alike
        scales [ gui_scale_t ]      -- The density ramp scale_push reads
    
    An earlier design split these across three blocks with different instance counts, so a style
    set owned the colors and three metrics while chrome kept the rest.  That asymmetry is what
    made a kit's style a subset of chrome's instead of a peer.  Now a set owns the whole struct:
    chrome is simply the set at index 0.

    Two coordinate systems index the one space, both plain base-plus-offset -- no route table, no
    inversion, no name-to-slot map.  A color OR A FACE is ( role, phase ) -- the same coordinate
    into two parallel runs, which is the whole reason a face costs no new machinery; a scalar is a
    gui_style_var_t.  That is the entire addressing story.  There is no SELECTED plane to index:
    a selected read washes whatever ( role, phase ) already resolved to (style_wash_selected),
    live, rather than naming a second stored cell.

    The palette is the ONE run whose slots are not read directly by anything above: a render never
    asks for a seed, it asks for a cell.  The seeds live in the slot space anyway so that a seed
    push is an override like any other -- saved, replayed and popped by the same machinery -- and
    so that a set switch carries a kit's authored colour along with its derived colour.

    Working set: the installed layer with the push/pop stacks applied -- the value an unscoped
    read returns.  ONE run, always the current set's, so a read is s_work.slot[ slot ] with a
    COMPILE-TIME slot: no block base to load, no instance arithmetic, nothing to scan.

    Stacks: saved (slot, previous) pairs so pop restores regardless of which slots a push
    touched.  One PUSH is one stack entry even when it spans a whole phase row (GUI_PHASE_ALL), so
    pop_style_color( 1 ) always undoes exactly one push_style_color.

    FOUR stacks, one per public pop verb: pop_style_color, pop_style_face, pop_style_var and
    pop_style_seed each pop their own pushes, so an interleaved sequence unwinds correctly (the
    same reason Dear ImGui keeps two).  The seed stack is separate for a second reason as well
    -- its entries are an order of magnitude wider, since one seed push displaces the whole
    derived grid, and there is no reason to make the var stack pay for a fan it never uses.

    Next-item layer: next_style_* queues (slot, value) pairs in `next`; at the per-item resolve
    seam they are WRITTEN THROUGH into the working set, and `item` remembers what each slot held
    so the write can be taken back when the item ends.  Same shape as a push, different lifetime.

    That write-through is the point: every override -- push, pop, next -- lands in the slot
    itself, so a resolved read is one indexed load with nothing to scan.  The cost moved to the
    writes, which are orders of magnitude rarer than the reads.

==============================================================================================*/

#define GUI_STYLE_STACK_DEPTH 32

/* The style system also treats gui_style_t, the whole thing, as one flat array of u32 slots,
   so a single push/pop/override mechanism can address any field uniformly by index -- no
   separate code path per field type.

   So the slot layout: the field order of gui_style_t, as runs laid end to end.  The palette
   runs FIRST because it runs first in the pipeline: seeds and ramp are what a theme authors,
   the colour grid is what the bake derives from them, and the metrics follow. */

#define STYLE_SEED_BASE   0                                         // seed[GUI_SEED_COUNT] starts at 0
#define STYLE_RAMP_BASE   ( STYLE_SEED_BASE  + GUI_SEED_COUNT )     // ramp[] starts right after seed[]

/* palette.ext[GUI_EXT_RESERVED_COUNT] sits here in the struct -- right after ramp[], since it is
   the third and last field of gui_palette_t -- but earns no STYLE_xxx_SLOT of its own: nothing
   addresses "the authored default" by slot, only style.ext[] (STYLE_EXT_BASE, below) which is
   what a bake copies it INTO and what style_ext / push_style_ext actually read.  The four u32s
   still have to be counted here, or every base after them is wrong by four. */
#define STYLE_COL_BASE    ( STYLE_RAMP_BASE  + GUI_RAMP_COUNT + GUI_EXT_RESERVED_COUNT )   // col[][] starts after palette

#define STYLE_COL_COUNT   ( GUI_ROLE_COUNT   * GUI_PHASE_COUNT )    // full size of col[][]

#define STYLE_FACE_BASE   ( STYLE_COL_BASE   + STYLE_COL_COUNT )    // face[][] starts right after col[][]
#define STYLE_EXT_BASE    ( STYLE_FACE_BASE  + STYLE_COL_COUNT )    // ext[] starts right after face[][]
#define STYLE_VAR_BASE    ( STYLE_EXT_BASE   + GUI_STYLE_EXT_MAX )  // var[] starts right after ext[]
#define STYLE_SCALE_BASE  ( STYLE_VAR_BASE   + GUI_VAR_COUNT )      // scales[] starts right after var[]
#define STYLE_SCALE_COUNT ( GUI_SCALE_COUNT  * 3 )                  // each scale step is 3 f32s: row, pad, gap
#define STYLE_SLOT_COUNT  ( STYLE_SCALE_BASE + STYLE_SCALE_COUNT )  // total size = end of the last run

#define STYLE_COL_SLOT( role, phase )  ( STYLE_COL_BASE  + (u32)( role ) * GUI_PHASE_COUNT + (u32)( phase ) )

/* The face plane is the colour plane's exact shape one base along, so ONE offset expression
   serves both -- a face cell and its colour cell are always the same distance apart, which is
   what lets the fan collector below emit either simply by picking a base. */

#define STYLE_FACE_SLOT( role, phase ) ( STYLE_FACE_BASE + (u32)( role ) * GUI_PHASE_COUNT + (u32)( phase ) )

/* The load-bearing equivalence: the struct a theme is authored as and the flat run an override
   indexes are the SAME bytes.  Break the field order and this fires at compile time. */

ORB_STATIC_ASSERT( sizeof( gui_style_t ) == STYLE_SLOT_COUNT * sizeof( u32 ),
                   "style slot layout must mirror gui_style_t field order" );

/*==============================================================================================
    Style layers -- The two layers:

    s_store -- INSTALLED. One complete gui_style_t per set: what the theme compiled plus what
               that set's source overwrote.  Written only by style_install; never by a push.
               Typed, so gui_style_edit() hands a kit &s_store[set] with no cast.
    s_work  -- RESOLVED.  The CURRENT set's installed values with every push / next override
               already written in.  The only array a read touches, and it is a file static of
               known size -- so COL_TEXT_PRIMARY_IDLE compiles to a load from a fixed address,
               which is the whole reason the sets live behind an index instead of the reads
               living behind a base pointer.

    Capacity is the array bound: a fifth set is a compile error, not a first-frame assert.
==============================================================================================*/

static gui_style_t s_store[ GUI_STYLE_SET_MAX ];   // installed, one per set

/*==============================================================================================
    The resolved run, in BOTH of its views at once.  A read indexes .slot; the bake writes 
    through .style.  The union is not a convenience -- it is the static assert made usable:
    "the struct IS the storage" stops being a claim the reader has to verify and becomes the
    declaration itself, and re-deriving the grid after a seed push is 
    gui_style_bake( &s_work.style ) rather than a pointer cast the next reader has to talk
    themselves into.
==============================================================================================*/

static union
{
    gui_style_t style;                      // typed view -- what the bake reads and writes
    u32         slot[ STYLE_SLOT_COUNT ];   // flat view  -- what every style read indexes

} s_work;

static u16 s_set_cur;                       // which set s_work currently mirrors

/*============================================================================================*/
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
static style_stack_t s_face_stack;                    // push_style_face's stack
static style_stack_t s_ext_stack;                     // push_style_ext's stack

/* The brush BODIES, per set.  Only the HANDLE lives in the slot space (gui_style_t.face), which is
   the split that makes the face plane free: a handle is a u32 like every other slot, so push /
   pop / next / set-switch / replay all work on it with no new machinery, while the brush itself --
   24 bytes, registered once, named from many cells -- stays out of a run that is copied wholesale
   on every set switch and every landing.

   Indexed [set][handle - 1]; handle 0 is GUI_FACE_NONE and never resolves.  Per SET rather than
   global so a kit's art is its own: a handle only means something inside the set that issued it,
   exactly as a colour cell only means something inside the set that authored it. */

static gui_brush_t s_brush  [ GUI_STYLE_SET_MAX ][ GUI_STYLE_BRUSH_MAX ];
static u32         s_brush_n[ GUI_STYLE_SET_MAX ];

/* Extended-palette registration count, per set -- the brush pool's exact shape, minus the body
   array: an ext value lives in the slot space itself (gui_style_t.ext), so there is nothing here
   to store beside the count.  Starts each landing at GUI_EXT_RESERVED_COUNT, not 0: those slots
   are the standard severity colours, authored by the theme, and never up for re-registration. */
static u32 s_ext_n[ GUI_STYLE_SET_MAX ];

/* The SEED stack -- push_style_seed's, and a different shape from the two above because a seed
   push is a different KIND of override.  A colour push replaces a value; a seed push replaces a
   SOURCE, and the whole grid is re-derived from it -- so the entry has to remember the grid it
   displaced, not the one slot it named.  Slots are not stored: they are always the seed plus the
   contiguous colour run, known at compile time.

   Depth 8, not 32: a seed push is a coarse scope -- a panel, a HUD, a dialog -- exactly like a
   style set, and for the same reason.  Nobody brackets a single widget with a re-derivation. */

#define GUI_STYLE_SEED_DEPTH 8

typedef struct
{
    u32 prev_col[ STYLE_COL_COUNT ];   // the whole derived grid, as it stood before the re-bake
    u32 prev_seed;                     // what the named seed held
    u32 cur;                           // what it was set to (replay re-applies this)
    u8  seed;                          // which seed this push named

} style_seed_save_t;

static style_seed_save_t s_seed_stack[ GUI_STYLE_SEED_DEPTH ];
static u32               s_seed_sp;                   // count of pushes, not index

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
    return s_work.slot[ slot ];   /* the installed value with every live override already applied */
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

    s_work.slot[ slot ] = val;
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
            sv->prev[ i ] = s_work.slot[ slot[ i ] ];
        }
    }
    ++st->sp;

    for ( u8 i = 0; i < n; ++i ) s_work.slot[ slot[ i ] ] = val;
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

    A read site says COL_TEXT_PRIMARY_IDLE or WIDGET_PAD; it never sees a slot, which is what kept
    the schema rewrites off the 230-odd chrome read sites.
==============================================================================================*/

static u16 style_col_slot ( u8 role, u8 phase ) { return (u16)STYLE_COL_SLOT ( role, phase ); }
static u16 style_face_slot( u8 role, u8 phase ) { return (u16)STYLE_FACE_SLOT( role, phase ); }
static u16 style_var_slot ( u32 var )           { return (u16)( STYLE_VAR_BASE + var ); }
static u16 style_ext_slot ( u32 ext )           { return (u16)( STYLE_EXT_BASE + ext ); }

/* One indexed load.  No projection, no override test, no fallback chain -- the installed value
   and any override live in the SAME slot, which is what the flat space bought. */
u32 style_col( u8 role, u8 phase ) { return style_read( style_col_slot( role, phase ) ); }

/* The FACE read: the brush a cell names, or NULL when the cell names none -- which is the answer
   for every cell of every theme that authors no art, and is why "has this cell a face?" costs one
   indexed load and a compare against zero on the way to the ordinary colour fill.

   Returns a pointer INTO the set's pool.  Valid until the pool is rewritten (a landing, since a
   source re-registers its brushes), which is well inside one frame's paint -- callers use it and
   drop it, exactly as they use a resolved colour. */
const gui_brush_t*
style_face( u8 role, u8 phase )
{
    if ( role >= GUI_ROLE_COUNT || phase >= GUI_PHASE_COUNT )
        return NULL;

    u32 h = style_read( style_face_slot( role, phase ) );
    if ( h == GUI_FACE_NONE || h > s_brush_n[ s_set_cur ] )
        return NULL;                 /* no face, or a handle from a set that no longer owns it */

    return &s_brush[ s_set_cur ][ h - 1u ];
}

/* Wash a resolved colour toward the theme's accent by GUI_RAMP_SELECT * travel, spending
   bake_wash (style/gui_bake.c, unity-included above this file) -- the same formula the bake
   itself derives the grid with, so the two never disagree.  Reads the active set's ramp and
   accent straight out of the slot space -- no new storage, since a palette's seeds and ramp are
   already live slots (STYLE_SEED_BASE / STYLE_RAMP_BASE). */
u32
style_wash_selected( u32 color, f32 travel )
{
    if ( travel <= 0.0f ) return color;

    f32 t      = travel * style_bits_f32( style_read( STYLE_RAMP_BASE + GUI_RAMP_SELECT ) );
    u32 accent = style_read( STYLE_SEED_BASE + GUI_SEED_ACCENT );
    return bake_wash( color, t, accent );
}

u32 style_col_selected( u8 role, u8 phase ) { return style_wash_selected( style_col( role, phase ), 1.0f ); }

/* The extended-palette read: one indexed load, exactly like style_col, but at a flat slot with no
   phase axis to fan across.  Out-of-range (never registered this landing, or never registered at
   all) reads 0 rather than asserting -- a caller cannot tell "not registered" from "registered
   transparent black", which is fine, since neither is a colour anyone painted with on purpose. */
u32
style_ext( gui_style_ext_t ext )
{
    if ( (u32)ext >= GUI_STYLE_EXT_MAX ) return 0;
    return style_read( style_ext_slot( (u32)ext ) );
}

void
style_push_ext( gui_style_ext_t ext, u32 abgr )
{
    if ( (u32)ext < GUI_STYLE_EXT_MAX )
    {
        u16 s = style_ext_slot( (u32)ext );
        style_push( &s_ext_stack, &s, 1, abgr );
    }
}

void style_pop_ext( u32 count ) { style_pop( &s_ext_stack, count ); }

f32 style_var( gui_style_var_t var )
{
    if ( (u32)var >= GUI_VAR_COUNT ) return 0.0f;
    return style_bits_f32( style_read( style_var_slot( (u32)var ) ) );
}

/* A SHAPE pick, read back as the enum it is.  The GUI_CLASS_SHAPE vars carry a small enum in an
   f32 slot -- the price of one uniform var space -- and getting it back out is a ROUNDING, not a
   truncation and not a threshold.

   It earns a function because the seven read sites had grown FOUR different spellings of it:
   `style_var( x ) >= 0.5f` (five sites), `(u32)style_var( x ) == ENUM` (one), and
   `(u32)( style_var( x ) + 0.5f )` (one).  Only the last is correct, and the other two are each
   wrong in their own way.  The truncating cast turns a slot holding 0.999999 -- which is what an
   f32 round-trip through a style editor or an interpolated push can leave -- into pick 0, so a
   theme silently renders the wrong glyph.  The 0.5f threshold is a BOOLEAN test wearing a pick's
   clothing: it is only correct while a var has exactly two values, and CHECK_SHAPE already has
   three (tick / disc / cross), which is precisely why that one site had to spell it differently.
   One accessor, rounding once, and a pick can grow a third value without hunting call sites. */
u32
style_shape( gui_style_var_t var )
{
    f32 v = style_var( var );
    return ( v <= 0.0f ) ? 0u : (u32)( v + 0.5f );   /* negatives clamp rather than wrap huge */
}

/* The ramp, read from the working set like everything else so a kit's DENSE is a kit's own.
   scale_push turns a step into three var pushes; sz_scale_row reads one without pushing. */
f32
style_scale( gui_scale_t s, u32 field )
{
    if ( (u32)s >= GUI_SCALE_COUNT ) s = GUI_SCALE_STD;
    return style_bits_f32( style_read( STYLE_SCALE_BASE + (u32)s * 3u + field ) );
}

/* Collect the slots a public grid push spans.  GUI_PHASE_ALL is the one "whole axis" selector,
   so the fan is one cell up to one whole phase row.

   `base` picks WHICH grid: the colour plane or the face plane.  They have identical shape, so one
   collector serves both and a face push is a colour push that landed in a different run -- there
   is no second fan to keep in step. */
static u8
style_grid_fan( u32 base, u8 role, u8 phase, u16* out )
{
    if ( role >= GUI_ROLE_COUNT || phase > GUI_PHASE_ALL ) return 0;

    const u8 p0 = ( phase == GUI_PHASE_ALL ) ? 0 : phase;
    const u8 p1 = ( phase == GUI_PHASE_ALL ) ? GUI_PHASE_COUNT : (u8)( phase + 1 );

    u8 n = 0;
    for ( u8 p = p0; p < p1; ++p )
        out[ n++ ] = (u16)( base + (u32)role * GUI_PHASE_COUNT + p );

    return n;
}

static void
style_push_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr )
{
    u16 slot[ STYLE_FAN_MAX ];
    u8  n = style_grid_fan( STYLE_COL_BASE, (u8)role, (u8)phase, slot );
    if ( n ) style_push( &s_col_stack, slot, n, abgr );
}

/* Scope a FACE over the same rectangle a colour push scopes.  Its own stack (the house rule: one
   stack per public pop verb) so an interleaved colour / face / var sequence unwinds correctly. */
static void
style_push_face( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face )
{
    u16 slot[ STYLE_FAN_MAX ];
    u8  n = style_grid_fan( STYLE_FACE_BASE, (u8)role, (u8)phase, slot );
    if ( n ) style_push( &s_face_stack, slot, n, face );
}

static void style_pop_face( u32 count ) { style_pop( &s_face_stack, count ); }

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
    u8  n = style_grid_fan( STYLE_COL_BASE, (u8)role, (u8)phase, slot );
    for ( u8 i = 0; i < n; ++i ) style_next( slot[ i ], abgr );
}

static void
style_next_face( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face )
{
    u16 slot[ STYLE_FAN_MAX ];
    u8  n = style_grid_fan( STYLE_FACE_BASE, (u8)role, (u8)phase, slot );
    for ( u8 i = 0; i < n; ++i ) style_next( slot[ i ], face );
}

static void
style_next_var( gui_style_var_t var, f32 value )
{
    if ( (u32)var < GUI_VAR_COUNT ) style_next( style_var_slot( (u32)var ), style_f32_bits( value ) );
}

/*==============================================================================================
    The seed push -- re-derive rather than replace.

    The verb GUI_PHASE_ALL could never be.  A phase-row push writes one value into four cells,
    which flattens the ramp: pushed on BG it hands you a button that stops reacting to hover.
    A seed push changes the SOURCE and re-runs the bake, so the four cells stay four colours a
    ramp step apart -- just built from somewhere else.  That is what "recolour this panel" means
    nearly every time it is asked for, and it had no verb at all until the seeds existed.

    Cost lands where it belongs.  A re-bake is 40 derived cells plus 40 saved ones, paid once per
    push; reads stay one indexed load, exactly as before, because the derived values land in the
    same slots any other override writes.  Re-seeding the ACCENT moves the selected wash too
    (style_wash_selected reads the same live slot), which is the behaviour you want and could not
    have got from any number of colour pushes.
==============================================================================================*/

/* Re-derive the grid from the working run's own palette.  This is the union earning its keep:
   .style and .slot are the same bytes, so the bake reads the seeds a push just wrote and writes
   the cells the next read will index, with no copy in or out. */
static void
style_rebake( void )
{
    gui_style_bake( &s_work.style );
}

static void
style_push_seed( gui_style_seed_t seed, u32 abgr )
{
    if ( (u32)seed >= GUI_SEED_COUNT ) return;

    ORB_ASSERT( s_seed_sp < GUI_STYLE_SEED_DEPTH && "style seed push: too deep -- mismatched push/pop" );
    if ( s_seed_sp < GUI_STYLE_SEED_DEPTH )
    {
        style_seed_save_t* sv = &s_seed_stack[ s_seed_sp ];

        sv->seed      = (u8)seed;
        sv->cur       = abgr;
        sv->prev_seed = s_work.slot[ STYLE_SEED_BASE + (u32)seed ];

        for ( u32 i = 0; i < STYLE_COL_COUNT; ++i )
            sv->prev_col[ i ] = s_work.slot[ STYLE_COL_BASE + i ];
    }
    ++s_seed_sp;

    s_work.slot[ STYLE_SEED_BASE + (u32)seed ] = abgr;
    style_rebake();
}

/* Put back the seed AND the grid it displaced.  The cells go back through style_restore, not by
   plain assignment, so a next-item colour override live at the moment of the pop keeps its slot
   -- the same rule a colour pop already follows, for the same reason: a pop must not punch
   through a more specific scope. */
static void
style_pop_seed( u32 count )
{
    while ( count-- && s_seed_sp )
    {
        --s_seed_sp;
        if ( s_seed_sp < GUI_STYLE_SEED_DEPTH )
        {
            const style_seed_save_t* sv = &s_seed_stack[ s_seed_sp ];

            s_work.slot[ STYLE_SEED_BASE + sv->seed ] = sv->prev_seed;

            for ( u32 i = 0; i < STYLE_COL_COUNT; ++i )
                style_restore( (u16)( STYLE_COL_BASE + i ), sv->prev_col[ i ] );
        }
    }
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
        s_work.slot[ s_item[ s_item_n ].slot ] = s_item[ s_item_n ].prev;
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

        s_item[ s_item_n++ ] = ( style_item_t ){ slot, s_work.slot[ slot ], s_next[ i ].val };
        s_work.slot[ slot ]       = s_next[ i ].val;
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
            sv->prev[ i ]           = s_work.slot[ sv->slot[ i ] ];   /* rebase onto the new value */
            s_work.slot[ sv->slot[ i ] ] = sv->cur;
        }
    }
}

/* Seeds replay the same way, but a step further: re-applying the seed is not enough, the grid
   has to be DERIVED again -- against the palette the new instance brought with it, which is the
   whole point of pushing a seed rather than 40 cells.  Push gold over chrome's blue and switch to
   a kit's ember set, and the cells land on ember-plus-gold, not on chrome's. */
static void
style_seed_replay( void )
{
    u32 n = ( s_seed_sp < GUI_STYLE_SEED_DEPTH ) ? s_seed_sp : GUI_STYLE_SEED_DEPTH;

    for ( u32 e = 0; e < n; ++e )
    {
        style_seed_save_t* sv = &s_seed_stack[ e ];

        sv->prev_seed = s_work.slot[ STYLE_SEED_BASE + sv->seed ];   /* rebase onto the new value */
        for ( u32 i = 0; i < STYLE_COL_COUNT; ++i )
            sv->prev_col[ i ] = s_work.slot[ STYLE_COL_BASE + i ];

        s_work.slot[ STYLE_SEED_BASE + sv->seed ] = sv->cur;
        style_rebake();
    }
}

/* One run, so a set switch and a landing reseed exactly the same slots -- there is nothing to
   filter by, which is what the block layer used to need.

   Seeds go FIRST, and that ordering is a rule rather than an accident: a seed push re-derives
   every cell, so it supersedes any per-cell colour push that was outstanding when it landed.
   Replaying seeds first reproduces the nesting that verb is actually used in -- bracket a region
   with a seed, then tweak individual cells inside it -- and leaves the per-cell pushes on top
   where the caller put them. */
static void
style_overrides_replay( void )
{
    style_seed_replay();
    style_stack_replay( &s_col_stack );
    style_stack_replay( &s_face_stack );
    style_stack_replay( &s_var_stack );

    for ( u32 i = 0; i < s_item_n; ++i )
    {
        s_item[ i ].prev           = s_work.slot[ s_item[ i ].slot ];
        s_work.slot[ s_item[ i ].slot ] = s_item[ i ].cur;
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
    [ GUI_ROLE_PANEL       ] = "Panel",
    [ GUI_ROLE_PANEL_CHILD ] = "Panel (child)",
    [ GUI_ROLE_TITLE  ] = "Title",
    [ GUI_ROLE_BG     ] = "Control",
    [ GUI_ROLE_BORDER ] = "Border",
    [ GUI_ROLE_TEXT_PRIMARY   ] = "Text (primary)",
    [ GUI_ROLE_TEXT_SECONDARY ] = "Text (secondary)",
    [ GUI_ROLE_ACCENT ] = "Accent",
    [ GUI_ROLE_MARK   ] = "Mark",
    [ GUI_ROLE_GRAB   ] = "Grab",
};

static const char* const k_ext_name[ GUI_EXT_RESERVED_COUNT ] =
{
    [ GUI_EXT_INFO  ] = "Info",
    [ GUI_EXT_OK    ] = "OK",
    [ GUI_EXT_WARN  ] = "Warn",
    [ GUI_EXT_ERROR ] = "Error",
    [ GUI_EXT_DROP  ] = "Drop",
    [ GUI_EXT_SHADOW] = "Shadow",
};

static const char* const k_phase_name[ GUI_PHASE_COUNT ] =
{
    [ GUI_PHASE_IDLE   ] = "Idle",
    [ GUI_PHASE_HOT    ] = "Hot",
    [ GUI_PHASE_ACTIVE ] = "Active",
    [ GUI_PHASE_INERT  ] = "Inert",
};

/* The var axis is described once, in gui_theme.c's k_var table (name + class together), because
   metrics_compute needs the class and is included above this file; the two palette axes are
   named there for the same reason of proximity to the themes that author them.  These are the
   read doors -- five axes, one accessor each, no table anywhere above this line. */

const char* gui_style_role_name ( gui_style_role_t r )  { return ( (u32)r < GUI_ROLE_COUNT  && k_role_name [ r ]       ) ? k_role_name [ r ]       : "?"; }
const char* gui_style_phase_name( gui_style_phase_t p ) { return ( (u32)p < GUI_PHASE_COUNT && k_phase_name[ p ]       ) ? k_phase_name[ p ]       : "?"; }
const char* gui_style_seed_name ( gui_style_seed_t s )  { return ( (u32)s < GUI_SEED_COUNT  && k_seed_name [ s ]       ) ? k_seed_name [ s ]       : "?"; }
const char* gui_style_ramp_name ( gui_style_ramp_t r )  { return ( (u32)r < GUI_RAMP_COUNT  && k_ramp_name [ r ]       ) ? k_ramp_name [ r ]       : "?"; }
const char* gui_style_var_name  ( gui_style_var_t v )   { return ( (u32)v < GUI_VAR_COUNT   && k_var       [ v ].name  ) ? k_var       [ v ].name  : "?"; }
const char* gui_style_class_name( gui_style_class_t c ) { return ( (u32)c < GUI_CLASS_COUNT && k_class_name[ c ]       ) ? k_class_name[ c ]       : "?"; }

/* Only the reserved slots are named -- a kit's own registered colour has no engine-owned name,
   exactly as a kit's own brush has no entry in any name table either. */
const char* gui_style_ext_name( gui_style_ext_t e ) { return ( (u32)e < GUI_EXT_RESERVED_COUNT && k_ext_name[ e ] ) ? k_ext_name[ e ] : "?"; }

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

    /* Drop the set's brushes before its source runs.  A source re-registers its art on every
       landing (that is what a landing IS -- re-declare the look), so without this the pool would
       grow by the source's whole art set per theme / font / scale change and exhaust itself after
       a handful.  Safe because the handles it hands back are re-issued in the same order. */
    s_brush_n[ set ] = 0;

    /* Same reset, for the extended palette's kit-registered slots: the reserved four came across
       intact in the *style_active() copy above (baked from palette.ext), and everything past them
       is a registration this landing has not made yet -- cleared so a set never inherits a
       previous landing's colours at an index its current source does not claim. */
    s_ext_n[ set ] = GUI_EXT_RESERVED_COUNT;
    for ( u32 i = GUI_EXT_RESERVED_COUNT; i < GUI_STYLE_EXT_MAX; ++i )
        s_store[ set ].ext[ i ] = 0;

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
    for ( u32 i = 0; i < STYLE_SLOT_COUNT; ++i ) s_work.slot[ i ] = src[ i ];
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

    s_col_stack.sp = s_var_stack.sp = s_face_stack.sp = s_ext_stack.sp = 0;
    s_seed_sp = 0;
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

/* Register a brush in a set's pool and hand back the handle a face cell names.  Called from a
   style SOURCE (so the set being filled is the one that gets it) or ad hoc against the current
   set, mirroring gui_style_edit's rule exactly.

   Registration is IDEMPOTENT PER LANDING, not cumulative: the pool is reset when a set is
   installed, so a source that registers its art every landing -- which is the only sane way to
   write one, since a landing is where a source re-declares its whole look -- does not leak a new
   handle per theme change.  That reset is also why handles are stable to hold across a frame but
   not across a landing: re-read them from the source that made them. */
gui_style_face_t
gui_style_brush_add( const gui_brush_t* b )
{
    if ( !b ) return GUI_FACE_NONE;

    u16 set = ( s_installing >= 0 ) ? (u16)s_installing : s_set_cur;

    bool have_room = s_brush_n[ set ] < GUI_STYLE_BRUSH_MAX;
    ORB_ASSERT( have_room && "style brush pool exhausted -- raise GUI_STYLE_BRUSH_MAX" );
    if ( !have_room )
        return GUI_FACE_NONE;      /* a flat colour beats a corrupt handle */

    s_brush[ set ][ s_brush_n[ set ] ] = *b;
    return (gui_style_face_t)( ++s_brush_n[ set ] );   /* 1-based; 0 stays GUI_FACE_NONE */
}

/* Claim the next free extended-palette slot in a set's pool and seed it with a default -- the
   ext-plane sibling of gui_style_brush_add, same idempotent-per-landing contract (called from a
   style SOURCE, reset to GUI_EXT_RESERVED_COUNT at the top of every landing).  Unlike a brush
   handle the slot value itself lives in the flat slot space (gui_style_t.ext), so registering one
   is a plain store rather than a pool write: push / pop / a set switch already know how to move
   it, with nothing new to keep in step. */
gui_style_ext_t
gui_style_ext_add( u32 default_abgr )
{
    u16 set = ( s_installing >= 0 ) ? (u16)s_installing : s_set_cur;

    bool have_room = s_ext_n[ set ] < GUI_STYLE_EXT_MAX;
    ORB_ASSERT( have_room && "style ext pool exhausted -- raise GUI_STYLE_EXT_MAX" );
    if ( !have_room )
        return GUI_EXT_INFO;      /* a reserved slot's colour beats a corrupt handle */

    gui_style_ext_t id = (gui_style_ext_t)s_ext_n[ set ]++;
    s_store[ set ].ext[ id ] = default_abgr;
    return id;
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

/*==============================================================================================
    The two state predicates every projection below is written in.  Both were open-coded at six
    sites between them, so the rule "the nav cursor lights a widget exactly like the mouse does"
    was authored six times; it is authored here instead.
==============================================================================================*/

/* "Reads as hovered."  The keyboard nav cursor lights a widget exactly like the mouse, and the
   two are mutually exclusive (gui_nav.c drops nav.highlight on any mouse move), so at most one
   is ever set.  Kept separate from style_phase because the MIX needs this weight even while the
   item is ALSO active, where the phase distillation below reports ACTIVE instead. */
bool
style_is_hot( gui_item_state_t st )
{
    return st.hover || st.nav;
}

/* Interact state -> style PHASE: the three-way rule every projection here and every render above
   picks a face with.  This is the whole body of the PUBLIC gui_item_phase (stock/), which now
   delegates down to it -- authored here because style is the lowest unit that needs the rule, and
   a rule with two homes is a rule that drifts.  (GUI_PHASE_INERT is never derived from live
   interaction -- a render selects it deliberately, and it never means "disabled".) */
u8
style_phase( gui_item_state_t st )
{
    return st.active            ? GUI_PHASE_ACTIVE
         : style_is_hot( st )   ? GUI_PHASE_HOT
                                : GUI_PHASE_IDLE;
}

/* Frame-background tint for a "framed field" widget (checkbox box, slider track, drag box, input):
   hover / nav / active lift it to the shared hot / active cells -- one at a time, since hover and
   nav-highlight are mutually exclusive -- over a caller-supplied idle base so each field keeps its
   own resting colour, matching how Dear ImGui's FrameBgHovered lifts every framed control, not
   just buttons. */

u32
col_frame_bg( gui_item_state_t st, u32 idle_color )
{
    switch ( style_phase( st ) )
    {
        case GUI_PHASE_ACTIVE: return COL_BG_ACTIVE;
        case GUI_PHASE_HOT:    return COL_BG_HOT;
        default:               return idle_color;   /* the field keeps its own resting colour */
    }
}

/* Common case background color for a pushbutton style widget.
   col_frame_bg with the plain widget background as the idle base. */
u32 col_item_bg( gui_item_state_t st )
{
    return col_frame_bg( st, COL_BG_IDLE );
}

/* The control face of an item that can be CHOSEN -- col_item_bg washed for selection.

   This is the projection the library was missing, and its absence is what made every list widget
   write `selected ? COL_BG_ACTIVE : COL_BG_HOT`: naming "selected" that way cost you the ability
   to say "hovered", so a selected row went inert the moment it became selected.  Here the phase
   is distilled from the live state exactly as it always was, and `selected` washes whichever
   phase cell that resolves to -- so a chosen row lifts on hover and sinks on press like every
   other surface in the system, the wash riding along on top.

   The phase comes from style_phase above -- the same rule the public gui_item_phase hands a user
   widget, so a stock render and its sibling can never pick different faces. */
u32
col_item_bg_selected( gui_item_state_t st, bool selected )
{
    u8 phase = style_phase( st );
    return selected ? style_col_selected( GUI_ROLE_BG, phase ) : style_col( GUI_ROLE_BG, phase );
}

/* The ink for a glyph drawn on a BARE ICON BUTTON -- one that paints no face at rest and fills
   only once hot or pressed: caption buttons (close X, maximize box, minimize bar), the dock
   maximize pin, a tab's close cross.

   Keyed on hover OR active, which is the SAME predicate those callers fill on -- and that is the
   whole point.  The two used to disagree: every one of them filled on ( hover || active ) but
   inked on ( hover ) alone.  Press one and slide off it without releasing -- the item keeps
   capture, so active stays true while hover goes false -- and the DIM ink ends up on the ACTIVE
   fill.  Those two cells are 63 luma apart in the dark theme and 12 in the light one, which is
   not a button with a symbol on it, it is a plain grey square.

   Fixed here rather than by nudging the palette, because the palette was never wrong: DIM ink is
   meant to sit on the title band, where it has 84-115 luma of separation, and it does.  It simply
   must not be the ink chosen at the moment a pressed fill is underneath it.  One predicate, read
   by both halves, is what makes that unrepresentable rather than merely fixed. */
u32
col_btn_glyph( gui_item_state_t st )
{
    return ( st.hover || st.active ) ? COL_TEXT_PRIMARY_IDLE : COL_TEXT_SECONDARY_IDLE;
}

/* The border of a focusable FIELD -- input box, numeric field, drag box, slider track.  A
   field's ground does not travel the phase axis (input_text_begin carries the reasoning), so the
   border alone says "the caret is here", on BORDER[ACTIVE].  Seven widgets spelled this ternary
   by hand before it had a name; one projection is what keeps an eighth from drifting. */
u32
col_field_border( gui_item_state_t st )
{
    return st.focused ? COL_BORDER_ACTIVE : COL_BORDER_IDLE;
}

/* The border of a bare TRACK -- a surface that paints its own ground (a gradient ramp, a checker,
   a swatch) and so cannot show hover in its fill the way a framed field does.  Its edge is the
   only thing left to light, which is why this is a border projection and not a BG one.  The
   sibling of col_field_border on the hot axis rather than the focus axis; the colour picker's
   four ramps spelled this ternary by hand before it had a name. */
u32
col_track_border( gui_item_state_t st )
{
    return style_is_hot( st ) ? COL_BORDER_HOT : COL_BORDER_IDLE;
}

/* The tab chip -- the TITLE band speaking ( state, current ).  TITLE[ACTIVE] is baked as the
   BODY colour ("a live tab IS its panel"), so the CURRENT chip reads it to merge into the
   content below -- and a PRESSED chip reads the same cell, previewing the join a release
   commits.  The current chip deliberately shows no hover: clicking it again does nothing, so
   lighting it would promise a click it cannot deliver.  This projection is why the tab bar and
   the dock strip cannot drift apart: both spend it, neither spells the mapping. */
u32
col_tab_bg( gui_item_state_t st, bool current )
{
    return style_col( GUI_ROLE_TITLE, current ? GUI_PHASE_ACTIVE : style_phase( st ) );
}

/* The chip's ink, keyed on the SAME predicate as its fill -- the col_btn_glyph rule: splitting
   the two is what puts DIM ink on a live face. */
u32
col_tab_ink( gui_item_state_t st, bool current )
{
    return ( current || style_phase( st ) != GUI_PHASE_IDLE ) ? COL_TEXT_PRIMARY_IDLE : COL_TEXT_SECONDARY_IDLE;
}

/* The movable part of a track control -- slider knob, scrollbar thumb -- off the GRAB row.
   Its own role rather than col_item_bg because a knob has TWO lifting neighbours: the track under
   it rides BG and the value fill beside it rides ACCENT, so a knob on either row matches one of
   them exactly in some phase (on BG it vanishes into the hovered track; on ACCENT into the fill).
   GRAB is authored per theme as the contrast anchor, opposite in polarity to the theme, which is
   what keeps the knob readable against both at once.  There is no INERT step here: a phase is
   selected from live interaction, and INERT is the deliberate inert face a render picks itself. */
u32 col_grab( gui_item_state_t st )
{
    return style_col( GUI_ROLE_GRAB, style_phase( st ) );
}

/*==============================================================================================
    The MIX -- the continuous coordinate over the same grid.

    Everything above resolves a cell: it names one phase and reads the colour there.  That is why
    a widget SNAPS.  Nothing in the projections is wrong, and adding an "animated" twin of each
    would only spread the same snap over twice the surface -- the enumeration itself is the
    ceiling, because you cannot name a cell halfway between IDLE and HOT.

    So the mix breaks the read in two.  style_mix distils live interaction into three continuous
    weights and is the ONE thing here that touches storage; style_col_mix spends those weights on
    a row of the grid and is as pure as every projection above it.  The split is what makes motion
    affordable to apply EVERYWHERE rather than at the four sites that could justify a damper:

      - ONE probe per item, not per painted thing.  A widget reads the mix once and spends it on
        its surface, its border and its ink, so a three-part widget costs one 16-byte slot -- and
        the three arrive together, because they share weights instead of each damping separately.
      - Storage stays proportional to items IN MOTION.  The weights rest at zero, so a settled
        widget's slot goes cold and evicts (gui_anim4's contract); an idle UI holds nothing.
      - id == GUI_ID_NONE opts out completely: the weights come back hard 0/1 with no probe at
        all, which is what a non-interactive caller and a replaying volatile block both want.
      - The rates are style vars, so a theme owns the feel of the whole widget set, and setting
        them to 0 makes the library snap -- the same code path, no animation branch anywhere.
==============================================================================================*/

#define ANIM_TAG_MIX  0xA501u   /* id_combine salt: keeps this slot distinct from all other per-widget state */

/* Distil (interaction, selection) into the continuous grid coordinate, damped.

   The three channels ride ONE gui_anim4 slot at three theme rates.  Note the rest vector: hot and
   act rest at 0 so they ramp UP from the palette base on first sight, but sel rests at whatever it
   currently IS -- an item that is already selected the first time it is drawn must not slide into
   selection, while one that BECOMES selected must.  gui_anim4 seeds a history-less channel from
   rest, so naming rest per channel is the whole of that distinction. */
gui_style_mix_t
style_mix( gui_id_t id, gui_item_state_t st, bool selected )
{
    gui_style_mix_t want = { style_is_hot( st ) ? 1.0f : 0.0f,
                             st.active          ? 1.0f : 0.0f,
                             selected           ? 1.0f : 0.0f };

    if ( id == GUI_ID_NONE )
        return want;   /* no identity -> no storage -> no motion, by the caller's choice */

    gui_anim4_t rest   = { 0.0f, 0.0f, want.sel, 0.0f };
    gui_anim4_t target = { want.hot, want.act, want.sel, 0.0f };
    gui_anim4_t speed  = { style_var( GUI_VAR_ANIM_HOT ),
                           style_var( GUI_VAR_ANIM_ACTIVE ),
                           style_var( GUI_VAR_ANIM_SELECT ), 0.0f };

    gui_anim4_t a = gui_anim4( id_combine( id, ANIM_TAG_MIX ), rest, target, speed );
    return ( gui_style_mix_t ){ a.x, a.y, a.z };
}

/* IDLE -> HOT -> ACTIVE composited by the two phase weights, in that order: a press reads on top
   of a hover because you cannot press what you are not over. */
static u32
mix_phase( u32 idle, u32 hot, u32 act, gui_style_mix_t m )
{
    return col_lerp( col_lerp( idle, hot, m.hot ), act, m.act );
}

/* Spend a mix on one role: the phase composite, then washed toward the accent by sel.  Pure --
   no storage, no interact server, callable with a hand-built mix.

   style_wash_selected already early-outs at travel <= 0, so an unselected item reads EXACTLY
   what style_col( role, phase ) would give it -- no second cell, no lerp toward one. */
u32
style_col_mix( u8 role, gui_style_mix_t m )
{
    u32 n = mix_phase( style_col( role, GUI_PHASE_IDLE   ),
                       style_col( role, GUI_PHASE_HOT    ),
                       style_col( role, GUI_PHASE_ACTIVE ), m );
    return style_wash_selected( n, m.sel );
}

/* col_frame_bg through a mix: the caller's resting colour lifting to the shared BG hot / active
   cells.  No selected wash -- a framed field is not a thing you select, it is a thing you type in. */
u32
col_frame_bg_mix( gui_style_mix_t m, u32 idle_color )
{
    return mix_phase( idle_color, COL_BG_HOT, COL_BG_ACTIVE, m );
}

/* The two one-call shorthands, for the widgets that paint a single surface and want the whole
   read in one line.  Both are style_mix + style_col_mix with the role filled in. */
u32 col_item_bg_mix( gui_id_t id, gui_item_state_t st, bool selected )
{
    return style_col_mix( GUI_ROLE_BG, style_mix( id, st, selected ) );
}

u32 col_grab_mix( gui_id_t id, gui_item_state_t st )
{
    return style_col_mix( GUI_ROLE_GRAB, style_mix( id, st, false ) );
}

/* True while NO ambient style scope is open -- the volatile-replay precondition check
   (chrome/widgets/gui_volatile.c) reads it through this predicate so the stack types stay
   private.  An idle-frame replay re-runs the callback outside whatever scope surrounded the
   original emit, so any of these would silently re-colour the block: a colour or var push, a
   seed push (which re-derives the whole grid, so it is the loudest of them), a live next-item
   override, or a style set the replay will not be inside. */
bool
style_stacks_empty( void )
{
    return s_col_stack.sp == 0 && s_var_stack.sp == 0 && s_face_stack.sp == 0 && s_seed_sp == 0
        && s_item_n == 0 && s_set_sp == 0;
}

// clang-format on
/*============================================================================================*/
