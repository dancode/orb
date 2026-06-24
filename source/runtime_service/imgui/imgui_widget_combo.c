/*==============================================================================================

    runtime_service/imgui/imgui_widget_combo.c -- Combo box + list box.

    Two selection widgets built entirely out of the existing toolkit rather than new machinery:

      Combo   -- a framed preview box (selected text + a down arrow on the right edge) with a
                 trailing label.  Clicking it drops a popup of selectable rows below the box.  The
                 dropdown is the popup layer (imgui_popup.c) keyed off the combo's widget id and
                 anchored at the box, so occlusion, click-outside close, overlay detach, and z-band
                 all come from there unchanged; only the box-anchored placement + a min width that
                 matches the box are combo-specific.  A row clicked in the body dismisses the combo
                 (selectable flags s_build.combo_item_clicked; combo_end closes on it).

      List box -- a framed, independently scrolling box of selectable rows with a trailing label.
                 It is a child_begin (which already frames, clips, and scrolls) plus the label drawn
                 to its right -- no new region type.

    Each comes in a generic Begin/End form (full control over the rows) and a one-liner over an
    array of strings, mirroring Dear ImGui's BeginCombo/Combo and BeginListBox/ListBox split.

    Included by imgui.c after imgui_popup.c, so the popup internals (popup_open_id, popup_is_open_id,
    popup_set_anchor, popup_begin_common_id, IMGUI_POPUP_*) and every widget / layout helper above
    are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Combo box
----------------------------------------------------------------------------------------------*/

/* Persistent per-combo state: the last frame the dropdown body emitted.  Used to tell an "open"
   click (toggle closed) from a "closed" click (open), since popup_close_check has already dropped
   the popup by the time combo_begin runs -- without it the same click would close then reopen. */
typedef struct { u32 open_frame; } imgui_combo_state_t;

/* The height a list of `n` selectable rows occupies in a stack body: each row is WIDGET_H with a
   WIDGET_GAP between rows, plus the region's top gap and the bottom border.  Shared by the combo
   dropdown height cap and the list box default. */
static f32
combo_rows_h( i32 n )
{
    if ( n < 1 ) n = 1;
    return (f32)n * WIDGET_H + ( (f32)n + 1.0f ) * WIDGET_GAP + WIN_BORDER;
}

/* Mandatory combo dropdown window behavior: fixed, uncollapsible, no title bar -- a popup but with
   the vertical scrollbar left dynamic (so a height-capped dropdown scrolls past the cap). */
#define IMGUI_COMBO_POPUP_FLAGS \
    ( IMGUI_WIN_NOTITLEBAR | IMGUI_WIN_NOMOVE | IMGUI_WIN_NORESIZE | IMGUI_WIN_NOCOLLAPSE )

/* Max visible rows for the dropdown from the height flag (0 = no cap -- "largest", auto-size).  An
   unset height behaves as REGULAR, matching Dear ImGui's default popup height. */
static i32
combo_cap_items( imgui_combo_flags_t flags )
{
    if ( flags & IMGUI_COMBO_HEIGHT_SMALL   ) return 4;
    if ( flags & IMGUI_COMBO_HEIGHT_REGULAR ) return 8;
    if ( flags & IMGUI_COMBO_HEIGHT_LARGE   ) return 20;
    if ( flags & IMGUI_COMBO_HEIGHT_LARGEST ) return 0;   /* no cap: as many as fit */
    return 8;                                             /* COMBO_NONE / unset: same as REGULAR */
}

