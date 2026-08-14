/*==============================================================================================

    gui/style/gui_stacks.c -- push/pop bracketing verbs: id scope, item flags, style overrides.

    Every function here is a thin wrapper with no logic of its own -- it just forwards to a stack
    that lives elsewhere:

        push_id / pop_id             id-scope levels for repeated widgets
        push_item_flag / next_       per-item behavior tweaks
        disabled_begin / _end        the named scope over GUI_ITEM_DISABLED

        push_style_color / _var      per-item theme overrides
        style_color                  the resolved read back out of the palette
        scale_push / _pop            a named density step, as three paired var pushes

    Two different stacks sit behind these verbs: id and item-flag pushes forward down to the
    interact server (core); style color / var / scale / seed pushes forward to statics kept in
    this unit's own gui_style_core.c.  Internal callers (e.g. combo's list rows using push_id) go
    through these same public functions -- there is no private shortcut.

    Included by gui_style.c LAST, after both machinery files, so every stack these wrappers call
    into already exists.  gui_api.c wires these functions into the public vtable.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    push_id / pop_id -- add a temporary id-scope level for repeated widgets within one region.

    Widget ids are already region-seeded, this is needed to separate widgets that share
    a label in the same region (e.g. list rows keyed by index).  
    
    push_id combines its key onto the current scope; pop_id removes one level.  
    
    Always balance them -- a region pop restores the scope depth anyway, so a stray push
    cannot escape its region, but balancing keeps ids stable.
==============================================================================================*/

void gui_push_id     ( const char* str ) { id_push( id_combine( id_seed(), id_hash( str ))); }
void gui_push_id_int ( i32 i )           { id_push( id_combine( id_seed(), (u32)i )); }
void gui_pop_id      ( void )            { id_pop(); }

/*==============================================================================================
    push_item_flag / pop_item_flag / next_item_flag -- the push-model per-item behavior set.

    - push/pop affect every widget until popped.
    - next_item_flag is a one-shot override the very next widget consumes, no pop needed.  
    - The merged value is resolved once per widget at emit time and read by item_state / the widget
    - so a new flag never touches a call site or the vtable layout consumers see.  

        gui()->push_item_flag( GUI_ITEM_DISABLED, true );
        gui()->button( "Off A" );  gui()->button( "Off B" );   // both disabled
        gui()->pop_item_flag();

        gui()->next_item_flag( GUI_ITEM_DISABLED, true );
        gui()->button( "Only this one is disabled" );
==============================================================================================*/

void gui_push_item_flag( gui_item_flags_t flag, bool enable ) { item_flag_push( flag, enable ); }
void gui_pop_item_flag ( void )                               { item_flag_pop(); }
void gui_next_item_flag( gui_item_flags_t flag, bool enable ) { item_flag_next( flag, enable ); }

/*==============================================================================================
    disabled_begin / disabled_end -- the named-scope shorthand for GUI_ITEM_DISABLED (the ImGui
    BeginDisabled / EndDisabled).  disabled_begin( true ) brackets a run of widgets so they all draw
    dimmed and reject input; disabled_begin( false ) pushes a no-op scope (so a conditional disable
    still balances with one disabled_end).  Nests cleanly via the item-flag stack.

        gui()->disabled_begin( !has_selection );
        gui()->button( "Delete" );
        gui()->disabled_end();
==============================================================================================*/

void
gui_disabled_begin( bool disabled )
{
    /* disabled state can only be added not removed within a disabled block 
       this or ensures that is true */

    bool now = (( s_build.item_flags & GUI_ITEM_DISABLED ) != 0 ) || disabled;
    item_flag_push( GUI_ITEM_DISABLED, now );
}

void gui_disabled_end( void ) { item_flag_pop(); }

/*==============================================================================================
    push_style_color / push_style_var (+ pop / next) -- the push-model theme override.

    push overrides a cell for every widget until the matching pop; pop takes a count, so two pushes
    are undone with one pop_style_*( 2 ), mirroring ImGui.  next_style_* overrides for just the
    next widget, no pop needed.  Colors are abgr (GUI_COLOR); vars are f32 pixels.

    A color names a (role, phase) cell of the color grid.  GUI_PHASE_ALL selects the whole phase
    row as ONE push, which is what recolouring "the text" or "the border" nearly always means --
    and it stays one entry, so it takes one pop:

        gui()->push_style_color( GUI_ROLE_BG, GUI_PHASE_IDLE, GUI_COLOR( 0xFF,0,0,0xFF ));
        gui()->push_style_color( GUI_ROLE_BG, GUI_PHASE_HOT,  GUI_COLOR( 0xFF,0x40,0x40,0xFF ));
        gui()->button( "Red Button" );
        gui()->pop_style_color( 2 );

        gui()->push_style_color( GUI_ROLE_TEXT, GUI_PHASE_ALL, GUI_COLOR( 0,0xFF,0,0xFF ));
        gui()->button( "Green label, hovered or not" );
        gui()->pop_style_color( 1 );

        gui()->push_style_var( GUI_VAR_PAD, 20.0f );
        gui()->button( "Roomy" );
        gui()->pop_style_var( 1 );

    There is no look-qualified pair: a selected read washes whatever cell already resolved
    (style_wash_selected / style_col_selected, gui.h), so overriding "how selection looks here"
    means pushing GUI_SEED_ACCENT or GUI_RAMP_SELECT for a scope, not naming a second plane --
    see push_style_seed below.
==============================================================================================*/

