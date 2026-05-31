/*==============================================================================================

    reflect_tool_parse.c - field/body parsers and top-level file dispatcher

    Hand-written recursive-descent declarator parser. No regex, no external libs.
    Lexer helpers live in reflect_tool_lex.c; attribute parsing lives in reflect_tool_attr.c.

    Field declarations supported (one declarator per REF_PROP):

        T        name;          -> base = T,  mods = REF_MODS_VALUE
        T*       name;          -> base = T,  mods = REF_MODS_PTR
        T**      name;          -> base = T,  mods = REF_MODS_PTR_PTR
        T* const name;          -> base = T,  mods = REF_MODS_CONST_PTR
        const T  name;          -> base = T,  mods = REF_MODS_CONST_VALUE
        const T* name;          -> base = T,  mods = REF_MODS_PTR_TO_CONST
        T        name[N];       -> base = T,  mods = REF_MODS_ARRAY,      aux = N
        T*       name[N];       -> base = T,  mods = REF_MODS_PTR_ARRAY,  aux = N

    Multi-declarator statements (e.g. `float x, y, z;`) are NOT supported per
    REF_PROP; place one REF_PROP marker per field.

==============================================================================================*/

#include "reflect_tool_internal.h"

/* mirror of REF_MODS_t bit encoding (kept local so the tool has zero engine deps) */

#define RT_MOD_NONE        0x00
#define RT_MOD_PTR         0x01
#define RT_MOD_ARRAY       0x02
#define RT_MOD_CONST       0x08   /* const on the pointer itself (T* const) */
#define RT_MOD_CONST_BASE  0x10   /* const on the base type (const T, const T*) */

// clang-format off
/*----------------------------------------------------------------------------------------------
    Field declaration parser
----------------------------------------------------------------------------------------------*/

/* Parse a single field declaration starting just past the closing paren of REF_PROP(...).
   Stops at the terminating ';'. Returns position after ';' (or '\0'). */

static const char*
parse_field_decl( const char* p, decl_field_t* out )
{
    int      base_const                = 0;
    char     base_type[ RT_MAX_NAME ]  = { 0 };
    int      have_base                 = 0;
    char     field_name[ RT_MAX_NAME ] = { 0 };
    uint8_t  slots[ 2 ]                = { 0, 0 };
    int      slot_count                = 0;
    uint16_t array_count               = 0;
    int      seen_star                 = 0;

    while ( 1 )
    {
        p = skip_ws( p );
        if ( !*p || *p == ';' )
            break;
        if ( *p == ',' )
        {
            /* multi-declarator not supported; skip rest of statement */
            while ( *p && *p != ';' ) p++;
            break;
        }
        if ( *p == '*' )
        {
            if ( slot_count < 2 )
                slots[ slot_count++ ] = RT_MOD_PTR;
            seen_star = 1;
            p++;
            continue;
        }
        if ( is_ident_start( ( unsigned char )*p ) )
        {
            char        tok[ RT_MAX_NAME ];
            const char* np = read_ident( p, tok, RT_MAX_NAME );

            if ( strcmp( tok, "const" ) == 0 )
            {
                if ( !have_base )
                    base_const = 1;
                else if ( seen_star && slot_count > 0 )
                {
                    /* T* const -> set CONST bit on the last PTR slot */
                    slots[ slot_count - 1 ] |= RT_MOD_CONST;
                }
                else
                    base_const = 1;
                p = np;
                continue;
            }
            if ( strcmp( tok, "volatile" ) == 0 || strcmp( tok, "restrict" ) == 0 || strcmp( tok, "_Atomic" ) == 0 ||
                 strcmp( tok, "struct" ) == 0 || strcmp( tok, "enum" ) == 0 || strcmp( tok, "union" ) == 0 )
            {
                p = np;
                continue;
            }
            if ( strcmp( tok, "unsigned" ) == 0 || strcmp( tok, "signed" ) == 0 )
            {
                if ( !have_base )
                {
                    str_copy( base_type, tok, RT_MAX_NAME );
                    have_base = 1;
                }
                p = np;
                continue;
            }

            if ( !have_base )
            {
                str_copy( base_type, tok, RT_MAX_NAME );
                have_base = 1;
                p         = np;
                continue;
            }

            /* second identifier = field name */
            str_copy( field_name, tok, RT_MAX_NAME );
            p = np;

            /* optional [N] */
            const char* q = skip_ws( p );
            if ( *q == '[' )
            {
                q++;
                q            = skip_ws( q );
                uint32_t n   = 0;
                int      any = 0;
                while ( is_digit( ( unsigned char )*q ) )
                {
                    n = n * 10 + ( uint32_t )( *q - '0' );
                    q++;
                    any = 1;
                }
                if ( any )
                    array_count = ( uint16_t )n;
                while ( *q && *q != ']' ) q++;
                if ( *q == ']' )
                    q++;
                p = q;
                if ( slot_count < 2 )
                    slots[ slot_count++ ] = RT_MOD_ARRAY;
            }
            continue;
        }
        p++; /* unknown character - skip */
    }

    if ( *p == ';' )
        p++;

    str_copy( out->name, field_name, RT_MAX_NAME );
    str_copy( out->base_type, base_type, RT_MAX_NAME );
    out->array_count = array_count;

    if ( base_const )
        slots[ 0 ] |= RT_MOD_CONST_BASE;
    out->mods = ( uint16_t )( slots[ 0 ] | ( ( uint16_t )slots[ 1 ] << 8 ) );

    return p;
}

