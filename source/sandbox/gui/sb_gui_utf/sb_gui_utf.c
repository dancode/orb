/*==============================================================================================

    sandbox/gui/sb_gui_utf/sb_gui_utf.c -- the EXTENDED-CHARACTER demo: what lifting the ASCII
    ceiling bought, one window per claim.

        Scripts         Latin-1 / Latin Extended-A / Greek / Cyrillic rendering from one
                        -range bake -- the sparse .orb_font records and the two-tier lookup
        Type & Edit     live single-line + multiline editing over UTF-8: accented input,
                        whole-character caret/backspace, clipboard round-trip (CF_UNICODETEXT)
        Anatomy         a byte-vs-codepoint inspector over a live buffer -- watch a 3-byte
                        euro stay ONE character to the caret and the measure
        SDF Rotation    a distance-field -range bake spinning accented text through
                        draw_text_xf -- the extended pipeline ends at the same one quad

    Strings in this SOURCE are ASCII by project rule, so every extended character is spelled
    as \x UTF-8 bytes with the readable form in a comment beside it.

    ASSETS.  assets/font is generated and not tracked; every panel degrades to its bake
    command when the font is missing:

        bin\font_tool.exe CascadiaMono 16 "-range=latin,greek,cyrillic,0x20AC"
        bin\font_tool.exe CascadiaMono 32 -sdf "-range=latin1,0x20AC"

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/utf8.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

#define INK       GUI_COLOR( 0xE8, 0xE0, 0xD0, 0xFF )
#define INK_DIM   GUI_COLOR( 0x90, 0x8C, 0x84, 0xFF )
#define AMBER     GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF )
#define TEAL      GUI_COLOR( 0x20, 0xC0, 0xB0, 0xFF )
#define ACCENT    GUI_COLOR( 0x4C, 0x9E, 0xFF, 0xFF )
#define VIOLET    GUI_COLOR( 0xB0, 0x70, 0xFF, 0xFF )
#define WARN      GUI_COLOR( 0xFF, 0x70, 0x50, 0xFF )

#define TAU       6.28318530717959f

static f32 s_time;

/*==============================================================================================
    The two -range bakes under test.  Loaded by path like every generated asset; each window
    says how to bake its font when it is missing rather than quietly showing ASCII.
==============================================================================================*/

#define EURO_ASSET  "CascadiaMono_16px_latin-greek-cyrillic-0x20ac.orb_font"
#define SDF_ASSET   "CascadiaMono_32px_latin1-0x20ac_sdf.orb_font"
#define EURO_BAKE   "bin\\font_tool.exe CascadiaMono 16 \"-range=latin,greek,cyrillic,0x20AC\""
#define SDF_BAKE    "bin\\font_tool.exe CascadiaMono 32 -sdf \"-range=latin1,0x20AC\""

static u32 s_font_euro;   /* 0 = missing: windows fall back to the built-in ASCII face */
static u32 s_font_sdf;

static u32
font_try( const char* asset )
{
    char path[ 576 ];
    snprintf( path, sizeof( path ), "%s/assets/font/%s", sys_root_dir(), asset );

    u32 prev = gui()->font_active_id();
    u32 id   = gui()->font_load( path );
    gui()->font_use( prev );
    return id;
}

static void
load_fonts( void )
{
    s_font_euro = font_try( EURO_ASSET );
    s_font_sdf  = font_try( SDF_ASSET );

    printf( "[sb_gui_utf] scripts %s   sdf %s\n",
            s_font_euro ? EURO_ASSET : "MISSING",
            s_font_sdf  ? SDF_ASSET  : "MISSING" );
    fflush( stdout );
}

/* The missing-font banner every window shares: name the asset, print the bake line. */
static void
bake_hint( const char* asset, const char* cmd )
{
    gui()->text_colored( WARN, "font missing -- glyphs below the ceiling will show as '?'" );
    gui()->text_disabled( asset );
    gui()->text_disabled( cmd );
    gui()->separator();
}

