/*==============================================================================================

    sandbox/gui/sb_gui_timeline/gui_timeline.c - profiler timeline overlay (flame graph).

    Consumes engine/prof through prof_host.h direct calls and draws through the gui() vtable
    only -- canvas + item + draw_* + clip, no gui internals -- so the file lifts out as-is.

    The rings only remember the recent past (drop-newest on overflow), so the timeline keeps
    its own history, folded from the drain once per frame:

      - completed spans per thread  -- BEGIN/END pairs folded by a stack walk into
                                       { t0, t1, id, depth }, circular, overwrite-oldest
      - the live open stack         -- zones begun but not yet ended, drawn out to "now"
      - frame marks                 -- one global ring of { tick, frame number }

    View model: a time window [ view_t1 - span_ns, view_t1 ]. LIVE pins view_t1 to the newest
    event every frame; interacting (drag to pan, wheel to zoom around the cursor, click a bar
    in the frame strip to focus that frame) drops out of live, the Live button resumes it.
    Pausing also STOPS CAPTURE: an investigation can last minutes, and collecting on would
    wrap the circular history right through the data under inspection (and be stale by resume
    anyway). Resume discards the ring backlog and continues from now, leaving an honest gap.

    One-consumer rule: prof allows a single drain consumer. gui_timeline_update() stands down
    (drains nothing) while a Chrome-trace dump is active or the hitch monitor is armed -- those
    own the drain; the window says so instead of showing silently stale data.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/prof/prof_host.h"
#include "runtime_service/gui/gui_host.h"
#include "sandbox/gui/sb_gui_timeline/gui_timeline.h"

// clang-format off

/*==============================================================================================
    Limits + storage
==============================================================================================*/

#define TL_SPAN_CAP      8192    /* completed spans kept per thread; power of two           */
#define TL_FRAME_CAP     512     /* frame marks kept; power of two                          */
#define TL_STACK_MAX     32      /* live nesting depth tracked while folding                */
#define TL_DEPTH_ROWS    12      /* deepest nesting row a track draws                       */
#define TL_DRAIN_CHUNK   512     /* events per prof_drain call                              */

#define TL_ROW_H         17.0f   /* one nesting row (bar + 1px gap)                         */
#define TL_BAR_H         16.0f
#define TL_TRACK_GAP     6.0f    /* space between thread tracks                             */

typedef struct tl_span_s
{
    i64 t0, t1;     // zone begin/end ticks (sys_tick_nanoseconds domain)
    u32 id;         // zone name hash
    u16 depth;      // nesting row, 0 = outermost
    u16 _pad;
} tl_span_t;

typedef struct tl_open_s
{
    i64 t0;         // begin tick of a zone still open
    u32 id;
    u32 _pad;
} tl_open_t;

typedef struct tl_track_s
{
    tl_span_t spans[ TL_SPAN_CAP ];     // circular history, overwrite-oldest
    u32       head;                     // free-running append cursor
    tl_open_t open[ TL_STACK_MAX ];     // live zone stack (folding state AND drawable)
    u32       depth;                    // live stack depth (counts past TL_STACK_MAX)
    u32       max_depth;                // deepest span row seen; sizes the track
} tl_track_t;

typedef struct tl_frame_mark_s
{
    i64 tick;
    u32 number;     // low 32 bits of the frame number
    u32 _pad;
} tl_frame_mark_t;

typedef struct tl_hover_s
{
    bool valid;
    bool open;      // span still running (t1 is "now", not an end)
    u32  id;
    u32  thread;
    i64  t0, t1;
    u32  depth;
} tl_hover_t;

typedef struct tl_state_s
{
    /* capture */
    bool blocked;                       // another prof consumer owns the drain this frame
    i64  t_origin;                      // first tick seen; ruler zero
    i64  t_latest;                      // newest tick seen
    u64  drained;                       // lifetime events folded
    u32  frame_head;                    // free-running cursor into the frame-mark ring

    /* view */
    bool live;                          // right edge follows t_latest; false = paused, capture stopped
    bool was_live;                      // last update's live state; a rising edge = resume
    u32  drop_base;                     // cumulative prof drops at last resume/clear (readout baseline)
    f64  span_ns;                       // window width
    f64  view_t1;                       // window right edge (absolute ns)
    f32  drag_x;                        // pan anchor while the canvas item is held

    tl_hover_t hover;                   // span under the cursor, found during draw
} tl_state_t;