/*----------------------------------------------------------------------------------------------
    REF_API() function signature parser
----------------------------------------------------------------------------------------------*/

/* Parse a function signature that immediately follows REF_API().
   On entry p points to the first character past the REF_API() closing paren (whitespace
   already skipped by the caller).  Returns a pointer past the closing ')' of the param list. */

static const char*
parse_api_func( const char* p, module_api_t* mod )
{
    if ( mod->func_count >= RT_MAX_API_FUNCS )
        return p;

    p = skip_ws( p );

    /* skip storage-class / linkage keywords before the return type */
    for ( ;; )
    {
        if ( !is_ident_start( (unsigned char)*p ) )
            break;
        char        tok[ RT_MAX_NAME ];
        const char* np = read_ident( p, tok, RT_MAX_NAME );
        if ( strcmp( tok, "static"        ) == 0 ||
             strcmp( tok, "extern"        ) == 0 ||
             strcmp( tok, "inline"        ) == 0 ||
             strcmp( tok, "__inline"      ) == 0 ||
             strcmp( tok, "__forceinline" ) == 0 ||
             strcmp( tok, "__cdecl"       ) == 0 ||
             strcmp( tok, "__stdcall"     ) == 0 )
        {
            p = skip_ws( np );
        }
        else
        {
            break;
        }
    }

    const char* sig_start = p;

    /* find the opening '(' of the parameter list */
    const char* paren = NULL;
    for ( const char* q = p; *q && *q != ';' && *q != '{'; q++ )
    {
        if ( *q == '(' ) { paren = q; break; }
    }
    if ( !paren )
        return p;

    /* copy everything before '(' to extract "ret_type func_name" */
    char pre[ RT_MAX_NAME * 4 ] = { 0 };
    int  pre_len = (int)( paren - sig_start );
    if ( pre_len <= 0 || pre_len >= (int)sizeof( pre ) )
        return p;
    memcpy( pre, sig_start, (size_t)pre_len );
    pre[ pre_len ] = '\0';
    trim( pre );

    /* walk backwards to isolate the function name (the last identifier) */
    int end = str_len( pre );
    while ( end > 0 && !is_ident_char( (unsigned char)pre[ end - 1 ] ) ) end--;
    int name_end = end;
    while ( end > 0 && is_ident_char( (unsigned char)pre[ end - 1 ] ) ) end--;
    int name_start = end;

    if ( name_start >= name_end )
        return p;

    char func_name[ RT_MAX_NAME ] = { 0 };
    {
        int len = name_end - name_start;
        if ( len >= RT_MAX_NAME ) len = RT_MAX_NAME - 1;
        memcpy( func_name, pre + name_start, (size_t)len );
        func_name[ len ] = '\0';
    }

    char ret_type[ RT_MAX_NAME * 2 ] = { 0 };
    {
        if ( name_start > 0 && name_start < (int)sizeof( ret_type ) )
        {
            memcpy( ret_type, pre, (size_t)name_start );
            ret_type[ name_start ] = '\0';
        }
        trim( ret_type );
    }

    /* read the parameter list */
    char        params[ RT_MAX_PARAM_STR ] = { 0 };
    const char* after = read_paren_block( paren, params, RT_MAX_PARAM_STR );
    trim( params );

    /* derive field_name by stripping the "<module>_" prefix */
    char field_name[ RT_MAX_NAME ] = { 0 };
    if ( mod->name[ 0 ] )
    {
        int mlen = str_len( mod->name );
        if ( strncmp( func_name, mod->name, (size_t)mlen ) == 0 && func_name[ mlen ] == '_' )
            str_copy( field_name, func_name + mlen + 1, RT_MAX_NAME );
    }
    if ( !field_name[ 0 ] )
        str_copy( field_name, func_name, RT_MAX_NAME );

    /* store */
    api_func_t* f = &mod->funcs[ mod->func_count++ ];
    memset( f, 0, sizeof *f );
    str_copy( f->ret_type,   ret_type,   sizeof f->ret_type );
    str_copy( f->name,       func_name,  RT_MAX_NAME );
    str_copy( f->field_name, field_name, RT_MAX_NAME );
    str_copy( f->params,     params,     RT_MAX_PARAM_STR );

    return after;
}

