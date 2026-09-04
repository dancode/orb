/*==============================================================================================

    build_tool_12_gen_nmake.c -- Makefile-type .vcxproj generator.

    Invoked by:  build_tool.exe -gen
    Output dir:  <solution.out_dir>  (e.g. build/proj)

    This format "hijacks" Visual Studio: each .vcxproj is emitted with
    ConfigurationType Makefile, and its NMakeBuildCommandLine shells out to
    bin\build_tool.exe. The IDE provides IntelliSense, the debugger, and source
    navigation; build_tool keeps full control over the actual compile/link pipeline.
    build_tool_12_gen_msbuild.c is the other format -- native MSBuild projects, where
    MSBuild drives cl.exe itself.

    What this file emits, per target in a solution:

      <target>.vcxproj             -- Makefile project pointing at build_tool.exe
      <target>.vcxproj.filters     -- mirrors the on-disk folder tree

    Everything else a solution needs -- the directory scan, GUIDs, IntelliSense values,
    the .filters writer, the navigation project, and the .sln itself -- is shared with
    the MSBuild format and lives in build_tool_12_gen_vs.c.

==============================================================================================*/

/*==============================================================================================
    write_vcxproj_common_header()

    Writes the boilerplate XML required for a Visual Studio Makefile project.
    Three layers of config data:
      1. Unconditional PropertyGroup: OutDir/IntDir and the NMake build/clean commands.
      2. Per-config IntelliSense groups (ItemDefinitionGroup + NMake PropertyGroup):
         see emit_intellisense_config_groups() in 12_gen_vs.c.
      3. Per-config LocalDebuggerWorkingDirectory so F5 launches from the project root,
         plus LocalDebuggerCommand/Arguments when the target declares a 'run' line
         (DLL projects F5 into the host exe that loads them).

    Alias launcher targets route their build/clean/output through the aliased target;
    the project itself exists only to carry a distinct F5 command.
==============================================================================================*/

