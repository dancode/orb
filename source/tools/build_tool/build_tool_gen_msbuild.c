/*==============================================================================================

    build_tool_gen_msbuild.c -- MSBuild StaticLibrary/DynamicLibrary/Application generator.

    Generates native MSBuild projects (not NMake/Makefile) so Visual Studio uses its
    full EDG IntelliSense pipeline. The EDG front-end reads LanguageStandard_C and
    UseStandardPreprocessor from ItemDefinitionGroup/ClCompile, which the NMake/Makefile
    provider ignores. This is the same project type CMake generates.

    Invoked by:  build_tool.exe -gen_ms
    Output dir:  <solution.out_dir>_msbuild  (e.g. build/proj_msbuild)

    Build model: VS presses Build -> MSBuild -> cl.exe directly (no build_tool.exe
    involved). Actual CLI builds still use build_tool.exe -config.

    Shares all infrastructure from build_tool_11_gen.c (included first in unity build):
      guid_from_name(), scan_directory_recursive(), build_intellisense_defines(),
      s_out_dir / s_root_prefix / s_cd_root state, g_files[], g_filters[].

==============================================================================================*/

// clang-format off

/*==============================================================================================
    msbuild_config_type_str()

    Maps target_type_t to the MSBuild ConfigurationType string.
==============================================================================================*/

static const char*
msbuild_config_type_str( target_type_t type )
{
    if ( type == TARGET_STATIC_LIB  ) return "StaticLibrary";
    if ( type == TARGET_DYNAMIC_LIB ) return "DynamicLibrary";
    return "Application";
}

/*==============================================================================================
    write_msbuild_clcompile_group()

    Emits one <ItemDefinitionGroup Condition="..."> block containing the <ClCompile>
    properties for IntelliSense and the actual MSBuild compile step. Maps directly from
    platform_cc_base_flags() semantics to MSBuild property names:

        /W4                -> WarningLevel Level4
        /WX                -> TreatWarningAsError true
        /Zc:preprocessor   -> UseStandardPreprocessor true
        /std:c11           -> LanguageStandard_C stdc11
        /Zi (Debug)        -> DebugInformationFormat ProgramDatabase
        /Od (Debug)        -> Optimization Disabled
        /MDd (Debug)       -> RuntimeLibrary MultiThreadedDebugDLL
        /O2 (Release)      -> Optimization MaxSpeed
        /MD  (Release)     -> RuntimeLibrary MultiThreadedDLL
==============================================================================================*/

