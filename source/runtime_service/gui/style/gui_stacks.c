/*==============================================================================================

    runtime_service/gui/style/gui_stacks.c -- Bracketing vocabulary: id scope, item flags, style.

    The verbs a caller brackets widgets with -- thin wrappers, no machinery of their own:

        push_id / pop_id            id-scope levels for repeated widgets    -> core/gui_id.c
        push_item_flag / next_      per-item behavior tweaks                -> core/gui_ctx.c
        disabled_begin / _end       the named scope over GUI_ITEM_DISABLED
        push_style_color / _var     per-item theme overrides                -> gui_style_core.c
        style_color                 the resolved read back out of the palette
        scale_push / _pop           a named density step, as three paired var pushes

    Pure caller vocabulary (the machinery / vocabulary split: the stacks and their resolution live
    in the machinery files; the verbs a user speaks live here).  Nothing in the lib below depends
    on these wrappers; internal uses (combo's push_id for its list rows) are deliberate dogfooding
    through the gui_host.h declarations.

    The forwarding cuts two ways, which is why this file sits in the style unit: the id and
    item-flag brackets forward DOWN to interact-server seams (style -> core is the graph's own
    edge), while the style color / var / scale brackets reach this unit's own statics in
    gui_style_core.c -- which is what keeps those static.

    Included by gui_style.c LAST, above both machinery files.  gui_api.c (frame unit) wires these
    into the vtable through the gui_host.h declarations.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    push_id / pop_id -- add a temporary id-scope level for repeated widgets within one region.

    Widget ids are already region-seeded, so this is only needed to separate widgets that share a
    label in the same region (e.g. list rows keyed by index).  push_id combines its key onto the
    current scope; pop_id removes one level.  Always balance them -- a region pop restores the
    scope depth anyway, so a stray push cannot escape its region, but balancing keeps ids stable.
==============================================================================================*/

void gui_push_id    ( const char* str ) { id_push( id_combine( id_seed(), id_hash( str ) ) ); }
void gui_push_id_int( i32 i )           { id_push( id_combine( id_seed(), (u32)i ) ); }
void gui_pop_id     ( void )            { id_pop(); }

/*==============================================================================================
    push_item_flag / pop_item_flag / next_item_flag -- the push-model per-item behavior set.

    push/pop affect every widget until popped (and nest); next_item_flag is a one-shot override the
    very next widget consumes, no pop needed.  The merged value is resolved once per widget at emit
    time and read by item_state / the widget, so a new flag never touches a call site or the
    vtable layout consumers see.  See gui_item_flags_t in gui.h for the model and the flags.

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
        gui()->button( "Delete" );          // inert + dimmed while nothing is selected
        gui()->disabled_end();
==============================================================================================*/

void
gui_disabled_begin( bool disabled )
{
    /* OR the bit in -- never clear it -- so disabled_begin( false ) nested inside an outer
       disabled_begin( true ) keeps the widgets disabled (the ImGui nesting rule). */
    bool now = ( ( s_build.item_flags & GUI_ITEM_DISABLED ) != 0 ) || disabled;
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

        gui()->push_style_color( GUI_ROLE_BG, GUI_PHASE_IDLE, GUI_COLOR( 0xFF,0,0,0xFF ) );      // red
        gui()->push_style_color( GUI_ROLE_BG, GUI_PHASE_HOT,  GUI_COLOR( 0xFF,0x40,0x40,0xFF ) );
        gui()->button( "Red Button" );
        gui()->pop_style_color( 2 );                                             // both

        gui()->push_style_color( GUI_ROLE_TEXT, GUI_PHASE_ALL, GUI_COLOR( 0,0xFF,0,0xFF ) );
        gui()->button( "Green label, hovered or not" );
        gui()->pop_style_color( 1 );                                             // all four cells

        gui()->push_style_var( GUI_VAR_PAD, 20.0f );
        gui()->button( "Roomy" );
        gui()->pop_style_var( 1 );

    The plain verbs address the NORMAL plane, which is what an unqualified colour means
    everywhere in the system.  The _look pair adds the third coordinate and accepts GUI_LOOK_ALL
    to span both planes as ONE push -- so recolouring "the text, selected or not" is still a
    single balanced push_style_color_look, not two:

        gui()->push_style_color_look( GUI_ROLE_TEXT, GUI_PHASE_ALL, GUI_LOOK_ALL, ink );
        ... every text cell in the grid, both planes ...
        gui()->pop_style_color( 1 );                        // still ONE entry, ONE pop
==============================================================================================*/

void gui_push_style_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr ) { style_push_color( role, phase, GUI_LOOK_NORMAL, abgr ); }
void gui_pop_style_color ( u32 count )                                          { style_pop_color( count ); }
void gui_next_style_color( gui_style_role_t role, gui_style_phase_t phase, u32 abgr ) { style_next_color( role, phase, GUI_LOOK_NORMAL, abgr ); }

/* The look-qualified pair.  No pop of their own: a look push is a colour push, so it lands on
   the colour stack and pop_style_color takes it back -- one stack per pop verb, and adding a
   coordinate to a cell address does not make it a different kind of override. */
void gui_push_style_color_look( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look, u32 abgr ) { style_push_color( role, phase, look, abgr ); }
void gui_next_style_color_look( gui_style_role_t role, gui_style_phase_t phase, gui_style_look_t look, u32 abgr ) { style_next_color( role, phase, look, abgr ); }

void gui_push_style_var( gui_style_var_t var, f32 value )   { style_push_var( var, value ); }
void gui_pop_style_var ( u32 count )                          { style_pop_var( count ); }
void gui_next_style_var( gui_style_var_t var, f32 value )   { style_next_var( var, value ); }

/*==============================================================================================
    push_style_seed / pop_style_seed -- the BULK recolour: replace a source colour and re-derive.

    The verb the grid could not express.  push_style_color takes one cell; GUI_PHASE_ALL takes a
    row but writes one value into all four of its cells, which flattens the ramp -- push it on
    GUI_ROLE_BG and you get a button that no longer reacts to hover.  So the only bulk colour verb
    was the one you could not use on anything interactive.

    A seed push replaces the SOURCE and re-runs the bake, so the cells stay four colours a ramp
    step apart and every role that derives from that seed moves together:

        gui()->push_style_seed( GUI_SEED_ACCENT, GUI_COLOR( 0xC8, 0x96, 0x3C, 0xFF ) );
        ... this whole panel is gold: fills, hover washes, focus rings, nav highlight ...
        gui()->pop_style_seed( 1 );

    One push is one entry, so one pop undoes it, exactly like the other two stacks.  Nesting is
    shallow by design (8 deep): this is a panel-sized scope, not a per-widget one -- for a single
    widget, next_style_color is still the cheaper answer.

    Note what it does NOT do: it never reaches the INSTALLED style.  A seed push is ambient scope
    like every other push, cleared at the frame boundary.  To change a look permanently, write
    the palette through style_edit() and call style_bake().
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
    if ( (u32)s >= GUI_SCALE_COUNT ) s = GUI_SCALE_STD;   /* clamp, like the other stacks */
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

/* The seven GUI_VAR_*_SHAPE picks have no setter verb of their own: a theme AUTHORS one and
   push_style_var scopes it, which is the same pair every other style value gets.  Three global
   setters used to live here, from when chrome derived every look and there were no base widget
   builders to push around; the alternate renders they selected are untouched and still live in
   stock/gui_symbol_style.c. */

// clang-format on
/*============================================================================================*/