/*----------------------------------------------------------------------------------------------
    Body parsers
----------------------------------------------------------------------------------------------*/

static void
parse_struct_body( const char* body_start, const char* body_end, const char* buf_start, decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end && ( p = (const char*)memchr( p, 'R', (size_t)( body_end - p ) ) ) != NULL )
    {
        if ( !match_word( p, buf_start, "REF_PROP" ) ) { p++; continue; }
        p += (int)( sizeof( "REF_PROP" ) - 1 );
        p = skip_ws( p );

        char args[ 1024 ] = { 0 };
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof args );

        if ( type->field_count >= RT_MAX_FIELDS_PER_TYPE )
        {
            while ( p < body_end && *p && *p != ';' ) p++;
            if ( *p == ';' )
                p++;
            continue;
        }

        decl_field_t* f = &type->fields[ type->field_count ];
        memset( f, 0, sizeof *f );
        parse_attr_args( args, f->attrs, RT_MAX_ATTRS_PER_ITEM, &f->attr_count );
        p = parse_field_decl( p, f );

        if ( f->name[ 0 ] && f->base_type[ 0 ] )
            type->field_count++;
    }
}

/*--------------------------------------------------------------------------------------------*/
/* Parse a REF_ENUM / REF_BITSET body. Extract `NAME [= VALUE]` pairs.                          */

static void
parse_enum_body( const char* body_start, const char* body_end, decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end )
    {
        p = skip_ws( p );
        if ( p >= body_end || !is_ident_start( ( unsigned char )*p ) )
        {
            p++;
            continue;
        }

        char name[ RT_MAX_NAME ];
        p                         = read_ident( p, name, RT_MAX_NAME );
        p                         = skip_ws( p );

        char value[ RT_MAX_NAME ] = { 0 };
        int  has_value            = 0;
        if ( *p == '=' )
        {
            p++;
            p      = skip_ws( p );
            int vn = 0, depth = 0;
            while ( p < body_end && *p && ( depth > 0 || ( *p != ',' && *p != '}' ) ) )
            {
                if ( *p == '(' )
                    depth++;
                else if ( *p == ')' )
                    depth--;
                if ( vn < RT_MAX_NAME - 1 )
                    value[ vn++ ] = *p;
                p++;
            }
            value[ vn ] = '\0';
            trim( value );
            has_value = value[ 0 ] != '\0';
        }

        while ( p < body_end && *p && *p != ',' && *p != '}' ) p++;
        if ( *p == ',' )
            p++;

        if ( type->enum_count < RT_MAX_ENUMS_PER_TYPE && name[ 0 ] )
        {
            enum_t* e = &type->enums[ type->enum_count++ ];
            str_copy( e->name, name, RT_MAX_NAME );
            str_copy( e->value_expr, value, RT_MAX_NAME );
            e->has_value = has_value;
        }
    }
}

