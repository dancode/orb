/*==============================================================================================

    build_tool_12_gen_msbuild.c -- MSBuild StaticLibrary/DynamicLibrary/Application generator.

    Generates native MSBuild projects (not NMake/Makefile) so Visual Studio uses its
    full EDG IntelliSense pipeline. The EDG front-end reads LanguageStandard_C and
    UseStandardPreprocessor from ItemDefinitionGroup/ClCompile, which the NMake/Makefile
    provider ignores. This is the same project type CMake generates.

    Invoked by:  build_tool.exe -gen_ms
    Output dir:  <solution.out_dir>_ms  (e.g. build/proj_ms)

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
    msbuild_emit_prologue()

    XML preamble shared by the full target writer and the alias (Utility) writer:
    ProjectConfigurations, Globals, Cpp.Default.props, the per-config ConfigurationType
    + PlatformToolset groups, and Cpp.props. Everything after Cpp.props differs per
    writer (compile/link settings vs. bare debugger groups).
==============================================================================================*/

static void
msbuild_emit_prologue( FILE* f, const char* guid, const char* cfg_type )
{
    static const char* cfgs[ 2 ] = { "Debug", "Release" };

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );

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

    // Per-config configuration type + toolset ($(DefaultPlatformToolset) unless pinned
    // by -vs-version; see gen_platform_toolset).
    char ts[ 32 ];
    gen_platform_toolset( ts, sizeof( ts ) );
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\" Label=\"Configuration\">\n", cfgs[ ci ] );
        fprintf( f, "    <ConfigurationType>%s</ConfigurationType>\n", cfg_type );
        fprintf( f, "    <PlatformToolset>%s</PlatformToolset>\n", ts );
        fprintf( f, "  </PropertyGroup>\n" );
    }

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );
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
        /GF (Debug)        -> StringPooling true  (Release gets it free from /O2)
        /MDd (Debug)       -> RuntimeLibrary MultiThreadedDebugDLL
        /O2 (Release)      -> Optimization MaxSpeed
        /MD  (Release)     -> RuntimeLibrary MultiThreadedDLL
        /wd<N> entries     -> DisableSpecificWarnings (driven by g_warn_suppressions[])
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
        // /GF: Release inherits string pooling from /O2, Debug has to ask for it.
        fprintf( f, "      <StringPooling>true</StringPooling>\n" );
        fprintf( f, "      <RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>\n" );
    }
    else
    {
        fprintf( f, "      <Optimization>MaxSpeed</Optimization>\n" );
        fprintf( f, "      <RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>\n" );
    }

    // Collect /wd<num> entries from g_warn_suppressions that apply to this config + MSVC.
    // MSBuild uses <DisableSpecificWarnings> instead of /wd flags on the command line.
    char   disabled_warns[ 256 ] = { 0 };
    size_t dw_used               = 0;
    for ( int i = 0; i < g_warn_suppression_count; ++i )
    {
        warn_suppress_t* s = &g_warn_suppressions[ i ];
        if ( s->compiler != COMPILE_MSVC ) continue;
        if ( s->config != config && s->config != CONFIG_COUNT ) continue;
        if ( strncmp( s->flag, "/wd", 3 ) != 0 ) continue;
        gen_list_append( disabled_warns, sizeof( disabled_warns ), &dw_used, ';', s->flag + 3 );
    }
    if ( disabled_warns[ 0 ] )
        fprintf( f, "      <DisableSpecificWarnings>%s</DisableSpecificWarnings>\n", disabled_warns );

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

        // Per-target 'link_flag' directives (e.g. font_tool's freetype.lib) -- appended verbatim to
        // the linker command line, mirroring the NMake link path (build_tool_08_link.c) so this
        // solution links the same libs the real build does.  A bare relative path (a .lib) is
        // anchored to the repo root via $(ProjectDir)<root_prefix> -- the MSBuild link task's CWD is
        // the project dir, not the repo root the NMake build runs from -- while a raw switch (/STACK,
        // etc.) or an already-absolute path is emitted as-is.  NMake filters these by compiler only
        // (not config), so emit them in every config here too.
        char link_opts[ 1024 ] = { 0 };
        size_t lo_used = 0;
        for ( int i = 0; i < target->extra_link_flag_count; ++i )
        {
            const extra_flag_t* ef = &target->extra_link_flags[ i ];
            if ( ef->compiler != COMPILE_MSVC && ef->compiler != COMPILE_ALL )
                continue;

            char tok[ PATH_MAX ];
            if ( ef->flag[ 0 ] == '/' || ef->flag[ 0 ] == '-' || platform_is_abs_path( ef->flag ) )
                snprintf( tok, sizeof( tok ), "%s", ef->flag );                                // raw switch / absolute
            else
                snprintf( tok, sizeof( tok ), "$(ProjectDir)%s%s", s_ctx.root_prefix, ef->flag ); // repo-relative lib
            gen_list_append( link_opts, sizeof( link_opts ), &lo_used, ' ', tok );
        }
        if ( link_opts[ 0 ] )
            fprintf( f, "      <AdditionalOptions>%s %%(AdditionalOptions)</AdditionalOptions>\n", link_opts );

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