static void
write_vcxproj_common_header( FILE* f, const char* guid, target_type_t type, target_info_t* target )
{
    // Alias launchers: build commands and NMakeOutput act on the aliased target.
    const char* build_name = target->alias_for ? target->alias_for : target->name;

    const char* ext = ".exe";
    if ( type == TARGET_STATIC_LIB )    ext = ".lib";
    if ( type == TARGET_DYNAMIC_LIB )   ext = s_ctx.is_monolithic ? ".lib" : ".dll";

    const char* mono_flag = s_ctx.is_monolithic ? " -monolithic" : "";

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <PreferredToolArchitecture>x64</PreferredToolArchitecture>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", guid );
    fprintf( f, "    <Keyword>Win32Proj</Keyword>\n" );
    fprintf( f, "    <Platform Condition=\"'$(Platform)'==''\">x64</Platform>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    { char ts[ 32 ]; fprintf( f, "    <PlatformToolset>%s</PlatformToolset>\n", gen_platform_toolset( ts, sizeof( ts ) ) ); }
    // LanguageStandard_C and IntelliSenseMode are emitted per-config below.
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Unconditional: build commands. -no-deps lets MSBuild's scheduler (which honors
    // ProjectDependencies in the .sln) own build order so parallel solution builds
    // never race on shared dep outputs.
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_ctx.root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
             s_ctx.root_prefix, g_build_dir, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd %s &amp;&amp; %s -no-deps -config $(Configuration) -target %s%s</NMakeBuildCommandLine>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, build_name, mono_flag );
    fprintf( f, "    <NMakeOutput>%sbin\\%s%s</NMakeOutput>\n", s_ctx.root_prefix, build_name, ext );
    fprintf( f, "    <NMakeCleanCommandLine>cd %s &amp;&amp; %s -clean -target %s</NMakeCleanCommandLine>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, build_name );
    fprintf( f, "    <NMakeCompileFile>cd %s &amp;&amp; %s -no-deps -config $(Configuration) -target %s%s</NMakeCompileFile>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, build_name, mono_flag );
    fprintf( f, "  </PropertyGroup>\n" );

    // Single-file compile (Ctrl+F7). Unconditional so the command is available
    // regardless of active configuration.
    fprintf( f, "  <ItemDefinitionGroup>\n" );
    fprintf( f, "    <NMakeCompile>\n" );
    fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; %s -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, build_name, mono_flag );
    fprintf( f, "    </NMakeCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    emit_intellisense_config_groups( f, target );

    gen_emit_debug_property_groups( f, target, false );
}

/*==============================================================================================
    build_gen_proj_target()

    Emit one .vcxproj + matching .vcxproj.filters for a specific engine target.
    The vcxproj's <ClCompile> entry is the target's unity TU. Non-unity .c files
    are emitted as <ClInclude> (same as CMake) so IntelliSense context flows from
    the unity TU rather than per-file overrides. Per-file <ClCompile ExcludedFromBuild>
    with AdditionalOptions bypasses NMakeAdditionalOptions (/TC /std:c11) and causes
    the EDG parser to reject designated initializers and compound literals. Pure
    header files are also <ClInclude> for navigation.
==============================================================================================*/

static void
build_gen_proj_target( target_info_t* target )
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

    // Alias launchers: no sources of their own; NMakeOutput needs the aliased artifact type.
    const target_info_t* aliased = target->alias_for ? find_target( target->alias_for ) : NULL;
    target_type_t        type    = aliased ? aliased->type : target->type;

    write_vcxproj_common_header( f, guid, type, target );

    gen_scan_reset();

    if ( target->root_dir )
        scan_directory_recursive( target->root_dir, target->root_dir );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        if ( g_files[ i ].is_natvis )
            continue;    // emitted in a separate Natvis ItemGroup below

        bool is_unit = is_unit_file( target, g_files[ i ].path );

        char inc[ PATH_MAX + 32 ];
        gen_inc_path( g_files[ i ].path, inc, sizeof( inc ) );
        if ( is_unit )
        {
            const char* item_mono = s_ctx.is_monolithic ? " -monolithic" : "";
            fprintf( f, "    <ClCompile Include=\"%s\">\n", inc );
            fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; %s -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
                     s_ctx.cd_root, s_ctx.build_tool_exe, target->name, item_mono );
            fprintf( f, "    </ClCompile>\n" );
        }
        else
        {
            // All non-unity files (both .c and .h) are <ClInclude>. IntelliSense
            // context flows from the unity TU, inheriting NMakeAdditionalOptions
            // (/TC /std:c11 /Zc:preprocessor) without per-file overrides that
            // bypass the C mode and break designated initializers.
            fprintf( f, "    <ClInclude Include=\"%s\" />\n", inc );
        }
    }

    // For has_reflect targets, list the reflection-generated files explicitly.
    // The .c is a compile unit; the .h is a header for F12 navigation.
    // Both live in <build_dir>\<gen_dir>\ and are not under root_dir, so the
    // directory scan above misses them. They may not exist until the first build.
    if ( target->has_reflect )
    {
        const char* rname     = target_reflect_name( target );
        const char* item_mono = s_ctx.is_monolithic ? " -monolithic" : "";
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\">\n", s_ctx.root_prefix, g_build_dir,
                 g_gen_dir, rname );
        fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; %s -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
                 s_ctx.cd_root, s_ctx.build_tool_exe, target->name, item_mono );
        fprintf( f, "    </ClCompile>\n" );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\" />\n", s_ctx.root_prefix, g_build_dir,
                 g_gen_dir, rname );
    }

    fprintf( f, "  </ItemGroup>\n" );

    write_natvis_item_group( f );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // .filters file mirrors the folder structure in Solution Explorer.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s.vcxproj.filters", s_ctx.out_dir, target->name );
    write_vcxproj_filters_file( filters_path, target );
}

/*==============================================================================================

    build_gen_projects()

    Top-level entry point invoked by `build_tool.exe -gen`. Regenerates every Makefile
    .vcxproj and every .sln from the current target/solution registry.

    Safe to re-run anytime; the generated XML is fully deterministic given the
    registry contents. VS user state survives regen as long as target names
    don't change (see guid_from_name).

==============================================================================================*/

void
build_gen_projects( const gen_manifest_t* m )
{
    snprintf( s_ctx.build_tool_exe, sizeof( s_ctx.build_tool_exe ), "%s", m->build_tool_exe );
    run_solution_passes( m, "_nm", "", "Solution", build_gen_proj_target );
}

/*============================================================================================*/
