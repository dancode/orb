/*==============================================================================================

    build_tool_gen.c -- Generation of Visual Studio project files.

==============================================================================================*/

#define MAX_FILES 1024
#define MAX_FILTERS 512

typedef struct
{
    char path[ 256 ];
    char filter[ 256 ];
    bool is_header;
} file_info_t;

static file_info_t g_files[ MAX_FILES ];
static int         g_file_count = 0;

static char g_filters[ MAX_FILTERS ][ 256 ];
static int  g_filter_count = 0;

// GUIDs for static projects
static const char* g_guid_engine = "{DE231EAC-9C33-B4FA-8440-E3A81E12CA86}";

static void
add_filter( const char* filter )
{
    if ( filter[ 0 ] == '\0' ) return;
    for ( int i = 0; i < g_filter_count; ++i )
    {
        if ( _stricmp( g_filters[ i ], filter ) == 0 ) return;
    }
    if ( g_filter_count < MAX_FILTERS )
    {
        strcpy( g_filters[ g_filter_count++ ], filter );
    }
}

static void
add_filters_recursive( const char* filter )
{
    char  tmp[ 256 ];
    char* p = tmp;
    strcpy( tmp, filter );
    while ( *p )
    {
        if ( *p == '\\' )
        {
            *p = '\0';
            add_filter( tmp );
            *p = '\\';
        }
        p++;
    }
    add_filter( tmp );
}

static void
get_filter_for_path( const char* path, char* out_filter )
{
    out_filter[ 0 ] = '\0';
    if ( strncmp( path, "source/", 7 ) == 0 )
    {
        const char* sub        = path + 7;
        const char* last_slash = strrchr( sub, '/' );
        if ( last_slash )
        {
            size_t len = last_slash - sub;
            strncpy( out_filter, sub, len );
            out_filter[ len ] = '\0';
            for ( char* p = out_filter; *p; ++p )
                if ( *p == '/' ) *p = '\\';
        }
    }
}

static void
scan_directory_recursive( const char* dir )
{
    char search_path[ 512 ];
    sprintf( search_path, "%s/*", dir );

    struct _finddata_t find_data;
    intptr_t           handle = _findfirst( search_path, &find_data );

    if ( handle == -1 ) return;

    do
    {
        if ( strcmp( find_data.name, "." ) == 0 || strcmp( find_data.name, ".." ) == 0 ) continue;

        char path[ 512 ];
        sprintf( path, "%s/%s", dir, find_data.name );

        if ( find_data.attrib & _A_SUBDIR )
        {
            scan_directory_recursive( path );
        }
        else
        {
            const char* ext = strrchr( find_data.name, '.' );
            if ( ext )
            {
                bool is_c = _stricmp( ext, ".c" ) == 0;
                bool is_h = _stricmp( ext, ".h" ) == 0;

                if ( ( is_c || is_h ) && g_file_count < MAX_FILES )
                {
                    file_info_t* f = &g_files[ g_file_count++ ];
                    strcpy( f->path, path );
                    f->is_header = is_h;
                    get_filter_for_path( path, f->filter );
                    if ( f->filter[ 0 ] != '\0' ) add_filters_recursive( f->filter );
                }
            }
        }
    }
    while ( _findnext( handle, &find_data ) == 0 );

    _findclose( handle );
}