/* Alias launchers in the MSBuild flavor: a native Utility project that builds NOTHING.
   The .sln ProjectDependencies entry (see build_gen_solution) makes MSBuild build the
   aliased target natively first; this project only carries the F5 debugger command.
   Shelling out to build_tool.exe here (as the NMake flavor does) would run a second
   build system over the same bin\ artifact the native build just produced. */
static void
build_gen_proj_alias_msbuild( target_info_t* target )
{
    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_ctx.out_dir, target->name );

    char guid[ GUID_STR_MAX ];
    guid_from_name( target->name, guid, sizeof( guid ) );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    msbuild_emit_prologue( f, guid, "Utility" );
    gen_emit_debug_property_groups( f, target, true );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );
}

static void
build_gen_proj_target_msbuild( target_info_t* target )
{
    // Alias launchers: native no-build Utility project carrying the F5 command.
    if ( target->alias_for )
    {
        build_gen_proj_alias_msbuild( target );
        return;
    }

    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_ctx.out_dir, target->name );

    char guid[ GUID_STR_MAX ];
    guid_from_name( target->name, guid, sizeof( guid ) );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    target_type_t eff_type = ( s_ctx.is_monolithic && target->type == TARGET_DYNAMIC_LIB )
                           ? TARGET_STATIC_LIB : target->type;

    msbuild_emit_prologue( f, guid, msbuild_config_type_str( eff_type ) );

    // Property sheets (user overrides).
    static const char* cfgs[] = { "Debug", "Release" };
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <ImportGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\" Label=\"PropertySheets\">\n", cfgs[ ci ] );
        fprintf( f, "    <Import Project=\"$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props\" Condition=\"exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')\" Label=\"LocalAppDataPlatform\" />\n" );
        fprintf( f, "  </ImportGroup>\n" );
    }

    // Output and intermediate directories + debugger defaults (see the NMake writer for
    // the 'run' contract; user edits land in .vcxproj.user and override these).
    gen_emit_debug_property_groups( f, target, true );

    // Per-config compile + link settings.
    write_msbuild_clcompile_group( f, CONFIG_DEBUG,   target );
    write_msbuild_clcompile_group( f, CONFIG_RELEASE, target );

    // Reflection codegen pre-build event. Mirrors build_gen_reflect() in build_tool_09_exec.c:
    //   bin\<reflect tool>.exe -src <root_dir> -out <gen_dir> -name <reflect_name>
    // The tool's name comes from whichever target carries is_reflect_tool, not a literal, so a
    // project that registers its own reflect tool gets a working pre-build event. A target with
    // has_reflect and no such target registered emits no reflection command at all -- a real
    // build reports that as an error, and a bogus command line here would only obscure it.
    // Two-step command:
    //   1. build_tool.exe builds the reflect tool if missing or stale (incremental,
    //      nearly instant when already up to date). This handles fresh checkouts
    //      where the tool hasn't been compiled yet. It is not included in the
    //      solution's project list, so VS won't build it on its own.
    //      s_ctx.build_tool_exe resolves to the engine-absolute path for child
    //      projects (a child bin has no build_tool.exe); the tool's .exe lands in
    //      the local bin because build_tool runs with CWD = this project's root.
    //   2. the reflect tool generates the .generated.c/.h files.
    // cd /d also changes drive letter so projects on any drive work correctly.
    // NOTE: avoid "if not exist ... (cmd) && next" -- cmd.exe absorbs the && into
    // the if clause. Using build_tool.exe unconditionally avoids that trap entirely.
    // The resource manifest rides the same pre-build event: build_tool -res-manifest builds
    // res_tool if needed, computes the image's unit closure, and runs the harvest -- the
    // closure comes from orb.targets, which only build_tool can read. Nothing is compiled
    // from it; the event is how a VS build still proves every marked name resolves.
    bool                 wants_res = target_wants_res_manifest( target );
    const target_info_t* refl_tool = target->has_reflect ? find_reflect_tool() : NULL;
    if ( refl_tool || wants_res )
    {
        fprintf( f, "  <ItemDefinitionGroup>\n" );
        fprintf( f, "    <PreBuildEvent>\n" );
        fprintf( f, "      <Message>build_tool: generating %s%s%s for %s</Message>\n",
                 refl_tool ? "reflection" : "",
                 ( refl_tool && wants_res ) ? " + " : "",
                 wants_res ? "resource manifest" : "", target->name );
        fprintf( f, "      <Command>" );
        fprintf( f, "cd /d \"$(ProjectDir)%s\"", s_ctx.cd_root );
        if ( refl_tool )
        {
            const char* rname = target_reflect_name( target );

            char root_dir_norm[ PATH_MAX ];
            snprintf( root_dir_norm, sizeof( root_dir_norm ), "%s", target->root_dir );
            for ( char* p = root_dir_norm; *p; ++p )
                if ( *p == '/' ) *p = '\\';

            fprintf( f, " &amp;&amp; %s -config $(Configuration) -target %s", s_ctx.build_tool_exe, refl_tool->name );
            fprintf( f, " &amp;&amp; bin\\%s.exe -src %s -out %s\\%s -name %s",
                     refl_tool->name, root_dir_norm, g_build_dir, g_gen_dir, rname );
        }
        if ( wants_res )
            fprintf( f, " &amp;&amp; %s -config $(Configuration) -target %s -res-manifest", s_ctx.build_tool_exe, target->name );
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
        if ( g_files[ i ].is_natvis )
            continue;    // emitted in a separate Natvis ItemGroup below
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
        const char* rname = target_reflect_name( target );
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\" />\n",
                 s_ctx.root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\" />\n",
                 s_ctx.root_prefix, g_build_dir, g_gen_dir, rname );
    }

    fprintf( f, "  </ItemGroup>\n" );

    write_natvis_item_group( f );

    // ProjectReference items: MSBuild resolves build order and links .lib outputs.
    // In monolithic mode also include mono_deps (runtime-loaded modules that must be linked).
    bool has_refs = target->deps[ 0 ] || ( s_ctx.is_monolithic && target->mono_deps[ 0 ] );
    if ( has_refs )
    {
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            char dep_guid[ GUID_STR_MAX ];
            guid_from_name( target->deps[ i ], dep_guid, sizeof( dep_guid ) );
            fprintf( f, "    <ProjectReference Include=\"%s.vcxproj\">\n", target->deps[ i ] );
            fprintf( f, "      <Project>%s</Project>\n", dep_guid );
            fprintf( f, "    </ProjectReference>\n" );
        }
        if ( s_ctx.is_monolithic )
        {
            for ( int i = 0; target->mono_deps[ i ]; ++i )
            {
                char dep_guid[ GUID_STR_MAX ];
                guid_from_name( target->mono_deps[ i ], dep_guid, sizeof( dep_guid ) );
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
    // The reflect pre-build event shells to build_tool; resolve its path here too so
    // a standalone -gen-msbuild doesn't depend on the NMake pass having set it.
    snprintf( s_ctx.build_tool_exe, sizeof( s_ctx.build_tool_exe ), "%s", m->build_tool_exe );
    run_solution_passes( m, "_ms", "_ms", "MSBuild Solution", build_gen_proj_target_msbuild );
}

// clang-format on
/*============================================================================================*/