static void
write_msbuild_clcompile_group( FILE* f, config_t config, target_info_t* target )
{
    const char* cond = ( config == CONFIG_DEBUG )
        ? "'$(Configuration)|$(Platform)'=='Debug|x64'"
        : "'$(Configuration)|$(Platform)'=='Release|x64'";

    char defines[ 1024 ];
    build_intellisense_defines( defines, sizeof( defines ), config, target );

    fprintf( f, "  <ItemDefinitionGroup Condition=\"%s\">\n", cond );
    fprintf( f, "    <ClCompile>\n" );
    fprintf( f, "      <WarningLevel>Level4</WarningLevel>\n" );
    fprintf( f, "      <TreatWarningAsError>true</TreatWarningAsError>\n" );

    if ( config == CONFIG_DEBUG )
    {
        fprintf( f, "      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>\n" );
        fprintf( f, "      <Optimization>Disabled</Optimization>\n" );
        fprintf( f, "      <RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>\n" );
    }
    else
    {
        fprintf( f, "      <Optimization>MaxSpeed</Optimization>\n" );
        fprintf( f, "      <RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>\n" );
    }

    fprintf( f, "      <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
    fprintf( f, "      <UseStandardPreprocessor>true</UseStandardPreprocessor>\n" );
    fprintf( f, "      <ScanSourceForModuleDependencies>false</ScanSourceForModuleDependencies>\n" );
    fprintf( f, "      <AdditionalIncludeDirectories>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n",
             s_root_prefix, s_root_prefix, g_build_dir, g_gen_dir );
    fprintf( f, "      <PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>\n",
             defines );
    fprintf( f, "    </ClCompile>\n" );

    // Link / Lib settings for non-static targets.
    if ( target->type == TARGET_DYNAMIC_LIB || target->type == TARGET_EXECUTABLE )
    {
        const char* subsystem = ( target->type == TARGET_EXECUTABLE ) ? "Console" : "Windows";
        const char* gen_debug = ( config == CONFIG_DEBUG ) ? "true" : "false";
        fprintf( f, "    <Link>\n" );
        fprintf( f, "      <SubSystem>%s</SubSystem>\n", subsystem );
        fprintf( f, "      <GenerateDebugInformation>%s</GenerateDebugInformation>\n", gen_debug );
        fprintf( f, "      <AdditionalDependencies>user32.lib;shell32.lib;gdi32.lib;advapi32.lib;%%(AdditionalDependencies)</AdditionalDependencies>\n" );
        fprintf( f, "    </Link>\n" );
    }

    fprintf( f, "  </ItemDefinitionGroup>\n" );
}

/*==============================================================================================
    build_gen_proj_target_msbuild()

    Emits one .vcxproj + .vcxproj.filters for a target as a real MSBuild project.
    Unity .c is listed as <ClCompile>; all other .c and .h are <ClInclude> so IntelliSense
    context flows from the unity TU without per-file compile interference.
    Dep targets are listed as <ProjectReference> items -- MSBuild resolves build order
    and links the resulting .lib automatically.
==============================================================================================*/

static void
build_gen_proj_target_msbuild( target_info_t* target )
{
    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_out_dir, target->name );

    char guid[ 64 ];
    guid_from_name( target->name, guid );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    const char* cfg_type = msbuild_config_type_str( target->type );

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    // Configurations.
    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );

    // Globals.
    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", guid );
    fprintf( f, "    <Keyword>Win32Proj</Keyword>\n" );
    fprintf( f, "    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>\n" );
    fprintf( f, "    <PreferredToolArchitecture>x64</PreferredToolArchitecture>\n" );
    // Default Platform to x64 when not supplied externally (e.g. standalone msbuild.exe
    // invocation or early evaluation before the solution configuration is resolved).
    // Without this, MSBuild falls back to Win32 and fires MSB8013 because Debug|Win32
    // is not in our ProjectConfigurations list.
    fprintf( f, "    <Platform Condition=\"'$(Platform)'==''\">x64</Platform>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );

    // Per-config configuration type. v143 = VS2022 toolset.
    static const char* cfgs[] = { "Debug", "Release" };
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\" Label=\"Configuration\">\n", cfgs[ ci ] );
        fprintf( f, "    <ConfigurationType>%s</ConfigurationType>\n", cfg_type );
        fprintf( f, "    <PlatformToolset>v143</PlatformToolset>\n" );
        fprintf( f, "  </PropertyGroup>\n" );
    }

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Property sheets (user overrides).
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <ImportGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\" Label=\"PropertySheets\">\n", cfgs[ ci ] );
        fprintf( f, "    <Import Project=\"$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props\" Condition=\"exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')\" Label=\"LocalAppDataPlatform\" />\n" );
        fprintf( f, "  </ImportGroup>\n" );
    }

    // Output and intermediate directories.
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\">\n", cfgs[ ci ] );
        fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_root_prefix );
        fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
                 s_root_prefix, g_build_dir, g_int_dir );
        fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_cd_root );
        fprintf( f, "  </PropertyGroup>\n" );
    }

    // Per-config compile + link settings.
    write_msbuild_clcompile_group( f, CONFIG_DEBUG,   target );
    write_msbuild_clcompile_group( f, CONFIG_RELEASE, target );

    // Reflection codegen pre-build event. Mirrors step 6 in build_tool_08_exec.c:
    //   bin\reflect_tool.exe <root_dir> <gen_dir> <reflect_name>
    // Two-step command:
    //   1. build_tool.exe builds reflect_tool if missing or stale (incremental,
    //      nearly instant when already up to date). This handles fresh checkouts
    //      where reflect_tool.exe hasn't been compiled yet. reflect_tool is not
    //      included in the solution's project list, so VS won't build it on its own.
    //   2. reflect_tool.exe generates the .generated.c/.h files.
    // cd /d also changes drive letter so projects on any drive work correctly.
    // NOTE: avoid "if not exist ... (cmd) && next" -- cmd.exe absorbs the && into
    // the if clause. Using build_tool.exe unconditionally avoids that trap entirely.
    if ( target->has_reflect )
    {
        const char* rname = target->reflect_name ? target->reflect_name : target->name;

        char root_dir_norm[ PATH_MAX ];
        snprintf( root_dir_norm, sizeof( root_dir_norm ), "%s", target->root_dir );
        for ( char* p = root_dir_norm; *p; ++p )
            if ( *p == '/' ) *p = '\\';

        fprintf( f, "  <ItemDefinitionGroup>\n" );
        fprintf( f, "    <PreBuildEvent>\n" );
        fprintf( f, "      <Message>reflect_tool: building tool and generating %s.generated.c/.h</Message>\n", rname );
        fprintf( f, "      <Command>" );
        fprintf( f, "cd /d \"$(ProjectDir)%s\"", s_cd_root );
        fprintf( f, " &amp;&amp; bin\\build_tool.exe -config $(Configuration) -target reflect_tool" );
        fprintf( f, " &amp;&amp; bin\\reflect_tool.exe %s %s\\%s %s", root_dir_norm, g_build_dir, g_gen_dir, rname );
        fprintf( f, "</Command>\n" );
        fprintf( f, "    </PreBuildEvent>\n" );
        fprintf( f, "  </ItemDefinitionGroup>\n" );
    }

    // Source files: unity .c as ClCompile, everything else as ClInclude.
    g_file_count   = 0;
    g_filter_count = 0;
    scan_directory_recursive( target->root_dir, target->root_dir );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        bool        is_unit  = false;
        const char* filename = g_files[ i ].path;
        for ( const char* p = g_files[ i ].path; *p; ++p )
            if ( *p == '/' || *p == '\\' ) filename = p + 1;

        for ( int j = 0; target->units[ j ]; ++j )
        {
            if ( platform_stricmp( filename, target->units[ j ] ) == 0 )
            {
                is_unit = true;
                break;
            }
        }

        if ( is_unit )
            fprintf( f, "    <ClCompile Include=\"%s%s\" />\n", s_root_prefix, g_files[ i ].path );
        else
            fprintf( f, "    <ClInclude Include=\"%s%s\" />\n", s_root_prefix, g_files[ i ].path );
    }

    // Reflection-generated files (may not exist until first build).
    if ( target->has_reflect )
    {
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\" />\n",
                 s_root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\" />\n",
                 s_root_prefix, g_build_dir, g_gen_dir, rname );
    }
    fprintf( f, "  </ItemGroup>\n" );

    // ProjectReference items: MSBuild resolves build order and links .lib outputs.
    if ( target->deps[ 0 ] )
    {
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            char dep_guid[ 64 ];
            guid_from_name( target->deps[ i ], dep_guid );
            fprintf( f, "    <ProjectReference Include=\"%s.vcxproj\">\n", target->deps[ i ] );
            fprintf( f, "      <Project>%s</Project>\n", dep_guid );
            fprintf( f, "    </ProjectReference>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
    }

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // .filters file: mirrors the on-disk folder tree.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s.vcxproj.filters", s_out_dir, target->name );
    f = fopen( filters_path, "w" );
    if ( !f ) return;

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_filter_count; ++i )
    {
        fprintf( f, "    <Filter Include=\"%s\">\n", g_filters[ i ] );
        fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n",
                 ( unsigned int )i );
        fprintf( f, "    </Filter>\n" );
    }
    if ( target->has_reflect )
    {
        fprintf( f, "    <Filter Include=\"generated\">\n" );
        fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n",
                 ( unsigned int )g_filter_count );
        fprintf( f, "    </Filter>\n" );
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        bool        is_unit  = false;
        const char* filename = strrchr( g_files[ i ].path, '/' );
        if ( !filename ) filename = strrchr( g_files[ i ].path, '\\' );
        if ( filename ) filename++; else filename = g_files[ i ].path;

        for ( int j = 0; target->units[ j ]; ++j )
        {
            if ( platform_stricmp( filename, target->units[ j ] ) == 0 )
            {
                is_unit = true;
                break;
            }
        }

        const char* tag = is_unit ? "ClCompile" : "ClInclude";
        fprintf( f, "    <%s Include=\"%s%s\">\n", tag, s_root_prefix, g_files[ i ].path );
        if ( g_files[ i ].filter[ 0 ] != '\0' )
            fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
        fprintf( f, "    </%s>\n", tag );
    }
    if ( target->has_reflect )
    {
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\">\n",
                 s_root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "      <Filter>generated</Filter>\n" );
        fprintf( f, "    </ClCompile>\n" );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\">\n",
                 s_root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "      <Filter>generated</Filter>\n" );
        fprintf( f, "    </ClInclude>\n" );
    }
    fprintf( f, "  </ItemGroup>\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );
}