static void
write_vcxproj_common_header( FILE* f, const char* guid, const char* out_name, target_type_t type )
{
    const char* ext = ".exe";
    if ( type == TARGET_STATIC_LIB ) ext = ".lib";
    if ( type == TARGET_DYNAMIC_LIB ) ext = ".dll";

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );
    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", guid );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    fprintf( f, "    <PlatformToolset>v143</PlatformToolset>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)..\\bin\\</OutDir>\n" );
    fprintf( f, "    <IntDir>$(ProjectDir)%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n", g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -config $(Configuration) -target %s</NMakeBuildCommandLine>\n", out_name );
    fprintf( f, "    <NMakeOutput>..\\bin\\%s%s</NMakeOutput>\n", out_name, ext );
    fprintf( f, "    <NMakeCleanCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -clean</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)..\\source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
}

static void
build_gen_proj_target( target_info_t* target, int index )
{
    char vcxproj_path[ 256 ];
    sprintf( vcxproj_path, "%s/%s.vcxproj", g_build_dir, target->name );

    char guid[ 64 ];
    sprintf( guid, "{DE231EAC-9C33-B4FA-8440-E3A81E12%04X}", 0xB000 + index );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f ) return;

    write_vcxproj_common_header( f, guid, target->name, target->type );

    char search_path[ 512 ];
    sprintf( search_path, "%s/*", target->root_dir );

    struct _finddata_t find_data;
    intptr_t           handle = _findfirst( search_path, &find_data );

    if ( handle != -1 )
    {
        fprintf( f, "  <ItemGroup>\n" );
        do
        {
            if ( find_data.attrib & _A_SUBDIR ) continue;

            const char* ext = strrchr( find_data.name, '.' );
            if ( !ext ) continue;

            bool is_c = _stricmp( ext, ".c" ) == 0;
            bool is_h = _stricmp( ext, ".h" ) == 0;

            if ( is_c || is_h )
            {
                bool is_unit = false;
                for ( int i = 0; i < target->unit_count; ++i )
                {
                    if ( _stricmp( find_data.name, target->units[ i ] ) == 0 )
                    {
                        is_unit = true;
                        break;
                    }
                }

                if ( !is_unit )
                {
                    fprintf( f, "    <ClInclude Include=\"..\\%s/%s\" />\n", target->root_dir, find_data.name );
                }
            }
        }
        while ( _findnext( handle, &find_data ) == 0 );
        _findclose( handle );
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < target->unit_count; ++i )
        {
            fprintf( f, "    <ClCompile Include=\"..\\%s/%s\" />\n", target->root_dir, target->units[ i ] );
        }
        fprintf( f, "  </ItemGroup>\n" );
    }

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );
}

