/*==============================================================================================

    build_tool_12_gen_msbuild.c -- MSBuild StaticLibrary/DynamicLibrary/Application generator.

    Generates native MSBuild projects (not NMake/Makefile) so Visual Studio uses its
    full EDG IntelliSense pipeline. The EDG front-end reads LanguageStandard_C and
    UseStandardPreprocessor from ItemDefinitionGroup/ClCompile, which the NMake/Makefile
    provider ignores. This is the same project type CMake generates.

    Invoked by:  build_tool.exe -gen_ms
    Output dir:  <solution.out_dir>_msbuild  (e.g. build/proj_msbuild)

    Build model: VS presses Build -> MSBuild -> cl.exe directly 
    (no build_tool.exe involved). CLI builds still use build_tool.exe -config.

    Shares all infrastructure from build_tool_12_gen_nmake.c (included first in unity build):
      guid_from_name(), scan_directory_recursive(), build_intellisense_defines(),
      s_ctx.out_dir / s_ctx.root_prefix / s_ctx.cd_root state, g_files[], g_filters[].

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
    // In monolithic solutions DLL modules are archived as static libs.
    target_type_t eff_type = ( s_ctx.is_monolithic && target->type == TARGET_DYNAMIC_LIB )
                           ? TARGET_STATIC_LIB : target->type;

    const char* cond = ( config == CONFIG_DEBUG )
        ? "'$(Configuration)|$(Platform)'=='Debug|x64'"
        : "'$(Configuration)|$(Platform)'=='Release|x64'";

    char defines[ 1024 ];
    build_intellisense_defines( defines, sizeof( defines ), config, target );

    char extra_incs[ 1024 ];
    build_extra_include_dirs_str( target, extra_incs, sizeof( extra_incs ) );
    const char* extra_sep = extra_incs[ 0 ] ? ";" : "";

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
    fprintf( f, "      <AdditionalIncludeDirectories>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n",
             s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
    fprintf( f, "      <PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>\n",
             defines );
    fprintf( f, "    </ClCompile>\n" );

    // Link settings for exe and DLL targets (DLLs become static libs in monolithic mode).
    if ( eff_type == TARGET_DYNAMIC_LIB || eff_type == TARGET_EXECUTABLE )
    {
        const char* subsystem = ( eff_type == TARGET_EXECUTABLE ) ? "Console" : "Windows";
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
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_ctx.out_dir, target->name );

    char guid[ 64 ];
    guid_from_name( target->name, guid );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    target_type_t eff_type = ( s_ctx.is_monolithic && target->type == TARGET_DYNAMIC_LIB )
                           ? TARGET_STATIC_LIB : target->type;
    const char* cfg_type = msbuild_config_type_str( eff_type );

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

    // Per-config configuration type. Default to $(DefaultPlatformToolset) so MSBuild picks
    // whatever toolset is installed. When -vs-version was explicitly passed, substitute the
    // computed vNNN string (major + 126) to pin the toolset to that version.
    static const char* cfgs[] = { "Debug", "Release" };
    char toolset_str[ 32 ];
    if ( g_vs_major_version > 0 )
        snprintf( toolset_str, sizeof( toolset_str ), "v%d", g_vs_major_version + 126 );
    else
        snprintf( toolset_str, sizeof( toolset_str ), "$(DefaultPlatformToolset)" );
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\" Label=\"Configuration\">\n", cfgs[ ci ] );
        fprintf( f, "    <ConfigurationType>%s</ConfigurationType>\n", cfg_type );
        fprintf( f, "    <PlatformToolset>%s</PlatformToolset>\n", toolset_str );
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
        fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_ctx.root_prefix );
        fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
                 s_ctx.root_prefix, g_build_dir, g_int_dir );
        fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_ctx.cd_root );
        fprintf( f, "  </PropertyGroup>\n" );
    }

    // Per-config compile + link settings.
    write_msbuild_clcompile_group( f, CONFIG_DEBUG,   target );
    write_msbuild_clcompile_group( f, CONFIG_RELEASE, target );

    // Reflection codegen pre-build event. Mirrors step 6 in build_tool_09_exec.c:
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
        fprintf( f, "cd /d \"$(ProjectDir)%s\"", s_ctx.cd_root );
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
        char inc[ PATH_MAX + 32 ];
        gen_inc_path( g_files[ i ].path, inc, sizeof( inc ) );
        if ( is_unit_file( target, g_files[ i ].path ) )
            fprintf( f, "    <ClCompile Include=\"%s\" />\n", inc );
        else
            fprintf( f, "    <ClInclude Include=\"%s\" />\n", inc );
    }

    // Reflection-generated files (may not exist until first build).
    if ( target->has_reflect )
    {
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\" />\n",
                 s_ctx.root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\" />\n",
                 s_ctx.root_prefix, g_build_dir, g_gen_dir, rname );
    }
    fprintf( f, "  </ItemGroup>\n" );

    // ProjectReference items: MSBuild resolves build order and links .lib outputs.
    // In monolithic mode also include mono_deps (runtime-loaded modules that must be linked).
    bool has_refs = target->deps[ 0 ] || ( s_ctx.is_monolithic && target->mono_deps[ 0 ] );
    if ( has_refs )
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
        if ( s_ctx.is_monolithic )
        {
            for ( int i = 0; target->mono_deps[ i ]; ++i )
            {
                char dep_guid[ 64 ];
                guid_from_name( target->mono_deps[ i ], dep_guid );
                fprintf( f, "    <ProjectReference Include=\"%s.vcxproj\">\n", target->mono_deps[ i ] );
                fprintf( f, "      <Project>%s</Project>\n", dep_guid );
                fprintf( f, "    </ProjectReference>\n" );
            }
        }
        fprintf( f, "  </ItemGroup>\n" );
    }

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // .filters file: mirrors the on-disk folder tree.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s.vcxproj.filters", s_ctx.out_dir, target->name );
    write_vcxproj_filters_file( filters_path, target );
}

/*==============================================================================================

    build_gen_projects_msbuild()

    Top-level entry point invoked by `build_tool.exe -gen_ms`. Mirrors
    build_gen_projects() but generates MSBuild StaticLibrary/DLL/Application projects
    instead of NMake/Makefile projects. Output lands in <sln.out_dir>_ms.

    build_gen_solution() from build_tool_12_gen_nmake.c is reused as-is -- the .sln format is
    identical regardless of project type, and it reads s_ctx.out_dir (already updated by
    compute_path_parts) rather than sln->out_dir directly.

==============================================================================================*/

void
build_gen_projects_msbuild( const gen_manifest_t* m )
{
    run_solution_passes( m, "_ms", "_ms", "MSBuild Solution", build_gen_proj_target_msbuild );
}

// clang-format on
/*============================================================================================*/
