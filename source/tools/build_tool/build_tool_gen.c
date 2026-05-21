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
static const char* g_guid_build  = "{DE231EAC-9C33-B4FA-8440-E3A81E12CA87}";

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
write_vcxproj_common_header( FILE* f, const char* guid, const char* out_name, bool is_lib )
{
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
    fprintf( f, "    <NMakeBuildCommandLine>bin\\build_tool.exe -config $(Configuration) -target %s</NMakeBuildCommandLine>\n", out_name );
    fprintf( f, "    <NMakeOutput>bin\\%s%s</NMakeOutput>\n", out_name, is_lib ? ".lib" : ".exe" );
    fprintf( f, "    <NMakeCleanCommandLine>bin\\build_tool.exe -clean</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
}

static void
build_gen_proj_target( target_info_t* target, int index )
{
    char vcxproj_path[ 256 ];
    sprintf( vcxproj_path, "%s.vcxproj", target->name );

    char guid[ 64 ];
    sprintf( guid, "{DE231EAC-9C33-B4FA-8440-E3A81E12%04X}", 0xB000 + index );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f ) return;

    write_vcxproj_common_header( f, guid, target->name, target->type == TARGET_STATIC_LIB );

    // 1. Scan target directory for files
    // For simplicity, we'll just scan the immediate root_dir for now.
    // If the target spans multiple folders, it would need a more complex scan.
    
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
                    fprintf( f, "    <ClInclude Include=\"%s/%s\" />\n", target->root_dir, find_data.name );
                }
            }
        }
        while ( _findnext( handle, &find_data ) == 0 );
        _findclose( handle );
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < target->unit_count; ++i )
        {
            fprintf( f, "    <ClCompile Include=\"%s/%s\" />\n", target->root_dir, target->units[ i ] );
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
    sprintf( vcxproj_path, "%s.vcxproj", g_proj_name );
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
    fprintf( f, "    <NMakeBuildCommandLine>bin\\build_tool.exe -config $(Configuration)</NMakeBuildCommandLine>\n" );
    fprintf( f, "    <NMakeOutput>bin\\%s.exe</NMakeOutput>\n", g_out_name );
    fprintf( f, "    <NMakeCleanCommandLine>bin\\build_tool.exe -clean</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        const char* tag = g_files[ i ].is_header ? "ClInclude" : "ClCompile";
        fprintf( f, "    <%s Include=\"%s\" />\n", tag, g_files[ i ].path );
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // Filters file
    char filters_path[ 256 ];
    sprintf( filters_path, "%s.vcxproj.filters", g_proj_name );
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
            fprintf( f, "    <%s Include=\"%s\">\n", tag, g_files[ i ].path );
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
    printf( "Generating Visual Studio projects...\n" );

    // 1. Generate Target Projects
    for ( int i = 0; i < g_target_count; ++i )
    {
        build_gen_proj_target( &g_targets[ i ], i );
    }

    // 2. Generate Navigation Project
    build_gen_proj_engine_navigation();

    // 3. Generate Master Solution
    char sln_path[ 256 ];
    sprintf( sln_path, "%s.sln", g_proj_name );
    FILE* f = fopen( sln_path, "w" );
    if ( f )
    {
        fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
        fprintf( f, "# Visual Studio Version 17\n" );

        // Write Engine Project
        fprintf( f, "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"%s\", \"%s.vcxproj\", \"%s\"\n", g_proj_name, g_proj_name, g_guid_engine );
        fprintf( f, "EndProject\n" );

        // Write Target Projects
        for ( int i = 0; i < g_target_count; ++i )
        {
            char guid[ 64 ];
            sprintf( guid, "{DE231EAC-9C33-B4FA-8440-E3A81E12%04X}", 0xB000 + i );
            fprintf( f, "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"%s\", \"%s.vcxproj\", \"%s\"\n", g_targets[ i ].name, g_targets[ i ].name, guid );
            fprintf( f, "EndProject\n" );
        }

        fprintf( f, "Global\n" );
        
        // Configuration Mapping
        fprintf( f, "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n" );
        fprintf( f, "\t\tDebug|x64 = Debug|x64\n" );
        fprintf( f, "\t\tRelease|x64 = Release|x64\n" );
        fprintf( f, "\tEndGlobalSection\n" );

        // Solution Folders (Nesting)
        fprintf( f, "\tGlobalSection(NestedProjects) = preSolution\n" );
        // We'll need another set of GUIDs for the folders themselves.
        // For now, let's keep it simple and just list projects.
        // Folders require explicit "Project" entries of a special type.
        fprintf( f, "\tEndGlobalSection\n" );

        fprintf( f, "EndGlobal\n" );
        fclose( f );
    }

    printf( "Projects generated successfully.\n" );
}

/*============================================================================================*/