/*==============================================================================================

    build_gen_projects_msbuild()

    Top-level entry point invoked by `build_tool.exe -gen_ms`. Mirrors
    build_gen_projects() but generates MSBuild StaticLibrary/DLL/Application projects
    instead of NMake/Makefile projects. Output lands in <sln.out_dir>_msbuild.

    build_gen_solution() from build_tool_11_gen.c is reused as-is -- the .sln format is
    identical regardless of project type, and it reads s_out_dir (already updated by
    compute_path_parts) rather than sln->out_dir directly.

==============================================================================================*/

void
build_gen_projects_msbuild( void )
{
    for ( int i = 0; i < g_solution_count; ++i )
    {
        solution_info_t* sln = &g_solutions[ i ];
        s_is_monolithic = sln->is_monolithic;

        // MSBuild projects land alongside the NMake ones, in a sibling dir.
        char msbuild_out_dir[ PATH_MAX ];
        snprintf( msbuild_out_dir, sizeof( msbuild_out_dir ), "%s_msbuild", sln->out_dir );

        compute_path_parts( msbuild_out_dir );
        ensure_dir( msbuild_out_dir );

        printf( "Generating MSBuild Solution '%s' in %s/...\n", sln->name, msbuild_out_dir );

        for ( const char* const* tn = sln->target_names; *tn; ++tn )
        {
            for ( int j = 0; j < g_target_count; ++j )
            {
                if ( strcmp( g_targets[ j ].name, *tn ) == 0 )
                {
                    build_gen_proj_target_msbuild( &g_targets[ j ] );
                    break;
                }
            }
        }

        // Solution file shares format with NMake; reuse the existing writer.
        // s_out_dir is already pointing at msbuild_out_dir from compute_path_parts above.
        build_gen_solution( sln );
    }

    printf( "\nMSBuild projects generated successfully.\n" );
}

// clang-format on
/*============================================================================================*/
