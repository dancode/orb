/*==============================================================================================

    str_new_test.c -- Demonstration and test for str_t / strbuf_t.

    This file shows why str_t + strbuf_t are superior to raw C strings for a game engine.
    Each section has a commentary block explaining the design benefit, followed by tests
    that exercise the code with ORB_ASSERT.

==============================================================================================*/
#include "orb.h"
#include "str_buf.h"   /* includes str.h transitively */
#include "str_arena.h"
#include <stdio.h>     /* printf for demo output */

/*==============================================================================================

    WHY THIS IS BETTER THAN C STRINGS -- OVERVIEW

    Problem with raw char*:
      1. Length is not stored -- every function must call strlen() to find it. If you pass
         a string through 10 call frames, strlen() fires 10 times.
      2. Sub-strings require allocation or manual bookkeeping (null-terminate in place, which
         mutates the source, or copy into a new buffer).
      3. No capacity -- overflow is the caller's problem and is easy to silently ignore.
      4. API inconsistency -- every function that takes a string needs to know: is this
         null-terminated? Where does it end? You never know without reading each signature.

    Solution:
      str_t  = {ptr, len}  -- read-only view, O(1) length, sub-strings are free.
      strbuf_t = {ptr, len, cap} -- writable buffer, always null-terminated, overflow tracked.

    The two types compose: strbuf_str() converts a strbuf_t into a str_t with zero cost.
    All read-only operations accept str_t. All write operations work on strbuf_t*.

==============================================================================================*/
// clang-format off
/*----------------------------------------------------------------------------------------------
    SECTION 1: Literal construction -- zero runtime cost

    The STR() macro uses sizeof to compute the length at compile time. The compiler evaluates
    the struct literal statically. Compare this to a typical C API:

        size_t n = strlen("shaders/basic.hlsl");    // runtime loop
        str_t  s = STR( "shaders/basic.hlsl" );     // compile-time constant
----------------------------------------------------------------------------------------------*/
static void
test_literal_construction( void )
{
    int32_t size   = sizeof( str_t );
    UNUSED( size );

    str_t shader = STR( "shaders/basic.hlsl" );
    ORB_ASSERT( shader.len == 18 );
    ORB_ASSERT( shader.ptr[ 0 ] == 's' );

    /* STR_EMPTY is a valid empty string -- ptr is not NULL, so it is safe to dereference. */
    ORB_ASSERT( str_is_empty( STR_EMPTY ) );
    ORB_ASSERT( STR_EMPTY.ptr != NULL );

    /* str_from_ptr_len when the length is already known (e.g. from parsing). */
    const char raw[] = { 'o', 'r', 'b', 0 };
    str_t s = str_from_ptr_len( raw, 3 );
    ORB_ASSERT( s.len == 3 );

    /* str_from_cstr -- the only place strlen() is called.
       After this, every operation on s is O(1) length access. */
    str_t from_c = str_from_cstr( "engine" );
    ORB_ASSERT( from_c.len == 6 );

    printf( "[PASS] literal construction\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 2: Sub-strings are free -- no allocation, no mutation

    With raw C strings, taking a sub-string means either:
      a) Copying into a new buffer (allocation required)
      b) Null-terminating in-place (mutates the source -- bad!)
      c) Passing both a pointer and a length separately (error-prone)

    With str_t, str_sub() just adjusts ptr and len. No allocation, no mutation.
    The sub-view shares memory with the original.