static tl_state_t      g_tl = { .live = true, .was_live = true, .span_ns = 100.0 * 1000.0 * 1000.0 };
static tl_track_t      g_tl_tracks[ PROF_MAX_THREADS ];
static tl_frame_mark_t g_tl_frames[ TL_FRAME_CAP ];
static prof_event_t    g_tl_buf[ TL_DRAIN_CHUNK ];

// clang-format on

/*==============================================================================================
    Small helpers
==============================================================================================*/

/* Stable, well-separated color per zone name: golden-ratio hue from the hash, fixed s/v. */
static u32
tl_hsv_abgr( f32 h, f32 s, f32 v )
{
    f32 r, g, b;
    i32 i = ( i32 )( h * 6.0f );
    f32 f = h * 6.0f - ( f32 )i;
    f32 p = v * ( 1.0f - s );
    f32 q = v * ( 1.0f - s * f );
    f32 t = v * ( 1.0f - s * ( 1.0f - f ) );

    switch ( i % 6 )
    {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return 0xFF000000u | ( ( u32 )( b * 255.0f ) << 16 ) | ( ( u32 )( g * 255.0f ) << 8 )
           | ( u32 )( r * 255.0f );
}

static u32
tl_zone_color( u32 id, bool hover )
{
    f32 hue = ( f32 )( ( id * 2654435769u ) >> 8 ) * ( 1.0f / 16777216.0f );
    return tl_hsv_abgr( hue, 0.55f, hover ? 0.95f : 0.58f );
}

/* Human duration: picks s / ms / us / ns by magnitude. */
static void
tl_fmt_time( f64 ns, char* out, u32 cap )
{
    f64 a = ns < 0.0 ? -ns : ns;

    if ( a >= 1e9 )
        snprintf( out, cap, "%.2f s", ns / 1e9 );
    else if ( a >= 1e6 )
        snprintf( out, cap, "%.2f ms", ns / 1e6 );
    else if ( a >= 1e3 )
        snprintf( out, cap, "%.1f us", ns / 1e3 );
    else
        snprintf( out, cap, "%.0f ns", ns );
}

/* Smallest 1/2/5 * 10^k >= raw -- the classic "nice" ruler step. */
static f64
tl_nice_step( f64 raw )
{
    f64 step = 1.0;
    while ( step < raw )
    {
        if ( step * 2.0 >= raw ) { step *= 2.0; break; }
        if ( step * 5.0 >= raw ) { step *= 5.0; break; }
        step *= 10.0;
    }
    return step;
}

static const char*
tl_zone_name( u32 id )
{
    const char* name = prof_name_lookup( id );
    return name ? name : "<unregistered>";
}

/*==============================================================================================
    Capture -- fold drained events into span history
==============================================================================================*/

static void
tl_ingest( u32 t, const prof_event_t* e )
{
    tl_state_t* s  = &g_tl;
    tl_track_t* tr = &g_tl_tracks[ t ];

    if ( !s->t_origin )
        s->t_origin = e->tick_ns;
    if ( e->tick_ns > s->t_latest )
        s->t_latest = e->tick_ns;

    switch ( e->type )
    {
        case PROF_EV_BEGIN:
        {
            if ( tr->depth < TL_STACK_MAX )
            {
                tr->open[ tr->depth ].t0 = e->tick_ns;
                tr->open[ tr->depth ].id = e->id;
            }
            tr->depth++;    /* count past the stack cap so deep pops still rebalance */
            break;
        }

        case PROF_EV_END:
        {
            if ( tr->depth == 0 )
                break;    /* orphan END: capture started mid-zone; nothing to pair */
            tr->depth--;
            if ( tr->depth < TL_STACK_MAX )
            {
                tl_span_t* sp = &tr->spans[ tr->head++ & ( TL_SPAN_CAP - 1 ) ];
                sp->t0        = tr->open[ tr->depth ].t0;
                sp->t1        = e->tick_ns;
                sp->id        = tr->open[ tr->depth ].id;
                sp->depth     = ( u16 )tr->depth;
                if ( tr->depth > tr->max_depth )
                    tr->max_depth = tr->depth;
            }
            break;
        }

        case PROF_EV_FRAME:
        {
            tl_frame_mark_t* fr = &g_tl_frames[ s->frame_head++ & ( TL_FRAME_CAP - 1 ) ];
            fr->tick            = e->tick_ns;
            fr->number          = e->id;
            break;
        }

        default:
            break;
    }
}

void
gui_timeline_update( void )
{
    tl_state_t* s = &g_tl;

    /* One-consumer rule: the dump / hitch monitor own the drain while active. */
    s->blocked = prof_dump_active() || prof_hitch_armed();
    if ( s->blocked )
        return;

    /* Paused = capture STOPPED, not just the view frozen: a pause can last minutes while a
       frame is investigated, and draining on would wrap the span rings right through the
       data on screen. The producers keep writing until their prof rings fill, then drop
       (cheap, counted); the stale backlog is discarded wholesale on resume. */
    if ( !s->live )
    {
        s->was_live = false;
        return;
    }

    u32 threads = prof_thread_count();
    if ( threads > PROF_MAX_THREADS )
        threads = PROF_MAX_THREADS;

    if ( !s->was_live )
    {
        /* Resuming: everything pending is pause-old and full of overflow holes -- dropped
           BEGINs would mis-pair the stack walk. Throw the backlog away and reset the fold
           stacks; unpaired ENDs from zones begun before the resume then land on depth 0 and
           are ignored. History up to the pause point stays valid on screen. The drops the
           pause deliberately caused are baselined out of the readout. */
        s->drop_base = 0;
        for ( u32 t = 0; t < threads; ++t )
        {
            while ( prof_drain( t, g_tl_buf, TL_DRAIN_CHUNK ) != 0 ) {}
            g_tl_tracks[ t ].depth = 0;
            s->drop_base += prof_thread_dropped( t );
        }
        s->was_live = true;
    }

    for ( u32 t = 0; t < threads; ++t )
    {
        u32 n;
        while ( ( n = prof_drain( t, g_tl_buf, TL_DRAIN_CHUNK ) ) != 0 )
        {
            for ( u32 i = 0; i < n; ++i )
                tl_ingest( t, &g_tl_buf[ i ] );
            s->drained += n;
        }
    }
}

bool
gui_timeline_is_live( void )
{
    return g_tl.live;
}

void
gui_timeline_clear( void )
{
    bool live = g_tl.live;
    f64  span = g_tl.span_ns;

    memset( &g_tl, 0, sizeof( g_tl ) );
    memset( g_tl_tracks, 0, sizeof( g_tl_tracks ) );
    memset( g_tl_frames, 0, sizeof( g_tl_frames ) );

    g_tl.live     = live;
    g_tl.was_live = live;
    g_tl.span_ns  = span;

    /* Restart the drops readout at zero too -- prof's counters are lifetime-cumulative. */
    u32 threads = prof_thread_count();
    if ( threads > PROF_MAX_THREADS )
        threads = PROF_MAX_THREADS;
    for ( u32 t = 0; t < threads; ++t )
        g_tl.drop_base += prof_thread_dropped( t );
}

/*==============================================================================================
    View -- input on the tracks canvas resolves this frame's window before anything draws
==============================================================================================*/

static void
tl_view_input( gui_rect_t r )
{
    tl_state_t* s = &g_tl;

    /* The whole tracks area is one custom widget: item() gives it hover / press-capture
       exactly like a stock widget, and holding it pans the window. */
    gui_item_state_t st = gui()->item( "##tl_tracks", r );

    f32 mx, my;
    gui()->get_mouse_pos( &mx, &my );

    f64 ns_per_px = s->span_ns / ( r.w > 1.0f ? ( f64 )r.w : 1.0 );

    if ( st.pressed )
        s->drag_x = mx;
    if ( st.active )
    {
        f32 dx = mx - s->drag_x;
        if ( dx != 0.0f )
        {
            s->view_t1 -= ( f64 )dx * ns_per_px;    /* drag content right = look earlier */
            s->drag_x = mx;
            s->live   = false;
        }
    }

    /* Wheel zoom anchored at the cursor: the time under the mouse stays put (live keeps the
       right edge pinned instead, so the newest data never slides away mid-zoom). */
    if ( st.hover )
    {
        f32 wheel = gui()->get_mouse_wheel();
        if ( wheel != 0.0f )
        {
            f64 t_mouse = ( s->view_t1 - s->span_ns ) + ( f64 )( mx - r.x ) * ns_per_px;
            f64 factor  = 1.0;
            for ( f32 w = wheel; w >= 1.0f; w -= 1.0f ) factor *= 0.8;
            for ( f32 w = wheel; w <= -1.0f; w += 1.0f ) factor *= 1.25;

            f64 span = s->span_ns * factor;
            if ( span < 5.0e3 ) span = 5.0e3;          /* 5 us floor  */
            if ( span > 30.0e9 ) span = 30.0e9;        /* 30 s ceiling */

            if ( !s->live )
            {
                f64 frac   = ( s->view_t1 - t_mouse ) / s->span_ns;
                s->view_t1 = t_mouse + frac * span;
            }
            s->span_ns = span;
        }
    }

    /* Live follow, and a loose clamp so a pan cannot strand the view outside the capture. */
    if ( s->live )
        s->view_t1 = ( f64 )s->t_latest;
    else if ( s->t_origin )
    {
        if ( s->view_t1 > ( f64 )s->t_latest + s->span_ns )
            s->view_t1 = ( f64 )s->t_latest + s->span_ns;
        if ( s->view_t1 < ( f64 )s->t_origin )
            s->view_t1 = ( f64 )s->t_origin;
    }
}

/*==============================================================================================
    Frame strip -- recent frame durations as bars, newest at the right; click focuses a frame
==============================================================================================*/

static void
tl_draw_frame_strip( gui_rect_t r )
{
    tl_state_t* s = &g_tl;

    gui()->draw_rect( r.x, r.y, r.w, r.h, 0x30000000 );
    gui()->push_clip( r.x, r.y, r.w, r.h );

    /* 60 fps guide line (16.7 of the 33.3 ms bar scale). */
    f32 guide_y = r.y + r.h - r.h * ( 16.7f / 33.3f );
    gui()->draw_dashed_line( r.x, guide_y, r.x + r.w, guide_y, 4.0f, 4.0f, 1.0f, 0x3CFFFFFF );

    bool hover_strip = gui()->is_mouse_hovering_rect( r );
    f32  mx, my;
    gui()->get_mouse_pos( &mx, &my );

    const f32 bw = 4.0f, gap = 1.0f;
    u32       count = s->frame_head < TL_FRAME_CAP ? s->frame_head : TL_FRAME_CAP;

    /* Bar k covers the interval between marks (newest-1-k) and (newest-k). */
    for ( u32 k = 0; k + 1 < count; ++k )
    {
        tl_frame_mark_t* f1 = &g_tl_frames[ ( s->frame_head - 1 - k ) & ( TL_FRAME_CAP - 1 ) ];
        tl_frame_mark_t* f0 = &g_tl_frames[ ( s->frame_head - 2 - k ) & ( TL_FRAME_CAP - 1 ) ];

        f32 x = r.x + r.w - ( f32 )( k + 1 ) * ( bw + gap );
        if ( x < r.x )
            break;

        f64 dur_ms = ( f64 )( f1->tick - f0->tick ) / 1.0e6;
        f32 frac   = ( f32 )( dur_ms / 33.3 );
        if ( frac > 1.0f ) frac = 1.0f;
        if ( frac < 0.06f ) frac = 0.06f;    /* keep tiny frames clickable */

        u32 col = dur_ms <= 8.0  ? 0xFF4CB04C     /* green  */
                : dur_ms <= 16.7 ? 0xFF50C8DC     /* yellow */
                : dur_ms <= 33.3 ? 0xFF3C8CE6     /* orange */
                                 : 0xFF4646DC;    /* red    */

        f32  bh    = r.h * frac;
        bool hover = hover_strip && mx >= x && mx < x + bw + gap && my >= r.y && my < r.y + r.h;
        if ( hover )
        {
            col |= 0x00303030;
            char dur[ 32 ];
            tl_fmt_time( dur_ms * 1.0e6, dur, sizeof( dur ) );
            char label[ 64 ];
            snprintf( label, sizeof( label ), "frame %u  %s", f1->number, dur );
            gui()->draw_text( r.x + 4.0f, r.y + 2.0f, 0xFFD0D0D0, label );

            /* Click: focus that frame -- pause and frame the window around it. */
            if ( gui()->is_mouse_clicked( APP_MOUSE_LEFT ) )
            {
                f64 dur_ns = ( f64 )( f1->tick - f0->tick );
                s->live    = false;
                s->span_ns = dur_ns * 1.3;
                s->view_t1 = ( f64 )f1->tick + dur_ns * 0.15;
            }
        }

        gui()->draw_rect( x, r.y + r.h - bh, bw, bh, col );
    }

    gui()->pop_clip();
}

/*==============================================================================================
    Ruler -- time labels relative to the first captured tick
==============================================================================================*/

static void
tl_draw_ruler( gui_rect_t r )
{
    tl_state_t* s = &g_tl;

    gui()->push_clip( r.x, r.y, r.w, r.h );
    gui()->draw_line( r.x, r.y + r.h - 1.0f, r.x + r.w, r.y + r.h - 1.0f, 1.0f, 0xFF505050 );

    if ( s->t_origin )
    {
        f64 t1     = s->view_t1;
        f64 t0     = t1 - s->span_ns;
        f64 px_per = ( f64 )r.w / s->span_ns;
        f64 step   = tl_nice_step( 80.0 / px_per );

        f64 t = ( f64 )( ( i64 )( t0 / step ) ) * step;
        while ( t < t0 )
            t += step;

        for ( ; t <= t1; t += step )
        {
            f32 x = r.x + ( f32 )( ( t - t0 ) * px_per );
            gui()->draw_line( x, r.y + r.h - 6.0f, x, r.y + r.h, 1.0f, 0xFF787878 );

            char label[ 32 ];
            tl_fmt_time( t - ( f64 )s->t_origin, label, sizeof( label ) );
            gui()->draw_text( x + 3.0f, r.y + 1.0f, 0xFF909090, label );
        }
    }

    gui()->pop_clip();
}

/*==============================================================================================
    Tracks -- one flame graph per thread ring
==============================================================================================*/

/* Draw one bar; returns true when the cursor is on it (hover bookkeeping at the call site). */
static bool
tl_draw_bar( f32 x0, f32 x1, f32 y, u32 id, bool can_hover, f32 mx, f32 my, bool open )
{
    if ( x1 - x0 < 1.0f )
        x1 = x0 + 1.0f;

    bool hover = can_hover && mx >= x0 && mx < x1 && my >= y && my < y + TL_BAR_H;
    u32  col   = tl_zone_color( id, hover );
    if ( open )
        col = ( col & 0x00FFFFFF ) | 0xB0000000;    /* still running: slightly translucent */

    gui()->draw_rect( x0, y, x1 - x0, TL_BAR_H, col );

    if ( x1 - x0 >= 28.0f )
    {
        gui_rect_t tr = { x0 + 3.0f, y, x1 - x0 - 6.0f, TL_BAR_H };
        gui()->draw_text_clipped( tr, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, 0xFF0E0E0E,
                                  tl_zone_name( id ) );
    }
    return hover;
}

static void
tl_draw_tracks( gui_rect_t r )
{
    tl_state_t* s = &g_tl;

    gui()->push_clip( r.x, r.y, r.w, r.h );
    gui()->draw_rect( r.x, r.y, r.w, r.h, 0x28000000 );

    if ( !s->t_origin )
    {
        gui()->draw_text_in( r, GUI_ALIGN_CENTER, 0xFF808080, "waiting for capture..." );
        gui()->pop_clip();
        return;
    }

    f64 t1     = s->view_t1;
    f64 t0     = t1 - s->span_ns;
    f64 px_per = ( f64 )r.w / s->span_ns;

    /* Frame boundaries behind the bars: newest backward, stop once past the left edge. */
    u32 fcount = s->frame_head < TL_FRAME_CAP ? s->frame_head : TL_FRAME_CAP;
    for ( u32 k = 0; k < fcount; ++k )
    {
        tl_frame_mark_t* fr = &g_tl_frames[ ( s->frame_head - 1 - k ) & ( TL_FRAME_CAP - 1 ) ];
        if ( ( f64 )fr->tick < t0 )
            break;
        if ( ( f64 )fr->tick > t1 )
            continue;
        f32 x = r.x + ( f32 )( ( ( f64 )fr->tick - t0 ) * px_per );
        gui()->draw_line( x, r.y, x, r.y + r.h, 1.0f, 0x24FFFFFF );
    }

    bool can_hover = gui()->is_mouse_hovering_rect( r );
    f32  mx, my;
    gui()->get_mouse_pos( &mx, &my );
    s->hover.valid = false;

    u32 threads = prof_thread_count();
    if ( threads > PROF_MAX_THREADS )
        threads = PROF_MAX_THREADS;

    f32 ty = r.y + 4.0f;
    for ( u32 t = 0; t < threads; ++t )
    {
        tl_track_t* tr = &g_tl_tracks[ t ];
        if ( !tr->head && !tr->depth )
            continue;    /* ring claimed but no zones yet (or a pure counter thread) */

        u32 rows = tr->max_depth + 1;
        if ( rows > TL_DEPTH_ROWS )
            rows = TL_DEPTH_ROWS;

        /* Completed spans, oldest to newest. last_x1 per row implements the sub-pixel LOD:
           a span that does not advance the row's right edge by ~a pixel is skipped, so a
           thousand micro-zones cost a handful of rects instead of a thousand. */
        f32 last_x1[ TL_DEPTH_ROWS ];
        for ( u32 d = 0; d < TL_DEPTH_ROWS; ++d )
            last_x1[ d ] = -1.0e9f;

        u32 count = tr->head < TL_SPAN_CAP ? tr->head : TL_SPAN_CAP;
        for ( u32 k = 0; k < count; ++k )
        {
            tl_span_t* sp = &tr->spans[ ( tr->head - count + k ) & ( TL_SPAN_CAP - 1 ) ];
            if ( sp->depth >= rows )
                continue;
            if ( ( f64 )sp->t1 < t0 || ( f64 )sp->t0 > t1 )
                continue;

            f32 x0 = r.x + ( f32 )( ( ( f64 )sp->t0 - t0 ) * px_per );
            f32 x1 = r.x + ( f32 )( ( ( f64 )sp->t1 - t0 ) * px_per );
            if ( x1 - last_x1[ sp->depth ] < 0.75f )
                continue;
            last_x1[ sp->depth ] = x1;

            f32 y = ty + ( f32 )sp->depth * TL_ROW_H;
            if ( tl_draw_bar( x0, x1, y, sp->id, can_hover, mx, my, false ) )
            {
                s->hover = ( tl_hover_t ){ .valid  = true,
                                           .open   = false,
                                           .id     = sp->id,
                                           .thread = t,
                                           .t0     = sp->t0,
                                           .t1     = sp->t1,
                                           .depth  = sp->depth };
            }
        }

        /* Open zones: begun, not yet ended -- drawn out to the newest tick. */
        u32 od = tr->depth < TL_STACK_MAX ? tr->depth : TL_STACK_MAX;
        for ( u32 d = 0; d < od && d < rows; ++d )
        {
            if ( ( f64 )tr->open[ d ].t0 > t1 )
                continue;
            f32 x0 = r.x + ( f32 )( ( ( f64 )tr->open[ d ].t0 - t0 ) * px_per );
            f32 x1 = r.x + ( f32 )( ( ( f64 )s->t_latest - t0 ) * px_per );
            if ( x0 < r.x ) x0 = r.x;
            f32 y = ty + ( f32 )d * TL_ROW_H;
            if ( tl_draw_bar( x0, x1, y, tr->open[ d ].id, can_hover, mx, my, true ) )
            {
                s->hover = ( tl_hover_t ){ .valid  = true,
                                           .open   = true,
                                           .id     = tr->open[ d ].id,
                                           .thread = t,
                                           .t0     = tr->open[ d ].t0,
                                           .t1     = s->t_latest,
                                           .depth  = d };
            }
        }

        /* Thread label floats over the track's top-left; drops last so it stays readable. */
        {
            const char* label = prof_thread_label( t );
            char        text[ 48 ];
            snprintf( text, sizeof( text ), "%s (tid %u)",
                      label && label[ 0 ] ? label : "thread", t );
            gui_vec2_t sz = gui()->text_size( text );
            gui()->draw_rect( r.x + 2.0f, ty, sz.x + 8.0f, sz.y + 2.0f, 0xC0141414 );
            gui()->draw_text( r.x + 6.0f, ty + 1.0f, 0xFFC8C8C8, text );
        }

        ty += ( f32 )rows * TL_ROW_H + TL_TRACK_GAP;
        gui()->draw_line( r.x, ty - TL_TRACK_GAP * 0.5f, r.x + r.w, ty - TL_TRACK_GAP * 0.5f,
                          1.0f, 0xFF383838 );

        if ( ty > r.y + r.h )
            break;    /* out of vertical space; remaining tracks are clipped anyway */
    }

    gui()->pop_clip();
}

/*==============================================================================================
    Window
==============================================================================================*/

static void
tl_controls( void )
{
    tl_state_t* s = &g_tl;

    gui()->bar();

    if ( gui()->button( s->live ? "Pause" : "Live" ) )
    {
        s->live = !s->live;
        if ( s->live )
            s->view_t1 = ( f64 )s->t_latest;
    }
    if ( gui()->button( "Clear" ) )
        gui_timeline_clear();

    u32 drops = 0;
    u32 threads = prof_thread_count();
    if ( threads > PROF_MAX_THREADS )
        threads = PROF_MAX_THREADS;
    for ( u32 t = 0; t < threads; ++t )
        drops += prof_thread_dropped( t );
    drops = drops > s->drop_base ? drops - s->drop_base : 0;

    char span[ 32 ];
    tl_fmt_time( s->span_ns, span, sizeof( span ) );
    gui()->textf( "  view %s   events %llu   drops %u%s", span,
                  ( unsigned long long )s->drained, drops,
                  s->live ? "" : "   [paused -- capture stopped; drag to pan, wheel to zoom]" );
}

void
gui_timeline_window( void )
{
    tl_state_t* s = &g_tl;

    gui()->window_set_next_pos( 20.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 1240.0f, 440.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Profiler Timeline", GUI_WIN_NONE ) )
    {
        tl_controls();

        gui()->stack();
        if ( s->blocked )
        {
            gui()->text( prof_dump_active()
                             ? "standing down: a prof_dump capture owns the drain"
                             : "standing down: the armed hitch monitor owns the drain" );
        }

        /* Reserve all three bands first (canvas only reserves + returns the rect), resolve
           this frame's view from input on the tracks band, THEN draw -- so the ruler and the
           bars always agree on the window instead of lagging each other by a frame. */
        gui_rect_t strip_r  = gui()->canvas( 30.0f );
        gui_rect_t ruler_r  = gui()->canvas( 18.0f );
        gui_rect_t tracks_r = gui()->canvas( 0.0f );

        tl_view_input( tracks_r );
        tl_draw_frame_strip( strip_r );
        tl_draw_ruler( ruler_r );
        tl_draw_tracks( tracks_r );

        /* Tooltip for the span under the cursor (the tracks item() is the previous widget). */
        if ( s->hover.valid && gui()->tooltip_begin() )
        {
            gui()->stack();
            gui()->textf( "%s%s", tl_zone_name( s->hover.id ), s->hover.open ? "  (open)" : "" );

            char dur[ 32 ], start[ 32 ];
            tl_fmt_time( ( f64 )( s->hover.t1 - s->hover.t0 ), dur, sizeof( dur ) );
            tl_fmt_time( ( f64 )( s->hover.t0 - s->t_origin ), start, sizeof( start ) );
            gui()->textf( "duration  %s", dur );
            gui()->textf( "start     +%s", start );
            gui()->textf( "depth     %u", s->hover.depth );
            gui()->textf( "thread    %s", prof_thread_label( s->hover.thread ) );
            gui()->tooltip_end();
        }
    }
    gui()->window_end();
}

/*============================================================================================*/