/*==============================================================================================
    Scripts -- one -range bake carrying four scripts plus the euro sign.  Every line pairs the
    sample with what it demonstrates; the font is swapped in around the samples only, so the
    surrounding labels stay in the default face (mixing faces mid-window is itself the point:
    fonts are ids, extended lookup is per-slot).
==============================================================================================*/

static void
script_line( const char* what, const char* sample )
{
    gui()->text_disabled( what );
    if ( s_font_euro ) gui()->font_use( s_font_euro );
    gui()->text( sample );
    gui()->font_use( 0 );
}

static void
win_scripts( void )
{
    gui()->stack();
    if ( !s_font_euro )
        bake_hint( EURO_ASSET, EURO_BAKE );

    gui()->text_wrapped( "One .orb_font baked with -range=latin,greek,cyrillic,0x20AC: 523 "
                         "glyph records, sparse by codepoint, resolved through the dense ASCII "
                         "table + a binary-searched extension tier." );
    gui()->separator();

    gui()->cols( (f32[]){ 90.0f, 1.0f, GUI_END } );   /* label track + fill; repeats per line */

    /* Readable forms in the trailing comments -- the escapes ARE the UTF-8 bytes. */
    script_line( "latin-1",   "Caf\xC3\xA9 na\xC3\xAFve \xC3\xA0 la fa\xC3\xA7""ade \xC2\xBFqu\xC3\xA9?" );        /* Cafe naive a la facade que */
    script_line( "german",    "Stra\xC3\x9F""e \xC3\x9C""berma\xC3\x9F GR\xC3\x9C\xC3\x9F""E" );                    /* Strasse Ubermass GRUSSE    */
    script_line( "nordic",    "Sm\xC3\xB8rrebr\xC3\xB8""d \xC3\x85sa \xC3\x86on" );                                 /* Smorrebrod Asa Aeon        */
    script_line( "ext-A",     "\xC4\x8C""apek \xC5\x81""ukasz \xC5\x90rs\xC3\xA9g \xC5\xBB""ubr\xC3\xB3wka" );      /* Capek Lukasz Orseg Zubrowka*/
    script_line( "greek",     "\xCE\x91\xCE\xBB\xCF\x86\xCE\xAC\xCE\xB2\xCE\xB7\xCF\x84\xCE\xBF "
                              "\xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1 \xCF\x80\xCE\xB1\xCE\xBD" );                /* Alphabeto Omega pan        */
    script_line( "cyrillic",  "\xD0\x91\xD1\x83\xD0\xBA\xD0\xB2\xD1\x8B \xD0\x96\xD1\x83\xD1\x80\xD0\xBD\xD0\xB0"
                              "\xD0\xBB \xD0\xAD\xD1\x85\xD0\xBE" );                                                /* Bukvy Zhurnal Ekho         */
    script_line( "currency",  "\xE2\x82\xAC 9,99   \xC2\xA3 12   \xC2\xA5 1400   \xC2\xA2 50" );                    /* EUR GBP JPY cent           */
    script_line( "unmapped",  "CJK stays a later campaign: \xE4\xBD\xA0\xE5\xA5\xBD" );                             /* ni hao -> two '?'          */

    gui()->stack();
    gui()->separator();
    gui()->text_wrapped( "The last line is deliberate: codepoints the bake does not carry "
                         "resolve to '?' at the shared lookup, so measure and draw agree even "
                         "about what is missing." );
}

/*==============================================================================================
    Type & Edit -- live editing over UTF-8 buffers.  Everything here is the stock widgets; the
    demo is what the caret DOES: one Left/Right or Backspace per accented character, double-
    click selecting a whole accented word, clipboard round-tripping through CF_UNICODETEXT.
==============================================================================================*/

