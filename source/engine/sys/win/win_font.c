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
        const char* file = (const char*)data;
        if ( font_has_sep( file ) || ( file[ 0 ] && file[ 1 ] == ':' ) )
            snprintf( out, (size_t)out_size, "%s", file );
        else
            snprintf( out, (size_t)out_size, "%s\\%s", fonts_dir, file );

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
    UINT n = GetWindowsDirectoryA( fonts_dir, sizeof( fonts_dir ) );
    if ( n == 0 || n >= sizeof( fonts_dir ) - 7 )
        snprintf( fonts_dir, sizeof( fonts_dir ), "C:\\Windows\\Fonts" );
    else
        snprintf( fonts_dir + n, sizeof( fonts_dir ) - n, "\\Fonts" );

    if ( font_registry_hive( HKEY_LOCAL_MACHINE, want, fonts_dir, out, out_size ) ) return true;
    if ( font_registry_hive( HKEY_CURRENT_USER,  want, fonts_dir, out, out_size ) ) return true;
    return false;
}

/*============================================================================================*/