bool
imgui_combo_begin( const char* label, const char* preview_value, imgui_combo_flags_t flags )
{
    imgui_id_t   id  = widget_id( label );
    imgui_rect_t row = widget_next_rect( WIDGET_H );

    /* The box takes the control track and the label trails it (or sits in its field-split track),
       exactly like the other labeled value widgets.  The min control width keeps room for the
       arrow plus a little preview text when the cell is squeezed. */
    imgui_rect_t box = widget_split_label( row, label, WIDGET_H + font_char_h() * 2.0f, COL_TEXT_DIM );

    widget_state_t st = widget_behavior( id, box, WIDGET_KIND_BUTTON );

    /* The dropdown is a popup keyed off the combo's widget id, so it inherits the id scope + the
       "###" grammar and never collides with a same-titled combo elsewhere. */
    imgui_id_t           pid = id_combine( id, IMGUI_POPUP_SALT );
    imgui_combo_state_t* cs  = IMGUI_STATE( imgui_combo_state_t, id );

    /* Toggle on click.  A click while open is the dismiss gesture: popup_close_check (frame top)
       has already closed the dropdown as a click outside it, so only open when the body did NOT
       emit last frame -- otherwise the one click would close then immediately reopen it. */
    bool was_open = ( cs->open_frame + 1u == s_retained.frame );
    if ( st.clicked && !was_open )
        popup_open_id( pid, box.x, box.y + box.h );

    /* Keep the dropdown pinned under the box every frame it is open (so it follows a dragged
       parent window).  popup_clamp nudges it back on-screen, flipping it above the box near the
       bottom edge -- the placement reuse that makes a dedicated combo window unnecessary. */
    if ( popup_is_open_id( pid ) )
        popup_set_anchor( pid, box.x, box.y + box.h );

    /* Box frame: a button-tinted field, a down arrow boxed at the right edge, and the preview text
       fitted into the room before the arrow. */
    draw_push_rect_filled ( box.x, box.y, box.w, box.h, 0,0,1,1, 0, widget_bg_color( st ) );
    draw_push_rect_outline( box.x, box.y, box.w, box.h, WIN_BORDER, 0, COL_BORDER );

    imgui_rect_t arrow = { box.x + box.w - box.h, box.y, box.h, box.h };
    draw_arrow( arrow, IMGUI_DIR_DOWN, COL_TEXT );

    if ( preview_value && preview_value[ 0 ] )
    {
        f32 avail = arrow.x - ( box.x + WIDGET_PAD ) - WIDGET_PAD;
        draw_text_fit_n( box.x + WIDGET_PAD, text_center_y( box.y, box.h ),
                         COL_TEXT, preview_value, 0xFFFFFFFFu, avail );
    }

    /* Open the dropdown.  A HEIGHT_* cap makes it a fixed-width (box-wide), height-capped scrolling
       popup -- hug the rows up to the cap, then scroll; "largest" (cap 0) keeps the auto-size popup,
       hugging all rows.  Both reuse the popup path; only the size policy differs. */
    i32  cap = combo_cap_items( flags );
    bool vis;
    if ( cap > 0 )
    {
        vis = popup_begin_common_id( pid, NULL, IMGUI_COMBO_POPUP_FLAGS, false,
                                     box.w, combo_rows_h( cap ) );
    }
    else
    {
        vis = popup_begin_common_id( pid, NULL,
                                     IMGUI_COMBO_POPUP_FLAGS | IMGUI_WIN_ALWAYS_AUTOSIZE, false,
                                     0.0f, 0.0f );
    }

    if ( vis )
    {
        cs->open_frame   = s_retained.frame;   /* body emitted this frame -> "open" next frame */
        s_build.combo_open = true;              /* a row clicked here dismisses the combo */

        imgui_stack();                        /* the dropdown body is a vertical list */

        /* An auto-size ("largest") dropdown shrinks to its content; floor its measured width to the
           box so short items still line up under it.  A capped dropdown is already box-wide. */
        if ( cap <= 0 )
            widget_track_width( lf()->content_x + ( box.w - 2.0f * WIDGET_PAD ) );
    }
    return vis;
}

void
imgui_combo_end( void )
{
    /* A row clicked in the body dismisses the combo (selectable set the flag while combo_open). */
    if ( s_build.combo_item_clicked )
        imgui_popup_close_current();

    s_build.combo_open         = false;
    s_build.combo_item_clicked = false;

    imgui_popup_end();
}

/* One-liner over an array of strings: the common "pick one of N" case.  *current_item is the
   selected index (out of range shows an empty preview and selects nothing); returns true on the
   frame the selection changes.  push_id_int keeps the row ids distinct when labels repeat. */