/*----------------------------------------------------------------------------------------------
    Top-level marker dispatch
----------------------------------------------------------------------------------------------*/

/* api_only: when non-zero, only REF_MODULE and REF_API markers are processed.
   Used for .c file passes to avoid double-counting REF_STRUCT/REF_ENUM types
   that also appear in the module's .h through #include. */
static void
parse_buffer( const char* buf, parse_data_t* out, int api_only )
{
    const char* p = buf;
    while ( ( p = strchr( p, 'R' ) ) != NULL )
    {
        int kind = -1;

        if      ( !api_only && match_word( p, buf, "REF_STRUCT" ) ) kind = RT_KIND_STRUCT;
        else if ( !api_only && match_word( p, buf, "REF_BITSET" ) ) kind = RT_KIND_BITSET;
        else if ( !api_only && match_word( p, buf, "REF_UNION"  ) ) kind = RT_KIND_UNION;
        else if ( !api_only && match_word( p, buf, "REF_ENUM"   ) ) kind = RT_KIND_ENUM;
        else if ( match_word( p, buf, "REF_MODULE" ) )
        {
            p += (int)( sizeof( "REF_MODULE" ) - 1 );
            p = skip_ws( p );
            char mod_name[ RT_MAX_NAME ] = { 0 };
            if ( *p == '(' )
                p = read_paren_block( p, mod_name, RT_MAX_NAME );
            trim( mod_name );
            if ( mod_name[ 0 ] )
            {
                str_copy( out->module_api.name, mod_name, RT_MAX_NAME );
                out->module_api.has_module = 1;
            }
            continue;
        }
        else if ( match_word( p, buf, "REF_API" ) )
        {
            p += (int)( sizeof( "REF_API" ) - 1 );
            p = skip_ws( p );
            char dummy[ 16 ] = { 0 };
            if ( *p == '(' )
                p = read_paren_block( p, dummy, sizeof dummy );
            p = skip_ws( p );
            p = parse_api_func( p, &out->module_api );
            continue;
        }

        if ( kind < 0 ) { p++; continue; }

        p += ( kind == RT_KIND_ENUM  ) ? (int)( sizeof( "REF_ENUM"   ) - 1 )
           : ( kind == RT_KIND_UNION ) ? (int)( sizeof( "REF_UNION"  ) - 1 )
           :                            (int)( sizeof( "REF_STRUCT" ) - 1 );
        p  = skip_ws( p );

        /* optional attribute args in parentheses, e.g. REF_STRUCT( my_attr="value", other_attr=123 ) */

        char args[ 1024 ] = { 0 };
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof( args ));

        if ( out->type_count >= RT_MAX_TYPES )
            continue;

        decl_type_t* t = &out->types[ out->type_count ];
        memset( t, 0, sizeof *t );
        t->kind = kind;
        parse_attr_args( args, t->attrs, RT_MAX_ATTRS_PER_ITEM, &t->attr_count );

        /* Walk past optional 'typedef struct tag' preamble to '{'.
           REF_ENUM(); on its own line emits a trailing ';' we also skip. */
        p = skip_ws( p );
        if ( *p == ';' ) { p++; p = skip_ws( p ); }
        while ( is_ident_start( ( unsigned char )*p ) )
        {
            char tmp[ RT_MAX_NAME ];
            p = read_ident( p, tmp, RT_MAX_NAME );
            p = skip_ws( p );
            if ( *p == '{' )
                break;
            /* allow `: underlying_type` (rare in C enums) */
            if ( *p == ':' )
            {
                p++;
                p = skip_ws( p );
                while ( is_ident_start( ( unsigned char )*p ) )
                {
                    char tmp2[ RT_MAX_NAME ];
                    p = read_ident( p, tmp2, RT_MAX_NAME );
                    p = skip_ws( p );
                }
                if ( *p == '{' )
                    break;
            }
        }

        if ( *p != '{' )
            continue;

        const char* body_open  = p;
        const char* body_close = find_close_brace( p );
        if ( body_close == body_open )
            continue;

        if ( !read_typedef_name( body_close, t->name, RT_MAX_NAME ) )
            continue;

        const char* body_start = body_open + 1;
        const char* body_end   = body_close;

        if ( kind == RT_KIND_STRUCT || kind == RT_KIND_UNION )
        {
            parse_struct_body( body_start, body_end, buf, t );
            out->struct_count++;
        }
        else
        {
            parse_enum_body( body_start, body_end, t );
            out->enum_count++;
        }
        out->type_count++;

        p = body_close + 1;
        while ( *p && *p != ';' ) p++;
        if ( *p == ';' )
            p++;
    }
}