static char s_line [ 128 ] = "Caf\xC3\xA9 \xE2\x82\xAC 9,99 na\xC3\xAFve";                       /* Cafe EUR naive */
static char s_multi[ 1024 ] =
    "Gr\xC3\xBC\xC3\x9F""e aus M\xC3\xBCnchen!\n"                                                /* Gruesse aus Muenchen */
    "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1 \xCE\xB1\xCF\x80\xCF\x8C "
    "\xCF\x84\xCE\xB7\xCE\xBD \xCE\x91\xCE\xB8\xCE\xAE\xCE\xBD\xCE\xB1.\n"                       /* Kalimera apo tin Athina. */
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xB8\xD0\xB7 \xD0\x9C\xD0\xBE\xD1\x81"
    "\xD0\xBA\xD0\xB2\xD1\x8B.\n"                                                                /* Privet iz Moskvy. */
    "Try: type, paste, select, Ctrl+C into another app.";

static void
win_edit( void )
{
    gui()->stack();
    if ( !s_font_euro )
        bake_hint( EURO_ASSET, EURO_BAKE );

    gui()->text_wrapped( "The caret lives in BYTE space but moves in CHARACTERS: arrow through "
                         "the accents below and watch one keypress cross 2-3 bytes.  Backspace "
                         "erases a whole character; a full buffer never keeps half of one." );
    gui()->separator();

    if ( s_font_euro ) gui()->font_use( s_font_euro );
    gui()->input_text( "one line", s_line, sizeof( s_line ) );
    gui()->input_text_multiline( "editor", s_multi, sizeof( s_multi ), 0.0f );
    gui()->font_use( 0 );

    gui()->separator();
    gui()->text_disabled( "type accents via US-Intl dead keys or Alt codes (Alt+0233 = e-acute)" );
    gui()->text_disabled( "paste from any app; copy back out -- the clipboard is CF_UNICODETEXT" );
}

/*==============================================================================================
    Anatomy -- the inspector: walk a live buffer with utf8_decode and lay every character bare.
    THE window to open when byte-vs-character confuses someone: the euro is three bytes in the
    buffer, one column here, one caret step in the field above it.
==============================================================================================*/

static char s_probe[ 64 ] = "A\xC3\xA9\xE2\x82\xAC";                                            /* A e-acute EUR */

static void
win_anatomy( void )
{
    gui()->stack();
    if ( !s_font_euro )
        bake_hint( EURO_ASSET, EURO_BAKE );

    if ( s_font_euro ) gui()->font_use( s_font_euro );
    gui()->input_text( "probe", s_probe, sizeof( s_probe ) );
    gui()->font_use( 0 );

    u32 blen  = (u32)strlen( s_probe );
    i32 chars = utf8_count( s_probe, (i32)blen );

    char head[ 96 ];
    snprintf( head, sizeof( head ), "%u byte%s -- %d character%s",
              blen, blen == 1 ? "" : "s", chars, chars == 1 ? "" : "s" );
    gui()->text_colored( ACCENT, head );
    gui()->separator();

    /* glyph | code | size | raw bytes -- the template repeats row-major per 4 widgets. */
    gui()->cols( (f32[]){ 64.0f, 90.0f, 80.0f, 1.0f, GUI_END } );
    gui()->text_disabled( "glyph" );
    gui()->text_disabled( "code" );
    gui()->text_disabled( "size" );
    gui()->text_disabled( "bytes" );

    for ( u32 i = 0; i < blen; )
    {
        u32 adv;
        u32 cp = utf8_decode( s_probe + i, &adv );

        char one[ 8 ];
        memcpy( one, s_probe + i, adv );
        one[ adv ] = '\0';
        if ( s_font_euro ) gui()->font_use( s_font_euro );
        gui()->text( one );
        gui()->font_use( 0 );

        char buf[ 32 ];
        snprintf( buf, sizeof( buf ), "U+%04X", cp );
        gui()->text_colored( cp < 0x80u ? INK : TEAL, buf );

        snprintf( buf, sizeof( buf ), "%u byte%s", adv, adv == 1 ? "" : "s" );
        gui()->text( buf );

        char* p = buf;
        for ( u32 b = 0; b < adv; ++b )
            p += snprintf( p, sizeof( buf ) - (size_t)( p - buf ), "%02X ", (u8)s_probe[ i + b ] );
        gui()->text_disabled( buf );

        i += adv;
    }
    gui()->stack();
}