bool
imgui_combo( const char* label, i32* current_item, const char* const items[], i32 count )
{
    const char* preview = ( *current_item >= 0 && *current_item < count ) ? items[ *current_item ] : "";
    bool        changed = false;

    if ( imgui_combo_begin( label, preview, IMGUI_COMBO_NONE ) )
    {
        for ( i32 i = 0; i < count; ++i )
        {
            imgui_push_id_int( i );
            bool sel = ( *current_item == i );
            if ( imgui_selectable( items[ i ], &sel ) )
            {
                *current_item = i;
                changed       = true;
            }
            imgui_pop_id();
        }
        imgui_combo_end();
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
    List box

    A framed, independently scrolling box of selectable rows with a trailing label.  listbox_begin
    is a child_begin sized to the box (defaulting to ~7 rows tall and filling the line minus the
    label); the label is captured and drawn to the box's right at listbox_end.  A small stack
    carries the label + box rect across the pair, the same way the tooltip save slot does, so the
    boxes may nest.
----------------------------------------------------------------------------------------------*/

#define LISTBOX_DEPTH 4   /* nested list boxes (rare, but child_begin nests, so allow a few) */

static struct { const char* label; imgui_rect_t box; } s_listbox_stack[ LISTBOX_DEPTH ];
static u32 s_listbox_sp;

bool
imgui_listbox_begin( const char* label, f32 w, f32 h )
{
    /* Defaults: ~7 rows tall, and wide enough to fill the line after reserving the trailing label. */
    if ( h <= 0.0f )
        h = combo_rows_h( 7 );

    f32 lab_w = ( label_vis_len( label ) > 0 ) ? label_width( label ) + WIDGET_PAD : 0.0f;
    if ( w <= 0.0f )
    {
        w = imgui_content_avail().x - lab_w;
        if ( w < WIDGET_H * 4.0f ) w = WIDGET_H * 4.0f;
    }

    /* The framed scrolling box is a plain child; the label is the only thing layered on top. */
    imgui_child_begin( label, w, h, IMGUI_WIN_NONE );

    u32 slot = s_listbox_sp < LISTBOX_DEPTH ? s_listbox_sp : LISTBOX_DEPTH - 1;
    ++s_listbox_sp;
    s_listbox_stack[ slot ].label = label;
    s_listbox_stack[ slot ].box   = lf()->outer;   /* the child's box, for the trailing label */

    imgui_stack();   /* the list body is a vertical stack of selectable rows */
    return true;
}

void
imgui_listbox_end( void )
{
    imgui_child_end();

    if ( s_listbox_sp == 0 ) return;   /* unbalanced listbox_end -- ignore */
    --s_listbox_sp;
    u32          slot  = s_listbox_sp < LISTBOX_DEPTH ? s_listbox_sp : LISTBOX_DEPTH - 1;
    const char*  label = s_listbox_stack[ slot ].label;
    imgui_rect_t box   = s_listbox_stack[ slot ].box;

    /* Trailing label, drawn past the box's right edge and aligned to its first row (markers
       stripped by draw_label, like every label).  Drawn under the parent clip after child_end. */
    if ( label_vis_len( label ) > 0 )
        draw_label( box.x + box.w + WIDGET_PAD, text_center_y( box.y, WIDGET_H ), COL_TEXT, label );
}

/* One-liner over an array of strings: a scrolling list showing `height_in_items` rows (<= 0 picks
   min(count, 7)).  *current_item is the selected index; returns true when the selection changes. */
bool
imgui_listbox( const char* label, i32* current_item, const char* const items[], i32 count,
               i32 height_in_items )
{
    if ( height_in_items <= 0 )
        height_in_items = ( count < 7 ) ? count : 7;

    bool changed = false;
    if ( imgui_listbox_begin( label, 0.0f, combo_rows_h( height_in_items ) ) )
    {
        for ( i32 i = 0; i < count; ++i )
        {
            imgui_push_id_int( i );
            bool sel = ( *current_item == i );
            if ( imgui_selectable( items[ i ], &sel ) )
            {
                *current_item = i;
                changed       = true;
            }
            imgui_pop_id();
        }
        imgui_listbox_end();
    }
    return changed;
}

// clang-format on
/*============================================================================================*/