----------------------------------------------------------------------------------------------*/
static void
test_substrings_and_trim( void )
{
    str_t path = STR( "/engine/shaders/basic.hlsl" );

    /* Extract the filename -- no allocation, just index arithmetic. */
    i32 slash = str_rfind_char( path, '/' );
    ORB_ASSERT( slash != STR_NOT_FOUND );
    str_t filename = str_sub( path, slash + 1, path.len );
    ORB_ASSERT( str_equal( filename, STR( "basic.hlsl" ) ) );

    /* Extract extension. */
    i32 dot = str_rfind_char( filename, '.' );
    ORB_ASSERT( dot != STR_NOT_FOUND );
    str_t ext = str_suffix( filename, filename.len - dot );
    ORB_ASSERT( str_equal( ext, STR( ".hlsl" ) ) );

    /* Trim whitespace from both ends -- returns a sub-view, no allocation. */
    str_t padded = STR( "   hello world   " );
    str_t trimmed = str_trim( padded );
    ORB_ASSERT( str_equal( trimmed, STR( "hello world" ) ) );
    ORB_ASSERT( trimmed.ptr == padded.ptr + 3 );  /* same backing memory, shifted ptr */

    /* Prefix / suffix helpers. */
    str_t greeting = STR( "Hello, World!" );
    ORB_ASSERT( str_equal( str_prefix( greeting, 5 ), STR( "Hello" ) ) );
    ORB_ASSERT( str_equal( str_suffix( greeting, 6 ), STR( "World!" ) ) );

    /* str_trim_left / str_trim_right -- each direction independently. */
    ORB_ASSERT( str_equal( str_trim_left( STR( "   hello" ) ), STR( "hello" ) ) );
    ORB_ASSERT( str_equal( str_trim_left( STR( "hello" ) ), STR( "hello" ) ) );    /* no leading spaces */
    ORB_ASSERT( str_equal( str_trim_right( STR( "hello   " ) ), STR( "hello" ) ) );
    ORB_ASSERT( str_equal( str_trim_right( STR( "hello" ) ), STR( "hello" ) ) );   /* no trailing spaces */
    /* trim_left should not remove trailing; trim_right should not remove leading */
    ORB_ASSERT( str_equal( str_trim_left( STR( "  hi  " ) ), STR( "hi  " ) ) );
    ORB_ASSERT( str_equal( str_trim_right( STR( "  hi  " ) ), STR( "  hi" ) ) );

    printf( "[PASS] substrings and trim\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 3: Comparison and search

    str_equal checks length first -- if lengths differ, it returns false with no memcmp.
    This is a significant win over strcmp() when comparing many strings of different lengths
    (e.g., walking a hash table).
----------------------------------------------------------------------------------------------*/
static void
test_comparison_and_search( void )
{
    str_t a = STR( "RenderPass" );
    str_t b = STR( "renderpass" );
    str_t c = STR( "RenderPass" );

    ORB_ASSERT( !str_equal( a, b ) );              /* case-sensitive: not equal */
    ORB_ASSERT( str_equal_nocase( a, b ) );        /* case-insensitive: equal */
    ORB_ASSERT( str_equal( a, c ) );               /* identical content */

    /* str_cmp for sorting. */
    ORB_ASSERT( str_cmp( STR( "apple" ), STR( "banana" ) ) < 0 );
    ORB_ASSERT( str_cmp( STR( "zebra" ), STR( "ant" ) ) > 0 );
    ORB_ASSERT( str_cmp( STR( "same" ), STR( "same" ) ) == 0 );

    /* Search. */
    str_t sentence = STR( "the quick brown fox" );
    ORB_ASSERT( str_find( sentence, STR( "quick" ) ) == 4 );
    ORB_ASSERT( str_find( sentence, STR( "xyz" ) ) == STR_NOT_FOUND );
    ORB_ASSERT( str_starts_with( sentence, STR( "the" ) ) );
    ORB_ASSERT( str_ends_with( sentence, STR( "fox" ) ) );
    ORB_ASSERT( str_contains( sentence, STR( "brown" ) ) );
    ORB_ASSERT( !str_contains( sentence, STR( "cat" ) ) );

    /* rfind for path extension extraction. */
    str_t asset = STR( "assets/textures/wall.diffuse.png" );
    i32 last_dot = str_rfind_char( asset, '.' );
    ORB_ASSERT( last_dot != STR_NOT_FOUND );
    str_t last_ext = str_sub( asset, last_dot, asset.len );
    ORB_ASSERT( str_equal( last_ext, STR( ".png" ) ) );

    printf( "[PASS] comparison and search\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 4: Hashing -- O(n) over exactly the string bytes

    str_hash32/64 operates over exactly str.len bytes. There is no strlen call, no
    null-terminator dependency, and sub-strings hash correctly without any temporary buffer.

    Key benefit: you can hash a sub-string extracted with str_sub() without copying it.
    With char*, you would have to null-terminate (mutate) or copy before hashing.
----------------------------------------------------------------------------------------------*/
static void
test_hashing( void )
{
    str_t key   = STR( "position" );
    u32   hash1 = str_hash32( key );
    u32   hash2 = str_hash32( STR( "position" ) );
    ORB_ASSERT( hash1 == hash2 );  /* deterministic across calls */

    /* Hashing a sub-view -- no copy needed, unlike with char*. */
    str_t full = STR( "uniforms.position.xyz" );
    i32   dot  = str_find_char( full, '.' );
    str_t sub  = str_sub( full, dot + 1, full.len );    /* "position.xyz" */
    u32   sub_hash = str_hash32( sub );
    ORB_ASSERT( sub_hash != hash1 );  /* "position.xyz" != "position" */

    /* Different strings produce different hashes (no collision for these). */
    ORB_ASSERT( str_hash32( STR( "vertex" ) ) != str_hash32( STR( "fragment" ) ) );

    /* 64-bit hash for larger spaces. */
    u64 h64 = str_hash64( STR( "some_long_asset_name_for_a_shader" ) );
    ORB_ASSERT( h64 != 0 );

    printf( "[PASS] hashing\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 5: Parsing -- str_t -> number

    str_to_i32 / str_to_f64 take str_t directly, so you can parse a sub-string extracted
    from a config file with no temporary copy. Returns b32 success, no global errno.

    Compare to atoi() / strtol():
      atoi("42abc") returns 42 silently, no error indication.
      str_to_i32(STR("42abc"), &v) returns 0 -- explicit failure.
----------------------------------------------------------------------------------------------*/
static void
test_parsing( void )
{
    i32 ival;
    ORB_ASSERT( str_to_i32( STR( "42" ), &ival ) && ival == 42 );
    ORB_ASSERT( str_to_i32( STR( "-17" ), &ival ) && ival == -17 );
    ORB_ASSERT( str_to_i32( STR( "+100" ), &ival ) && ival == 100 );
    ORB_ASSERT( !str_to_i32( STR( "abc" ), &ival ) );   /* non-numeric fails explicitly */
    ORB_ASSERT( !str_to_i32( STR( "" ), &ival ) );      /* empty fails explicitly */

    i64 lval;
    ORB_ASSERT( str_to_i64( STR( "1000000000" ), &lval ) && lval == 1000000000LL );

    f64 fval;
    ORB_ASSERT( str_to_f64( STR( "3.14" ), &fval ) );
    ORB_ASSERT( fval > 3.13 && fval < 3.15 );
    ORB_ASSERT( str_to_f64( STR( "-0.5" ), &fval ) && fval == -0.5 );
    ORB_ASSERT( !str_to_f64( STR( "nan" ), &fval ) );  /* explicit fail */

    f32 f32val;
    ORB_ASSERT( str_to_f32( STR( "1.5" ), &f32val ) && f32val == 1.5f );

    /* Parse a sub-string directly -- no copy needed. */
    str_t config = STR( "width=1920" );
    i32   eq     = str_find_char( config, '=' );
    ORB_ASSERT( eq != STR_NOT_FOUND );
    str_t value_str = str_sub( config, eq + 1, config.len );
    ORB_ASSERT( str_to_i32( value_str, &ival ) && ival == 1920 );

    /* str_scan_i64 / str_scan_u64 -- prefix scan: stops at first non-digit,
       returns bytes consumed. Old str_parse_i64 behaviour. */
    i64 sval;
    ORB_ASSERT( str_scan_i64( STR( "123" ), &sval ) == 3 && sval == 123 );
    ORB_ASSERT( str_scan_i64( STR( "-456rest" ), &sval ) == 4 && sval == -456 );
    ORB_ASSERT( str_scan_i64( STR( "+7xyz" ), &sval ) == 2 && sval == 7 );
    ORB_ASSERT( str_scan_i64( STR( "99x" ), &sval ) == 2 && sval == 99 );   /* stops at 'x' */
    ORB_ASSERT( str_scan_i64( STR( "abc" ), &sval ) == 0 );                 /* no digits */
    ORB_ASSERT( str_scan_i64( STR_EMPTY, &sval ) == 0 );

    u64 uval;
    ORB_ASSERT( str_scan_u64( STR( "789end" ), &uval ) == 3 && uval == 789 );
    ORB_ASSERT( str_scan_u64( STR( "0" ), &uval ) == 1 && uval == 0 );
    ORB_ASSERT( str_scan_u64( STR( "abc" ), &uval ) == 0 );

    printf( "[PASS] parsing\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 6: strbuf_t -- building strings safely

    The strbuf_decl macro declares a stack-backed buffer in one line. All append functions
    are safe: they detect overflow and set a flag instead of writing past the end.

    The buffer is ALWAYS null-terminated after every operation. ptr can be passed to fopen(),
    printf(), any C API, at any point without additional preparation.
----------------------------------------------------------------------------------------------*/
static void
test_strbuf_basic( void )
{
    /* Declare a 256-byte stack buffer in one line. */
    strbuf_decl( path, 256 );
    ORB_ASSERT( strbuf_ok( path ) );
    ORB_ASSERT( path.len == 0 );
    ORB_ASSERT( path.ptr[ 0 ] == '\0' );  /* already null-terminated */

    /* Build a file path by appending pieces. */
    strbuf_append( &path, STR( "assets/" ) );
    strbuf_append( &path, STR( "textures/" ) );
    strbuf_append( &path, STR( "wall.png" ) );
    ORB_ASSERT( strbuf_ok( path ) );
    ORB_ASSERT( str_equal( strbuf_str( path ), STR( "assets/textures/wall.png" ) ) );
    ORB_ASSERT( path.ptr[ path.len ] == '\0' );  /* always null-terminated */

    /* Append a char. */
    strbuf_clear( &path );
    strbuf_append_char( &path, 'A' );
    strbuf_append_char( &path, 'B' );
    ORB_ASSERT( str_equal( strbuf_str( path ), STR( "AB" ) ) );

    /* Append a C string. */
    strbuf_clear( &path );
    strbuf_append_cstr( &path, "hello" );
    ORB_ASSERT( path.len == 5 );

    /* strbuf_str is a zero-cost view -- same pointer, same length. */
    str_t view = strbuf_str( path );
    ORB_ASSERT( view.ptr == path.ptr );
    ORB_ASSERT( view.len == path.len );

    /* STRBUF() macro wraps a pre-existing char array -- capacity derived at compile time. */
    char      storage[ 32 ] = { 0 };
    strbuf_t  wrapped = STRBUF( storage );
    ORB_ASSERT( strbuf_ok( wrapped ) );
    ORB_ASSERT( strbuf_cap( wrapped ) == 32 );
    strbuf_append( &wrapped, STR( "wrap" ) );
    ORB_ASSERT( str_equal( strbuf_str( wrapped ), STR( "wrap" ) ) );

    /* strbuf_from_ptr_cap when capacity is a runtime value. */
    char      dynbuf[ 16 ] = { 0 };
    i32       dynlen = 16;
    strbuf_t  dyn = strbuf_from_ptr_cap( dynbuf, dynlen );
    ORB_ASSERT( strbuf_cap( dyn ) == 16 );
    strbuf_append_cstr( &dyn, "dynamic" );
    ORB_ASSERT( str_equal( strbuf_str( dyn ), STR( "dynamic" ) ) );

    printf( "[PASS] strbuf basic\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 7: strbuf_t printf-style formatting

    strbuf_appendf appends formatted text into whatever space remains.
    strbuf_fmt clears the buffer first, then formats -- the cleanest way to re-use a buffer.

    Compare to snprintf():
        char buf[64];
        snprintf(buf, sizeof(buf), "pos: %d", x);   -- no overflow tracking, silently truncates

        strbuf_decl(buf, 64);
        strbuf_fmt(&buf, "pos: %d", x);
        if (!strbuf_ok(buf)) handle_overflow();     -- explicit, recoverable
----------------------------------------------------------------------------------------------*/
static void
test_strbuf_format( void )
{
    strbuf_decl( msg, 64 );

    /* Format from scratch. */
    strbuf_fmt( &msg, "entity_%04d", 42 );
    ORB_ASSERT( strbuf_ok( msg ) );
    ORB_ASSERT( str_equal( strbuf_str( msg ), STR( "entity_0042" ) ) );

    /* Append formatted text to existing content. */
    strbuf_clear( &msg );
    strbuf_appendf( &msg, "x=%d", 10 );
    strbuf_appendf( &msg, " y=%d", 20 );
    ORB_ASSERT( str_equal( strbuf_str( msg ), STR( "x=10 y=20" ) ) );

    /* strbuf_fmt resets even an overflowed buffer. */
    strbuf_decl( small, 8 );
    strbuf_fmt( &small, "this string is definitely too long to fit" );
    ORB_ASSERT( strbuf_overflowed( small ) );      /* detected */
    ORB_ASSERT( small.ptr[ small.len ] == '\0' );  /* still null-terminated */

    strbuf_fmt( &small, "ok" );                    /* clears overflow and re-formats */
    ORB_ASSERT( strbuf_ok( small ) );
    ORB_ASSERT( str_equal( strbuf_str( small ), STR( "ok" ) ) );

    /* Number formatting helpers. */
    char tmp[ 32 ];
    strbuf_t s64 = strbuf_from_i64( -123456789LL, tmp, sizeof( tmp ) );
    ORB_ASSERT( strbuf_ok( s64 ) );
    ORB_ASSERT( str_equal( strbuf_str( s64 ), STR( "-123456789" ) ) );

    char ftmp[ 32 ];
    strbuf_t sf = strbuf_from_f64( 3.14159, 2, ftmp, sizeof( ftmp ) );
    ORB_ASSERT( strbuf_ok( sf ) );
    ORB_ASSERT( str_equal( strbuf_str( sf ), STR( "3.14" ) ) );

    /* strbuf_from_u64 -- unsigned decimal (covers old fmt_u64 tests). */
    char utmp[ 32 ];
    strbuf_t su0 = strbuf_from_u64( 0u, utmp, sizeof( utmp ) );
    ORB_ASSERT( strbuf_ok( su0 ) && str_equal( strbuf_str( su0 ), STR( "0" ) ) );

    strbuf_t su1 = strbuf_from_u64( 999u, utmp, sizeof( utmp ) );
    ORB_ASSERT( strbuf_ok( su1 ) && str_equal( strbuf_str( su1 ), STR( "999" ) ) );

    strbuf_t su2 = strbuf_from_u64( 4294967295u, utmp, sizeof( utmp ) );  /* UINT32_MAX */
    ORB_ASSERT( strbuf_ok( su2 ) && str_equal( strbuf_str( su2 ), STR( "4294967295" ) ) );

    /* strbuf_from_hex64 -- lowercase hex, no "0x" prefix (covers old fmt_hex64 tests). */
    char htmp[ 32 ];
    strbuf_t sh0 = strbuf_from_hex64( 0u, htmp, sizeof( htmp ) );
    ORB_ASSERT( strbuf_ok( sh0 ) && str_equal( strbuf_str( sh0 ), STR( "0" ) ) );

    strbuf_t sh1 = strbuf_from_hex64( 0xABCu, htmp, sizeof( htmp ) );
    ORB_ASSERT( strbuf_ok( sh1 ) && str_equal( strbuf_str( sh1 ), STR( "abc" ) ) );

    strbuf_t sh2 = strbuf_from_hex64( 0xDEADBEEFu, htmp, sizeof( htmp ) );
    ORB_ASSERT( strbuf_ok( sh2 ) && str_equal( strbuf_str( sh2 ), STR( "deadbeef" ) ) );

    printf( "[PASS] strbuf format\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 8: Overflow detection

    Unlike snprintf() which silently truncates, strbuf records overflow and all subsequent
    writes become no-ops. You can check the state once at the end of a build sequence.

    The buffer is ALWAYS null-terminated in the overflowed state -- the partial string up to
    the available space is preserved and safe to use as a diagnostic message.
----------------------------------------------------------------------------------------------*/
static void
test_overflow_detection( void )
{
    strbuf_decl( buf, 10 );   /* only 9 chars + null */

    /* Fill exactly to capacity -- should succeed. */
    strbuf_append( &buf, STR( "123456789" ) );  /* 9 chars */
    ORB_ASSERT( strbuf_ok( buf ) );
    ORB_ASSERT( buf.len == 9 );
    ORB_ASSERT( buf.ptr[ 9 ] == '\0' );

    /* One more char -- should overflow. */
    b32 ok = strbuf_append_char( &buf, 'X' );
    ORB_ASSERT( !ok );
    ORB_ASSERT( strbuf_overflowed( buf ) );
    ORB_ASSERT( buf.ptr[ buf.len ] == '\0' );  /* still null-terminated */

    /* All subsequent writes are no-ops in overflow state. */
    ok = strbuf_append( &buf, STR( "more" ) );
    ORB_ASSERT( !ok );
    ORB_ASSERT( buf.len == 9 );  /* unchanged */

    /* strbuf_zero resets everything including the overflow flag. */
    strbuf_zero( &buf );
    ORB_ASSERT( strbuf_ok( buf ) );
    ORB_ASSERT( buf.len == 0 );

    /* WRAP usage: build a path, check once at end. */
    strbuf_decl( path, 64 );
    strbuf_append( &path, STR( "shaders/" ) );
    strbuf_append( &path, STR( "basic" ) );
    strbuf_append( &path, STR( ".hlsl" ) );
    if ( strbuf_ok( path ) )
    {
        /* path.ptr is safe to pass to fopen, CreateFile, etc. */
        ORB_ASSERT( str_equal( strbuf_str( path ), STR( "shaders/basic.hlsl" ) ) );
    }

    printf( "[PASS] overflow detection\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 9: Editing operations

    strbuf_insert, strbuf_remove, strbuf_chop, strbuf_trim, strbuf_strip_trailing.
    These modify the buffer in-place. All maintain the null-terminator invariant.
----------------------------------------------------------------------------------------------*/
static void
test_editing( void )
{
    strbuf_decl( buf, 64 );

    /* Insert: inject text at a given position. */
    strbuf_set( &buf, STR( "hello world" ) );
    strbuf_insert( &buf, 5, STR( " beautiful" ) );
    ORB_ASSERT( str_equal( strbuf_str( buf ), STR( "hello beautiful world" ) ) );

    /* Remove: cut a range of bytes. */
    strbuf_remove( &buf, 5, 10 );  /* remove " beautiful" */
    ORB_ASSERT( str_equal( strbuf_str( buf ), STR( "hello world" ) ) );

    /* Chop: truncate to a given length. */
    strbuf_set( &buf, STR( "abcdefgh" ) );
    strbuf_chop( &buf, 4 );
    ORB_ASSERT( str_equal( strbuf_str( buf ), STR( "abcd" ) ) );
    ORB_ASSERT( buf.ptr[ 4 ] == '\0' );  /* null-terminated after chop */

    /* Trim: remove last n characters. */
    strbuf_set( &buf, STR( "hello.png" ) );
    strbuf_trim( &buf, 4 );  /* remove ".png" */
    ORB_ASSERT( str_equal( strbuf_str( buf ), STR( "hello" ) ) );

    /* Strip trailing: remove all occurrences of a specific char from the end. */
    strbuf_set( &buf, STR( "path/to/dir///" ) );
    strbuf_strip_trailing( &buf, '/' );
    ORB_ASSERT( str_equal( strbuf_str( buf ), STR( "path/to/dir" ) ) );

    /* strbuf_set_cstr -- overwrite from a null-terminated C string. */
    strbuf_set_cstr( &buf, "from_cstr" );
    ORB_ASSERT( str_equal( strbuf_str( buf ), STR( "from_cstr" ) ) );
    ORB_ASSERT( buf.ptr[ buf.len ] == '\0' );

    printf( "[PASS] editing\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 10: C string interop

    str_to_cstr copies a str_t into a null-terminated buffer for C APIs.
    strbuf_t.ptr can always be passed directly without any extra step.
----------------------------------------------------------------------------------------------*/
static void
test_cstr_interop( void )
{
    /* str_t -> char* for C API call. */
    str_t name   = STR( "render_pipeline" );
    char  tmp[ 64 ];
    i32   written = str_to_cstr( name, tmp, sizeof( tmp ) );
    ORB_ASSERT( written == name.len );
    ORB_ASSERT( tmp[ written ] == '\0' );

    /* Truncation: if buf is too small, it still null-terminates. */
    char small[ 4 ];
    str_to_cstr( STR( "hello" ), small, sizeof( small ) );
    ORB_ASSERT( small[ 3 ] == '\0' );  /* always null-terminated */

    /* strbuf_t.ptr is directly usable -- no conversion needed. */
    strbuf_decl( path, 64 );
    strbuf_fmt( &path, "%s/%s", "textures", "wall.png" );
    ORB_ASSERT( strbuf_ok( path ) );
    /* path.ptr == "textures/wall.png\0" -- safe to pass to fopen() right here. */
    ORB_ASSERT( path.ptr[ path.len ] == '\0' );

    /* Heap-backed buffer (allocated variant). */
    strbuf_t heap = strbuf_alloc( 128 );
    ORB_ASSERT( strbuf_ok( heap ) );
    strbuf_fmt( &heap, "heap string %d", 99 );
    ORB_ASSERT( str_equal( strbuf_str( heap ), STR( "heap string 99" ) ) );
    strbuf_free( &heap );
    ORB_ASSERT( heap.ptr == NULL );  /* zeroed after free */

    printf( "[PASS] C string interop\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 11: The composition pattern

    The combination of str_t (view) + strbuf_t (builder) enables a clean, consistent API:
      - Functions that READ strings accept str_t (by value, cheap).
      - Functions that BUILD strings accept strbuf_t* (by pointer, writable).
      - Conversion between them is zero-cost and explicit.

    This mirrors the Rust &str / String split, or C++ string_view / string,
    but with zero runtime overhead from virtual dispatch or allocator abstraction.
----------------------------------------------------------------------------------------------*/
static void
test_composition( void )
{
    /* Build a shader key from components, then use it as a read-only view. */
    strbuf_decl( key, 128 );
    strbuf_fmt( &key, "%s_%s_%d", "vs", "basic", 2 );

    str_t key_view = strbuf_str( key );           /* zero-cost view */
    ORB_ASSERT( str_equal( key_view, STR( "vs_basic_2" ) ) );
    ORB_ASSERT( str_hash32( key_view ) == str_hash32( STR( "vs_basic_2" ) ) );

    /* Check a prefix without copying. */
    ORB_ASSERT( str_starts_with( key_view, STR( "vs_" ) ) );

    /* Extract a component by search -- no allocation. */
    i32   sep   = str_find_char( key_view, '_' );
    str_t stage = str_prefix( key_view, sep );
    ORB_ASSERT( str_equal( stage, STR( "vs" ) ) );

    printf( "[PASS] composition\n" );
}

/*----------------------------------------------------------------------------------------------
    SECTION 12: str_arena_t -- scope-lifetime string building

    str_arena_t manages many temporary strings that all share a single flat buffer and
    are freed together at scope exit. Marks + pops provide stack-like lifetime semantics
    with no per-string tracking.

    The key benefit over strbuf_t: three strings in one scope = one arena, not three
    separate fixed buffers with three independent overflow checks.
----------------------------------------------------------------------------------------------*/
static void
test_str_arena( void )
{
    /* Declare a self-contained stack-backed arena in one line. */
    str_arena_decl( scratch, 512 );
    ORB_ASSERT( str_arena_remaining( scratch ) == 512 );

    /* Push a formatted string -- null-terminated, safe as char* directly. */
    str_t a = str_arena_push_fmt( &scratch, "entity_%04d", 7 );
    ORB_ASSERT( str_equal( a, STR( "entity_0007" ) ) );

    /* Push a str_t view -- copies bytes into arena, no original pointer kept. */
    str_t b = str_arena_push_str( &scratch, STR( "render_pass" ) );
    ORB_ASSERT( str_equal( b, STR( "render_pass" ) ) );

    /* Push from a null-terminated C string. */
    str_t c = str_arena_push_cstr( &scratch, "hello" );
    ORB_ASSERT( str_equal( c, STR( "hello" ) ) );

    /* Multiple strings coexist -- arena cursor just advanced each time. */
    ORB_ASSERT( a.ptr != b.ptr );
    ORB_ASSERT( b.ptr != c.ptr );

    /* Marks allow releasing a group of allocations at once. */
    i32 mark = str_arena_mark( &scratch );
    i32 used_before = scratch.pos;

    str_t tmp1 = str_arena_push_fmt( &scratch, "tmp_%d", 1 );
    str_t tmp2 = str_arena_push_fmt( &scratch, "tmp_%d", 2 );
    ORB_ASSERT( str_equal( tmp1, STR( "tmp_1" ) ) );
    ORB_ASSERT( str_equal( tmp2, STR( "tmp_2" ) ) );
    ORB_ASSERT( scratch.pos > used_before );

    str_arena_pop( &scratch, mark );         /* release tmp1 and tmp2 together */
    ORB_ASSERT( scratch.pos == used_before );/* cursor back to where it was */

    /* str_arena_clear resets the entire arena. */
    str_arena_clear( &scratch );
    ORB_ASSERT( scratch.pos == 0 );

    /* strbuf integration: allocate a writable strbuf from the arena, build incrementally,
       then trim the unused reservation so cursor sits just past the written content. */
    strbuf_t ab = str_arena_strbuf( &scratch, 128 );
    ORB_ASSERT( strbuf_ok( ab ) );
    strbuf_appendf( &ab, "cmd: %s --n %d", "compile", 4 );
    ORB_ASSERT( strbuf_ok( ab ) );
    str_arena_trim_strbuf( &scratch, &ab );   /* release unused tail */
    str_t result = strbuf_str( ab );
    ORB_ASSERT( str_equal( result, STR( "cmd: compile --n 4" ) ) );

    /* Out-of-space: push_fmt returns STR_EMPTY when the arena is full. */
    str_arena_decl( tiny, 8 );
    str_t fail = str_arena_push_fmt( &tiny, "this is definitely too long" );
    ORB_ASSERT( str_is_empty( fail ) );
    ORB_ASSERT( tiny.pos == 0 );  /* cursor not advanced on failure */

    /* STR_ARENA() compound literal and str_arena_from_ptr_cap work symmetrically. */
    char        storage[ 64 ] = { 0 };
    str_arena_t lit = STR_ARENA( storage );
    ORB_ASSERT( lit.cap == 64 );
    str_t lit_s = str_arena_push_cstr( &lit, "arena_literal" );
    ORB_ASSERT( str_equal( lit_s, STR( "arena_literal" ) ) );

    str_arena_t dyn = str_arena_from_ptr_cap( storage, 64 );
    ORB_ASSERT( dyn.cap == 64 );

    printf( "[PASS] str_arena\n" );
}

/*==============================================================================================
    Entry Point
==============================================================================================*/

void
test_str_new( void )
{
    printf( "\n=== str_t / strbuf_t tests ===\n" );

    test_literal_construction();
    test_substrings_and_trim();
    test_comparison_and_search();
    test_hashing();
    test_parsing();
    test_strbuf_basic();
    test_strbuf_format();
    test_overflow_detection();
    test_editing();
    test_cstr_interop();
    test_composition();
    test_str_arena();

    printf( "=== all tests passed ===\n\n" );
}

/*============================================================================================*/
// clang-format off