// clang-format on

/*----------------------------------------------------------------------------------------------
    File loader and per-file parse helpers
----------------------------------------------------------------------------------------------*/

/* Opens path, reads up to max-1 bytes into buf, null-terminates, returns bytes read.
   Returns 0 on failure or if file is empty / exceeds buffer. */
static size_t
load_file( const char* path, char* buf, size_t max )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return 0;
    fseek( f, 0, SEEK_END );
    long size = ftell( f );
    fseek( f, 0, SEEK_SET );
    if ( size <= 0 || ( size_t )size >= max )
    {
        fclose( f );
        return 0;
    }
    size_t n  = fread( buf, 1, ( size_t )size, f );
    fclose( f );
    buf[ n ] = '\0';
    return n;
}

/*----------------------------------------------------------------------------------------------
    Full parse pass (.h): collects types + API; 
    records path in parse data->headers[] when something is found.
----------------------------------------------------------------------------------------------*/

static void
parse_file_headers( const char* path, parse_data_t* out, char* buf, size_t buf_max )
{
    if ( !load_file( path, buf, buf_max ) )
        return;

    strip_comments( buf );

    int before_types = out->type_count;
    int before_funcs = out->module_api.func_count;

    parse_buffer( buf, out, 0 );

    if ( ( out->type_count > before_types || out->module_api.func_count > before_funcs )
         && out->header_count < RT_MAX_FILES )
    {
        make_include_path( path, out->headers[ out->header_count++ ], RT_MAX_PATH );
    }
}

/* API-only parse pass (.c): collects REF_MODULE/REF_API and marks found funcs as C-sourced. */
static void
parse_file_api( const char* path, parse_data_t* out, char* buf, size_t buf_max )
{
    if ( !load_file( path, buf, buf_max ) )
        return;
    strip_comments( buf );

    int before_funcs = out->module_api.func_count;
    parse_buffer( buf, out, 1 );
    for ( int fi = before_funcs; fi < out->module_api.func_count; fi++ )
        out->module_api.funcs[ fi ].src_is_c = 1;
}

/*----------------------------------------------------------------------------------------------
    Public entry
----------------------------------------------------------------------------------------------*/

void
parse( const file_list_t* files, parse_data_t* out )
{
    out->type_count            = 0;
    out->struct_count          = 0;
    out->enum_count            = 0;
    out->header_count          = 0;
    out->module_api.has_module = 0;
    out->module_api.func_count = 0;
    out->module_api.name[ 0 ]  = '\0';

    static char s_buf[ 8 * 1024 * 1024 + 1 ];

    /* Header pass: types + API from .h files collected by scan. */
    for ( int i = 0; i < files->count; i++ )
        parse_file_headers( files->paths[ i ], out, s_buf, sizeof s_buf );

    /* Source pass: API-only from .c files; found functions get forward-decls in the generated .c. */
    static char s_c_paths[ RT_MAX_FILES ][ RT_MAX_PATH ];
    int c_count = platform_scan_dir( files->source_dir, s_c_paths, RT_MAX_FILES );
    for ( int i = 0; i < c_count; i++ )
        if ( str_ends_with( s_c_paths[ i ], ".c" ) )
            parse_file_api( s_c_paths[ i ], out, s_buf, sizeof s_buf );
}

/*--------------------------------------------------------------------------------------------*/