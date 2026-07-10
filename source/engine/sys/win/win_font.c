/*==============================================================================================

    win/win_font.c -- OS-installed font lookup (Windows).

    Windows installs fonts under a bare filename (CascadiaMono.ttf) but users name them by their
    friendly name ("Cascadia Mono").  The mapping lives in the registry:

        HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts   system-wide (bare filenames)
        HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts   per-user    (absolute paths)

    Each value is "<Friendly Name> (TrueType)" = "<file>".  sys_font_resolve_name() matches the
    requested name against the friendly name and resolves the file to an absolute path.

==============================================================================================*/

/*============================================================================================*/

/* Lowercase, alphanumeric-only copy of `s`.  Lets a requested name match a registry entry
   regardless of case, spaces, or punctuation ("Cascadia Mono" -> "cascadiamono"). */

static void
font_normalize( const char* s, char* out, int out_size )
{
    int len = 0;
    for ( const char* p = s; *p && len < out_size - 1; ++p )
    {
        char c = *p;
        if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
        if ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) )
            out[ len++ ] = c;
    }
    out[ len ] = '\0';
}

static bool
font_has_sep( const char* p )
{
    for ( ; *p; ++p )
        if ( *p == '\\' || *p == '/' ) return true;
    return false;
}

/* Resolve a registry font value to an absolute path: a bare filename (system fonts) is joined
   under `fonts_dir`; an absolute path (per-user "X:\...") is copied as-is. */

static void
font_value_path( const char* file, const char* fonts_dir, char* out, int out_size )
{
    if ( font_has_sep( file ) || ( file[ 0 ] && file[ 1 ] == ':' ) )
        snprintf( out, (size_t)out_size, "%s", file );
    else
        snprintf( out, (size_t)out_size, "%s\\%s", fonts_dir, file );
}

/* True for face formats the bakers (stb_truetype / FreeType) can rasterize.  Excludes bitmap
   .fon and other legacy registry entries. */

static bool
font_ext_bakeable( const char* path )
{
    const char* dot = strrchr( path, '.' );
    if ( !dot ) return false;
    return _stricmp( dot, ".ttf" ) == 0 || _stricmp( dot, ".otf" ) == 0
        || _stricmp( dot, ".ttc" ) == 0;
}

/* Absolute path of the system fonts directory (%WINDIR%\Fonts, with a hard fallback). */

static void
font_system_dir( char* out, int out_size )
{
    UINT n = GetWindowsDirectoryA( out, (UINT)out_size );
    if ( n == 0 || n >= (UINT)out_size - 7 )
        snprintf( out, (size_t)out_size, "C:\\Windows\\Fonts" );
    else
        snprintf( out + n, (size_t)out_size - n, "\\Fonts" );
}

/* Search one registry hive's font map for a friendly-name match.  A bare filename is resolved
   under `fonts_dir`; an absolute path (per-user fonts) is used as-is.  Returns true once a match
   resolves to a file that exists on disk. */

static bool
font_registry_hive( HKEY root, const char* want_norm, const char* fonts_dir,
                    char* out, int out_size )
{
    HKEY key;
    if ( RegOpenKeyExA( root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                        0, KEY_READ, &key ) != ERROR_SUCCESS )
        return false;

    bool found = false;
    char name[ 256 ];
    BYTE data[ MAX_PATH ];

    for ( DWORD i = 0; !found; ++i )
    {
        DWORD name_len = sizeof( name );
        DWORD data_len = sizeof( data );
        DWORD type     = 0;
        LONG  rc = RegEnumValueA( key, i, name, &name_len, NULL, &type, data, &data_len );
        if ( rc == ERROR_NO_MORE_ITEMS ) break;
        if ( rc != ERROR_SUCCESS || type != REG_SZ ) continue;

        /* Drop the trailing " (TrueType)" / " (OpenType)" decoration from the friendly name. */
        char* paren = strrchr( name, '(' );
        if ( paren && paren != name ) *paren = '\0';

        char nm[ 128 ];
        font_normalize( name, nm, sizeof( nm ) );

        bool hit = ( strcmp( nm, want_norm ) == 0 );
        if ( !hit )   /* "Cascadia Mono" should also match the registry's "Cascadia Mono Regular" */
        {
            size_t wl = strlen( want_norm );
            hit = ( strncmp( nm, want_norm, wl ) == 0 && strcmp( nm + wl, "regular" ) == 0 );
        }
        if ( !hit ) continue;

        /* data is a bare filename (system fonts) or an absolute path (per-user, "X:\..."). */
        font_value_path( (const char*)data, fonts_dir, out, out_size );
        found = ( sys_file_time( out ) > 0 );
    }

    RegCloseKey( key );
    return found;
}

