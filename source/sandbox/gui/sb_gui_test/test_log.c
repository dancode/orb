/*==============================================================================================

    sandbox/gui/sb_gui_test/test_log.c -- the GUI_LOG sink contract.

    gui_log's whole value is that a host can take delivery of every gui diagnostic, so the
    delivery contract is the thing worth pinning: what a sink receives, in what shape, at what
    severity.  A sink that silently stopped being called, or started receiving the "[gui] "
    prefix it is supposed to own, would not fail anything -- it would just quietly stop being
    useful, which is the failure mode a log has to be immune to.

    GUI_WARN_ONCE gets its own case because "once per run" is the entire point of the
    loud-overflow rule: a per-frame pool overflow that reported every frame would flood the log
    it is trying to be visible in.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Capturing sink.
==============================================================================================*/

#define LOGT_CAP 8

typedef struct
{
    gui_log_level_t level[ LOGT_CAP ];
    char            msg  [ LOGT_CAP ][ GUI_LOG_MSG_MAX ];
    void*           user [ LOGT_CAP ];
    u32             count;

} logt_capture_t;

static logt_capture_t s_cap;

static void
logt_sink( gui_log_level_t level, const char* msg, void* user )
{
    if ( s_cap.count >= LOGT_CAP )
        return;

    s_cap.level[ s_cap.count ] = level;
    s_cap.user [ s_cap.count ] = user;

    u32 i = 0;
    for ( ; msg[ i ] && i + 1u < GUI_LOG_MSG_MAX; ++i )
        s_cap.msg[ s_cap.count ][ i ] = msg[ i ];
    s_cap.msg[ s_cap.count ][ i ] = '\0';

    s_cap.count++;
}

static void
logt_reset( void )
{
    s_cap.count = 0;
    gui_log_set_fn( logt_sink, NULL );
}

/*==============================================================================================
    Delivery, formatting, severity.
==============================================================================================*/

static void
test_log_delivery( void )
{
    logt_reset();

    gui_log( GUI_LOG_INFO, "plain" );
    test_equal( 1u, s_cap.count );
    test_cstr_equal( "plain", s_cap.msg[ 0 ] );
    test_equal( GUI_LOG_INFO, s_cap.level[ 0 ] );

    /* Formatting happens before the sink sees it -- the sink takes a finished string, so it
       never has to know or replicate the format vocabulary. */
    gui_log( GUI_LOG_WARN, "%s=%d (%.1f)", "count", 42, 1.5f );
    test_cstr_equal( "count=42 (1.5)", s_cap.msg[ 1 ] );
    test_equal( GUI_LOG_WARN, s_cap.level[ 1 ] );

    gui_log( GUI_LOG_ERROR, "bad" );
    test_equal( GUI_LOG_ERROR, s_cap.level[ 2 ] );

    test_equal( 3u, s_cap.count );

    gui_log_set_fn( NULL, NULL );
}

static void
test_log_user_pointer( void )
{
    int marker = 7;

    s_cap.count = 0;
    gui_log_set_fn( logt_sink, &marker );
    gui_log( GUI_LOG_INFO, "x" );

    test_equal( 1u, s_cap.count );
    test_true( s_cap.user[ 0 ] == &marker );

    gui_log_set_fn( NULL, NULL );
}

/*==============================================================================================
    Framing -- the sink owns its own prefix and line ending.
==============================================================================================*/