/*==============================================================================================
    SDF Rotation -- the distance-field pipeline is codepoint-blind: the same -range records,
    the same one-quad-per-glyph draw_text_xf, now carrying accents.  Animated, so it owes
    request_redraw every frame it is visible (the gui is idle-skipped).
==============================================================================================*/

static void
win_sdf( void )
{
    gui()->stack();
    if ( !s_font_sdf )
    {
        bake_hint( SDF_ASSET, SDF_BAKE );
        gui()->text_wrapped( "Without the bake this window has nothing to spin -- the built-in "
                             "face is a point-sampled coverage atlas and would shimmer." );
        return;
    }

    gui()->text_wrapped( "A latin1+euro distance-field bake under draw_text_xf.  Scale and "
                         "angle are free; the accents ride along because the pipeline never "
                         "knew about ASCII in the first place." );

    gui_rect_t cell = gui()->canvas( 300.0f );
    f32        cx   = cell.x + cell.w * 0.5f;
    f32        cy   = cell.y + cell.h * 0.5f;

    static const char* spin = "* Caf\xC3\xA9 \xC3\x9C""ber \xE2\x82\xAC 9,99 *";                /* Cafe Ueber EUR */

    gui()->font_use( s_font_sdf );

    f32 rot   = s_time * 0.6f;
    f32 scale = 0.65f + 0.35f * sinf( s_time * 0.9f );

    gui_vec2_t t  = gui()->text_size( spin );
    f32        hx = t.x * scale * 0.5f;
    f32        hy = t.y * scale * 0.5f;
    f32        c  = cosf( rot );
    f32        sn = sinf( rot );
    gui()->draw_text_xf( cx - ( hx * c - hy * sn ), cy - ( hx * sn + hy * c ),
                         AMBER, spin, scale, rot );

    /* A still ring under the spinner: the same run radiating from the center at six fixed
       angles (draw_text_xf turns about its anchor), so edge quality is judgeable at every
       orientation at once while the bright copy animates over it. */
    for ( u32 k = 0; k < 6; ++k )
        gui()->draw_text_xf( cx, cy, GUI_COLOR( 0x40, 0x44, 0x52, 0x60 ),
                             spin, 0.35f, (f32)k * ( TAU / 6.0f ) + TAU / 24.0f );

    gui()->font_use( 0 );
    gui()->request_redraw();   /* animated: pull the next frame through the idle skip */
}

/*==============================================================================================
    Frame -- four fixed windows, no registry: this suite is small enough to see whole.
==============================================================================================*/

static void
build_frame( void )
{
    gui()->window_set_next_pos ( 24.0f,  24.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 560.0f, 420.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Scripts", GUI_WIN_NONE ) )
        win_scripts();
    gui()->window_end();

    gui()->window_set_next_pos ( 24.0f,  464.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 560.0f, 400.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Type & Edit", GUI_WIN_NONE ) )
        win_edit();
    gui()->window_end();

    gui()->window_set_next_pos ( 604.0f, 24.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 470.0f, 420.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Anatomy", GUI_WIN_NONE ) )
        win_anatomy();
    gui()->window_end();

    gui()->window_set_next_pos ( 604.0f, 464.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 470.0f, 400.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "SDF Rotation", GUI_WIN_NONE ) )
        win_sdf();
    gui()->window_end();
}

/*==============================================================================================
    Host
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_utf] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- extended characters",
        .w         = 1100, .h = 900,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.10f, 0.10f, 0.13f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_utf] gui->boot failed\n" );
        goto shutdown;
    }

    load_fonts();

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        s_time += dt;

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame();
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        gui()->boot_pace ( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
/*============================================================================================*/