bool
sys_font_resolve_name( const char* name, char* out, int out_size )
{
    if ( !name || !*name || !out || out_size < 2 )
        return false;

    char want[ 128 ];
    font_normalize( name, want, sizeof( want ) );
    if ( !want[ 0 ] )
        return false;

    /* System fonts store bare filenames relative to %WINDIR%\Fonts. */
    char fonts_dir[ MAX_PATH ];
    font_system_dir( fonts_dir, sizeof( fonts_dir ) );

    if ( font_registry_hive( HKEY_LOCAL_MACHINE, want, fonts_dir, out, out_size ) ) return true;
    if ( font_registry_hive( HKEY_CURRENT_USER,  want, fonts_dir, out, out_size ) ) return true;
    return false;
}

/*==============================================================================================
    Enumeration -- list every installed face by friendly name.
==============================================================================================*/

/* Walk one hive's font map, delivering each bakeable face to `cb`.  Sets *stop when the callback
   asks to end early.  Returns the number of faces delivered from this hive. */

static int
font_enum_hive( HKEY root, const char* fonts_dir, sys_glob_fn cb, void* ud, bool* stop )
{
    HKEY key;
    if ( RegOpenKeyExA( root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                        0, KEY_READ, &key ) != ERROR_SUCCESS )
        return 0;

    int  count = 0;
    char name[ 256 ];
    BYTE data[ MAX_PATH ];

    for ( DWORD i = 0; ; ++i )
    {
        DWORD name_len = sizeof( name );
        DWORD data_len = sizeof( data );
        DWORD type     = 0;
        LONG  rc = RegEnumValueA( key, i, name, &name_len, NULL, &type, data, &data_len );
        if ( rc == ERROR_NO_MORE_ITEMS ) break;
        if ( rc != ERROR_SUCCESS || type != REG_SZ ) continue;

        /* Drop the trailing " (TrueType)" / " (OpenType)" decoration and any space before it. */
        char* paren = strrchr( name, '(' );
        if ( paren && paren != name ) *paren = '\0';
        for ( int e = (int)strlen( name ) - 1; e >= 0 && ( name[ e ] == ' ' || name[ e ] == '\t' ); --e )
            name[ e ] = '\0';
        if ( !name[ 0 ] ) continue;

        char path[ MAX_PATH ];
        font_value_path( (const char*)data, fonts_dir, path, sizeof( path ) );
        if ( !font_ext_bakeable( path ) ) continue;
        if ( sys_file_time( path ) == 0 ) continue;

        ++count;
        if ( cb && !cb( name, path, ud ) ) { *stop = true; break; }
    }

    RegCloseKey( key );
    return count;
}

int
sys_font_enumerate( sys_glob_fn cb, void* userdata )
{
    char fonts_dir[ MAX_PATH ];
    font_system_dir( fonts_dir, sizeof( fonts_dir ) );

    bool stop  = false;
    int  total = font_enum_hive( HKEY_LOCAL_MACHINE, fonts_dir, cb, userdata, &stop );
    if ( !stop )
        total += font_enum_hive( HKEY_CURRENT_USER, fonts_dir, cb, userdata, &stop );
    return total;
}

/*============================================================================================*/