static void
test_log_framing( void )
{
    logt_reset();

    /* The migrated call sites kept their trailing "\n" from the printf they replaced, so the
       stripping is what lets both spellings deliver the same message.  Both forms must arrive
       identically -- otherwise a sink writing one line per message emits blank lines for half
       the call sites. */
    gui_log( GUI_LOG_INFO, "same text\n" );
    gui_log( GUI_LOG_INFO, "same text" );
    test_cstr_equal( "same text", s_cap.msg[ 0 ] );
    test_cstr_equal( "same text", s_cap.msg[ 1 ] );
    test_cstr_equal( s_cap.msg[ 0 ], s_cap.msg[ 1 ] );

    /* Windows line endings and repeated terminators strip too. */
    gui_log( GUI_LOG_INFO, "crlf\r\n" );
    test_cstr_equal( "crlf", s_cap.msg[ 2 ] );

    gui_log( GUI_LOG_INFO, "many\n\n\n" );
    test_cstr_equal( "many", s_cap.msg[ 3 ] );

    /* No "[gui] " prefix and no severity word: the sink adds whatever framing it wants, and a
       core-log sink would otherwise double up on the category it already stamps. */
    gui_log( GUI_LOG_WARN, "unprefixed" );
    test_cstr_equal( "unprefixed", s_cap.msg[ 4 ] );

    /* An interior newline is NOT stripped -- only the trailing run is. */
    gui_log( GUI_LOG_INFO, "a\nb\n" );
    test_cstr_equal( "a\nb", s_cap.msg[ 5 ] );

    /* A message that is nothing but newlines collapses to empty rather than misbehaving. */
    gui_log( GUI_LOG_INFO, "\n" );
    test_cstr_equal( "", s_cap.msg[ 6 ] );

    gui_log_set_fn( NULL, NULL );
}

/*==============================================================================================
    Truncation -- bounded, NUL-terminated, never a split or a drop.
==============================================================================================*/

static void
test_log_truncation( void )
{
    static char big[ GUI_LOG_MSG_MAX * 2 ];

    for ( u32 i = 0; i < sizeof( big ) - 1u; ++i )
        big[ i ] = 'x';
    big[ sizeof( big ) - 1u ] = '\0';

    logt_reset();
    gui_log( GUI_LOG_INFO, "%s", big );

    /* Delivered once, clipped to the buffer, and still a valid C string. */
    test_equal( 1u, s_cap.count );
    test_equal( GUI_LOG_MSG_MAX - 1u, cstr_len( s_cap.msg[ 0 ] ) );
    test_true( s_cap.msg[ 0 ][ GUI_LOG_MSG_MAX - 1u ] == '\0' );

    /* A message that exactly fills the buffer is not corrupted at the seam. */
    test_true( s_cap.msg[ 0 ][ 0 ] == 'x' );
    test_true( s_cap.msg[ 0 ][ GUI_LOG_MSG_MAX - 2u ] == 'x' );

    gui_log_set_fn( NULL, NULL );
}

/*==============================================================================================
    Install / uninstall.
==============================================================================================*/

static void
test_log_set_fn( void )
{
    logt_reset();
    gui_log( GUI_LOG_INFO, "captured" );
    test_equal( 1u, s_cap.count );

    /* NULL restores the default sink: the callback must stop being called immediately, or a
       host that tore down its logger would keep being called into during shutdown. */
    gui_log_set_fn( NULL, NULL );
    gui_log( GUI_LOG_INFO, "(sb_gui_test: this line on stdout is the expected default sink)" );
    test_equal( 1u, s_cap.count );

    /* Re-installing resumes delivery. */
    gui_log_set_fn( logt_sink, NULL );
    gui_log( GUI_LOG_INFO, "captured again" );
    test_equal( 2u, s_cap.count );
    test_cstr_equal( "captured again", s_cap.msg[ 1 ] );

    gui_log_set_fn( NULL, NULL );
}

/*==============================================================================================
    GUI_WARN_ONCE -- the loud-overflow rule's report half.
==============================================================================================*/

static void
test_warn_once( void )
{
    logt_reset();

    /* One call SITE reports once, however many times it is reached: a saturating pool is hit
       every frame, and a report per frame would bury the message in its own repetition. */
    for ( u32 i = 0; i < 100u; ++i )
        GUI_WARN_ONCE( "pool full (%u)\n", 64u );

    test_equal( 1u, s_cap.count );
    test_equal( GUI_LOG_WARN, s_cap.level[ 0 ] );
    test_cstr_equal( "pool full (64)", s_cap.msg[ 0 ] );

    /* The latch is PER SITE, not global -- a second overflow elsewhere must still be heard. */
    GUI_WARN_ONCE( "a different pool\n" );
    test_equal( 2u, s_cap.count );
    test_cstr_equal( "a different pool", s_cap.msg[ 1 ] );

    gui_log_set_fn( NULL, NULL );
}

/*============================================================================================*/