static void
build_gen_proj_engine_navigation( void )
{
    g_file_count   = 0;
    g_filter_count = 0;
    scan_directory_recursive( "source" );

    char vcxproj_path[ 256 ];
    sprintf( vcxproj_path, "%s/%s.vcxproj", g_build_dir, g_proj_name );
    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f ) return;

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );
    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", g_guid_engine );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    fprintf( f, "    <PlatformToolset>v143</PlatformToolset>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)..\\bin\\</OutDir>\n" );
    fprintf( f, "    <IntDir>$(ProjectDir)%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n", g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -config $(Configuration)</NMakeBuildCommandLine>\n" );
    fprintf( f, "    <NMakeOutput>..\\bin\\%s.exe</NMakeOutput>\n", g_out_name );
    fprintf( f, "    <NMakeCleanCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -clean</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)..\\source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        const char* tag = g_files[ i ].is_header ? "ClInclude" : "ClCompile";
        fprintf( f, "    <%s Include=\"..\\%s\" />\n", tag, g_files[ i ].path );
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    char filters_path[ 256 ];
    sprintf( filters_path, "%s/%s.vcxproj.filters", g_build_dir, g_proj_name );
    f = fopen( filters_path, "w" );
    if ( f )
    {
        fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
        fprintf( f, "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_filter_count; ++i )
        {
            fprintf( f, "    <Filter Include=\"%s\">\n", g_filters[ i ] );
            fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n", (unsigned int)i );
            fprintf( f, "    </Filter>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            const char* tag = g_files[ i ].is_header ? "ClInclude" : "ClCompile";
            fprintf( f, "    <%s Include=\"..\\%s\">\n", tag, g_files[ i ].path );
            if ( g_files[ i ].filter[ 0 ] != '\0' ) fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
            fprintf( f, "    </%s>\n", tag );
        }
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }
}

void
build_gen_projects( void )
{
    printf( "Generating Visual Studio projects in %s/...\n", g_build_dir );

#if defined( _WIN32 )
    if ( _access( g_build_dir, 0 ) != 0 )
    {
        char cmd[ 256 ];
        sprintf( cmd, "mkdir %s", g_build_dir );
        system( cmd );
    }
#else
    char cmd[ 256 ];
    sprintf( cmd, "mkdir -p %s", g_build_dir );
    system( cmd );
#endif

    for ( int i = 0; i < g_target_count; ++i )
    {
        build_gen_proj_target( &g_targets[ i ], i );
    }

    build_gen_proj_engine_navigation();

    char sln_path[ 256 ];
    sprintf( sln_path, "%s/%s.sln", g_build_dir, g_proj_name );
    FILE* f = fopen( sln_path, "w" );
    if ( f )
    {
        fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
        fprintf( f, "# Visual Studio Version 17\n" );

        const char* folder_type_guid = "{2150E333-8FDC-42A3-9474-1A3956D46DE8}";
        const char* cpp_type_guid    = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

        fprintf( f, "Project(\"%s\") = \"%s\", \"%s.vcxproj\", \"%s\"\n", cpp_type_guid, g_proj_name, g_proj_name, g_guid_engine );
        fprintf( f, "EndProject\n" );

        char  folders[ 16 ][ 64 ];
        char  folder_guids[ 16 ][ 64 ];
        int   folder_count = 0;

        for ( int i = 0; i < g_target_count; ++i )
        {
            bool found = false;
            for ( int j = 0; j < folder_count; ++j )
            {
                if ( strcmp( folders[ j ], g_targets[ i ].sln_folder ) == 0 )
                {
                    found = true;
                    break;
                }
            }
            if ( !found && folder_count < 16 )
            {
                strcpy( folders[ folder_count ], g_targets[ i ].sln_folder );
                sprintf( folder_guids[ folder_count ], "{DE231EAC-9C33-B4FA-8440-E3A81E12%04X}", 0xF000 + folder_count );
                folder_count++;
            }
        }

        for ( int i = 0; i < folder_count; ++i )
        {
            fprintf( f, "Project(\"%s\") = \"%s\", \"%s\", \"%s\"\n", folder_type_guid, folders[ i ], folders[ i ], folder_guids[ i ] );
            fprintf( f, "EndProject\n" );
        }

        for ( int i = 0; i < g_target_count; ++i )
        {
            char guid[ 64 ];
            sprintf( guid, "{DE231EAC-9C33-B4FA-8440-E3A81E12%04X}", 0xB000 + i );
            fprintf( f, "Project(\"%s\") = \"%s\", \"%s.vcxproj\", \"%s\"\n", cpp_type_guid, g_targets[ i ].name, g_targets[ i ].name, guid );
            fprintf( f, "EndProject\n" );
        }

        fprintf( f, "Global\n" );
        fprintf( f, "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n" );
        fprintf( f, "\t\tDebug|x64 = Debug|x64\n" );
        fprintf( f, "\t\tRelease|x64 = Release|x64\n" );
        fprintf( f, "\tEndGlobalSection\n" );

        fprintf( f, "\tGlobalSection(NestedProjects) = preSolution\n" );
        for ( int i = 0; i < g_target_count; ++i )
        {
            char proj_guid[ 64 ];
            sprintf( proj_guid, "{DE231EAC-9C33-B4FA-8440-E3A81E12%04X}", 0xB000 + i );

            char folder_guid[ 64 ] = "";
            for ( int j = 0; j < folder_count; ++j )
            {
                if ( strcmp( folders[ j ], g_targets[ i ].sln_folder ) == 0 )
                {
                    strcpy( folder_guid, folder_guids[ j ] );
                    break;
                }
            }
            if ( folder_guid[ 0 ] != '\0' )
            {
                fprintf( f, "\t\t%s = %s\n", proj_guid, folder_guid );
            }
        }
        fprintf( f, "\tEndGlobalSection\n" );
        fprintf( f, "EndGlobal\n" );
        fclose( f );
    }

    printf( "Projects generated successfully in %s/.\n", g_build_dir );
}

/*============================================================================================*/