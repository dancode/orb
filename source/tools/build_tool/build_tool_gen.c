/*==============================================================================================

    build_tool_gen.c -- Generation of Visual Studio project files.

==============================================================================================*/

typedef struct
{
    char** paths;
    size_t count;
    size_t cap;
} path_list_t;

static void
path_list_append( path_list_t* list, const char* path )
{
    if ( list->count >= list->cap )
    {
        list->cap   = list->cap == 0 ? 256 : list->cap * 2;
        list->paths = realloc( list->paths, list->cap * sizeof( char* ) );
    }
    list->paths[ list->count++ ] = _strdup( path );
}

static void
scan_directory( const char* dir, path_list_t* cl_files, path_list_t* h_files )
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
            scan_directory( path, cl_files, h_files );
        }
        else
        {
            const char* ext = strrchr( find_data.name, '.' );
            if ( ext )
            {
                if ( _stricmp( ext, ".c" ) == 0 )
                {
                    path_list_append( cl_files, path );
                }
                else if ( _stricmp( ext, ".h" ) == 0 )
                {
                    path_list_append( h_files, path );
                }
            }
        }
    }
    while ( _findnext( handle, &find_data ) == 0 );

    _findclose( handle );
}

static void
build_gen_proj_build_tool( void )
{
    char vcxproj_path[ 256 ];
    sprintf( vcxproj_path, "%s.vcxproj", g_build_proj_name );

    const char* build_tool_compile[] = {
        "source/tools/build_tool/build_tool.c",
    };
    const char* build_tool_header[] = {
        "source/tools/build_tool/build_tool.h",
        "source/tools/build_tool/build_tool_gen.c",
    };
    size_t build_tool_compile_count = sizeof( build_tool_compile ) / sizeof( build_tool_compile[ 0 ] );
    size_t build_tool_header_count = sizeof( build_tool_header ) / sizeof( build_tool_header[ 0 ] );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( f )
    {
        fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
        fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
        fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
        fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
        fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
        fprintf( f, "    <ProjectGuid>{DE231EAC-9C33-B4FA-8440-E3A81E12CA87}</ProjectGuid>\n" );
        fprintf( f, "  </PropertyGroup>\n" );
        fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
        fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
        fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
        fprintf( f, "    <PlatformToolset>v143</PlatformToolset>\n" );
        fprintf( f, "  </PropertyGroup>\n" );
        fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );
        fprintf( f, "  <PropertyGroup>\n" );
        fprintf( f, "    <NMakeBuildCommandLine>cl.exe /nologo /W4 /Zi source/tools/build_tool/build_tool.c /I source /Foobj/ /Fdobj/ /Fe:bin/build_tool.exe</NMakeBuildCommandLine>\n" );
        fprintf( f, "    <NMakeOutput>bin\\build_tool.exe</NMakeOutput>\n" );
        fprintf( f, "    <NMakeCleanCommandLine>bin\\build_tool.exe -clean</NMakeCleanCommandLine>\n" );
        fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
        fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
        fprintf( f, "  </PropertyGroup>\n" );
        
        fprintf( f, "  <ItemGroup>\n" );
        for ( size_t i = 0; i < build_tool_header_count; ++i )
        {
            fprintf( f, "    <ClInclude Include=\"%s\" />\n", build_tool_header[ i ] );
        }
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <ItemGroup>\n" );
        for ( size_t i = 0; i < build_tool_compile_count; ++i )
        {
            fprintf( f, "    <ClCompile Include=\"%s\" />\n", build_tool_compile[ i ] );
        }
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }

    char sln_path[ 256 ];
    sprintf( sln_path, "%s.sln", g_build_proj_name );
    f = fopen( sln_path, "w" );
    if ( f )
    {
        fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
        fprintf( f, "# Visual Studio Version 17\n" );
        fprintf( f, "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"%s\", \"%s.vcxproj\", \"{DE231EAC-9C33-B4FA-8440-E3A81E12CA87}\"\n", g_build_proj_name, g_build_proj_name );
        fprintf( f, "EndProject\n" );
        fprintf( f, "Global\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n\t\tDebug|x64 = Debug|x64\n\t\tRelease|x64 = Release|x64\n\tEndGlobalSection\nEndGlobal\n" );
        fclose( f );
    }
}

static void
build_gen_proj_engine( void )
{
    path_list_t cl_files = { 0 };
    path_list_t h_files  = { 0 };
    scan_directory( "source", &cl_files, &h_files );

    char vcxproj_path[ 256 ];
    sprintf( vcxproj_path, "%s.vcxproj", g_proj_name );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( f )
    {
        fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
        fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
        fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
        fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
        fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
        fprintf( f, "    <ProjectGuid>{DE231EAC-9C33-B4FA-8440-E3A81E12CA86}</ProjectGuid>\n" );
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
        for ( size_t i = 0; i < h_files.count; ++i ) fprintf( f, "    <ClInclude Include=\"%s\" />\n", h_files.paths[ i ] );
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <ItemGroup>\n" );
        for ( size_t i = 0; i < cl_files.count; ++i ) fprintf( f, "    <ClCompile Include=\"%s\" />\n", cl_files.paths[ i ] );
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }

    char sln_path[ 256 ];
    sprintf( sln_path, "%s.sln", g_proj_name );
    f = fopen( sln_path, "w" );
    if ( f )
    {
        fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
        fprintf( f, "# Visual Studio Version 17\n" );
        fprintf( f, "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"%s\", \"%s.vcxproj\", \"{DE231EAC-9C33-B4FA-8440-E3A81E12CA86}\"\n", g_proj_name, g_proj_name );
        fprintf( f, "EndProject\n" );
        fprintf( f, "Global\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n\t\tDebug|x64 = Debug|x64\n\t\tRelease|x64 = Release|x64\n\tEndGlobalSection\nEndGlobal\n" );
        fclose( f );
    }

    for ( size_t i = 0; i < cl_files.count; ++i ) free( cl_files.paths[ i ] );
    for ( size_t i = 0; i < h_files.count; ++i ) free( h_files.paths[ i ] );
    free( cl_files.paths );
    free( h_files.paths );
}

void
build_gen_projects( void )
{
    printf( "Generating Visual Studio projects...\n" );

    build_gen_proj_build_tool();
    build_gen_proj_engine();

    printf( "Projects generated successfully.\n" );
}

/*============================================================================================*/