void gui_push_style_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr ) { style_push_color( role, phase, abgr ); }
void gui_pop_style_color ( u32 count )                                                { style_pop_color( count ); }
void gui_next_style_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr ) { style_next_color( role, phase, abgr ); }

/* The FACE verbs -- the same shapes over the parallel plane.  Their own stack (one per pop verb,
   the house rule) so an interleaved colour / face / var sequence unwinds correctly. */

void gui_push_style_face( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face ) { style_push_face( role, phase, face ); }
void gui_pop_style_face ( u32 count )                                                             { style_pop_face( count ); }
void gui_next_style_face( gui_style_role_t role, gui_style_phase_t phase, gui_style_face_t face ) { style_next_face( role, phase, face ); }

void gui_push_style_var( gui_style_var_t var, f32 value )   { style_push_var( var, value ); }
void gui_pop_style_var ( u32 count )                        { style_pop_var( count ); }
void gui_next_style_var( gui_style_var_t var, f32 value )   { style_next_var( var, value ); }

/*==============================================================================================
    push_style_seed / pop_style_seed -- bulk recolour: replace a source colour and re-derive
    every role that comes from it.

    push_style_color only ever writes one cell -- and GUI_PHASE_ALL, its widest reach, writes one
    flat value into all four phase cells of a role, which erases hover/focus reactions (a button
    recoloured that way stops changing on hover).  push_style_seed instead replaces the SOURCE
    colour and re-runs the bake, so the four phase cells stay a ramp step apart and every role
    derived from that seed moves together:

        gui()->push_style_seed( GUI_SEED_ACCENT, GUI_COLOR( 0xC8, 0x96, 0x3C, 0xFF ) );
        ... this whole panel is gold: fills, hover washes, focus rings, nav highlight ...
        gui()->pop_style_seed( 1 );

    One push is one entry, so one pop undoes it, exactly like the other two stacks.  Nesting is
    shallow by design (8 deep): this is a panel-sized scope, not a per-widget one -- for a single
    widget, next_style_color is still the cheaper answer.

    A seed push never reaches the INSTALLED style -- it is ambient scope like every other push,
    cleared at the frame boundary.  To change a look permanently, write the palette through
    style_edit() and call style_bake().
==============================================================================================*/

void gui_push_style_seed( gui_style_seed_t seed, u32 abgr ) { style_push_seed( seed, abgr ); }
void gui_pop_style_seed ( u32 count )                       { style_pop_seed( count ); }

/*==============================================================================================
    scale_push / scale_pop -- scope a named density step (the theme's scale ramp, gui_scale_t)
    over the widgets until the matching pop: one declaration instead of per-row pixel sizes.

    A step is a paired push of the three metric slots (LINE_SIZE, WIDGET_PAD, WIDGET_GAP) with
    that step's theme values, so every metric read and counting helper inside the scope --
    WIDGET_H, sz_rows_h( n ), sz_fit_row -- speaks the step with no widget changes.

    Region PADDING (the inset between the region box and its content) is still captured when the
    region opens, so a push meant to affect a child's pad must precede child_begin/window_begin.
    Row HEIGHT and the GAP between rows resolve live, per row (mod_gap_x/_y in gui_layout_core.c,
    mirroring row_h) -- so a push placed anywhere before the first row of a body still lands, even
    after the region itself was opened by callee code the caller doesn't control (e.g. inside a
    combo dropdown, whose body region is opened by combo_begin, not the call site):

        gui()->scale_push( GUI_SCALE_DENSE );        // this panel is a dense list
        gui()->child_begin( "entities", 0, 0, 0 );
        ... selectable rows at the dense height ...
        gui()->child_end();
        gui()->scale_pop();

        if ( gui()->combo_begin( "##pick", preview, GUI_COMBO_NONE ) )   // box stays std scale
        {
            gui()->scale_push( GUI_SCALE_DENSE );     // only the dropdown body goes dense
            ... selectable rows ...
            gui()->scale_pop();
            gui()->combo_end();
        }
==============================================================================================*/

/* The ramp steps read through style_scale, not s_style, so a step means whatever the CURRENT
   style set says it means -- a kit's DENSE is the kit's own, exactly like its colors. */
#define SCALE_ROW 0u
#define SCALE_PAD 1u
#define SCALE_GAP 2u

void
gui_scale_push( gui_scale_t s )
{
    /* clamp, like the other stacks */
    if ( s >= GUI_SCALE_COUNT ) s = GUI_SCALE_STD;   
    style_push_var( GUI_VAR_ROW, style_scale( s, SCALE_ROW ) );
    style_push_var( GUI_VAR_PAD, style_scale( s, SCALE_PAD ) );
    style_push_var( GUI_VAR_GAP, style_scale( s, SCALE_GAP ) );
}

void
gui_scale_pop( void )
{
    style_pop_var( 3 );   /* the three slots scale_push pushed */
}

/* The row height of a ramp step, without pushing it -- size a child to another step's rows
   (a BAR header band above a DENSE list), or feed custom chrome. */
f32
gui_sz_scale_row( gui_scale_t s )
{
    return style_scale( s, SCALE_ROW );
}

/* The seven GUI_VAR_*_SHAPE picks have no setter verb of their own: a theme AUTHORS the value
   and push_style_var / next_style_var scope it, exactly like every other style var.  The
   alternate renders they select still live in stock/gui_symbol_style.c. */

// clang-format on
/*============================================================================================*/
