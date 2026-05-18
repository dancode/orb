/*==============================================================================================

    rs_gen_parse.c - field/body parsers and top-level file dispatcher

    Hand-written recursive-descent declarator parser. No regex, no external libs.
    Lexer helpers live in rs_gen_lex.c; attribute parsing lives in rs_gen_attr.c.

    Field declarations supported (one declarator per RS_PROP):

        T        name;          -> base = T,  no mods
        T*       name;          -> base = T,  mods = [PTR]
        T**      name;          -> base = T,  mods = [PTR, PTR]
        T* const name;          -> base = T,  mods = [CONST_PTR]
        const T  name;          -> base = T,  base_const = 1
        T        name[N];       -> base = T,  mods = [ARRAY],      aux = N
        T*       name[N];       -> base = T,  mods = [PTR, ARRAY], aux = N

    Multi-declarator statements (e.g. `float x, y, z;`) are NOT supported per
    RS_PROP; place one RS_PROP marker per field.

==============================================================================================*/

#include "rs_gen_internal.h"

/* mirror of rs.h modifier slot encoding (kept local so the tool has zero engine deps) */

#define RG_MOD_NONE          0
#define RG_MOD_PTR           1
#define RG_MOD_ARRAY         2
#define RG_MOD_SLOT( op, c ) ( ( ( op ) & 0x3 ) | ( ( ( c ) & 0x1 ) << 2 ) )

// clang-format off
/*----------------------------------------------------------------------------------------------
    Field declaration parser
----------------------------------------------------------------------------------------------*/

/* Parse a single field declaration starting just past the closing paren of RS_PROP(...).
   Stops at the terminating ';'. Returns position after ';' (or '\0'). */

static const char*
parse_field_decl( const char* p, rg_decl_field_t* out )
{
    int      base_const                = 0;
    char     base_type[ RG_MAX_NAME ]  = { 0 };
    int      have_base                 = 0;
    char     field_name[ RG_MAX_NAME ] = { 0 };
    uint8_t  slots[ 4 ]                = { 0, 0, 0, 0 };
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
            if ( slot_count < 4 )
                slots[ slot_count++ ] = ( uint8_t )RG_MOD_SLOT( RG_MOD_PTR, 0 );
            seen_star = 1;
            p++;
            continue;
        }
        if ( is_ident_start( ( unsigned char )*p ) )
        {
            char        tok[ RG_MAX_NAME ];
            const char* np = read_ident( p, tok, RG_MAX_NAME );

            if ( strcmp( tok, "const" ) == 0 )
            {
                if ( !have_base )
                    base_const = 1;
                else if ( seen_star && slot_count > 0 )
                {
                    /* T* const -> mark last PTR slot as const-qualified */
                    slots[ slot_count - 1 ] = ( uint8_t )( ( slots[ slot_count - 1 ] & 0x3 ) | ( 1 << 2 ) );
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
                    rg_str_copy( base_type, tok, RG_MAX_NAME );
                    have_base = 1;
                }
                p = np;
                continue;
            }

            if ( !have_base )
            {
                rg_str_copy( base_type, tok, RG_MAX_NAME );
                have_base = 1;
                p         = np;
                continue;
            }

            /* second identifier = field name */
            rg_str_copy( field_name, tok, RG_MAX_NAME );
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
                if ( slot_count < 4 )
                    slots[ slot_count++ ] = ( uint8_t )RG_MOD_SLOT( RG_MOD_ARRAY, 0 );
            }
            continue;
        }
        p++; /* unknown character - skip */
    }

    if ( *p == ';' )
        p++;

    rg_str_copy( out->name, field_name, RG_MAX_NAME );
    rg_str_copy( out->base_type, base_type, RG_MAX_NAME );
    out->array_count = array_count;
    out->base_const  = ( uint8_t )base_const;

    uint16_t mods    = 0;
    for ( int i = 0; i < 4; i++ ) mods = ( uint16_t )( mods | ( ( uint16_t )slots[ i ] << ( i * 4 ) ) );
    out->mods = mods;

    return p;
}

/*----------------------------------------------------------------------------------------------
    RS_API() function signature parser
----------------------------------------------------------------------------------------------*/

/* Parse a function signature that immediately follows RS_API().
   On entry p points to the first character past the RS_API() closing paren (whitespace
   already skipped by the caller).  Returns a pointer past the closing ')' of the param list. */

static const char*
parse_api_func( const char* p, rg_module_api_t* mod )
{
    if ( mod->func_count >= RG_MAX_API_FUNCS )
        return p;

    p = skip_ws( p );

    /* skip storage-class / linkage keywords before the return type */
    for ( ;; )
    {
        if ( !is_ident_start( (unsigned char)*p ) )
            break;
        char        tok[ RG_MAX_NAME ];
        const char* np = read_ident( p, tok, RG_MAX_NAME );
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
    char pre[ RG_MAX_NAME * 4 ] = { 0 };
    int  pre_len = (int)( paren - sig_start );
    if ( pre_len <= 0 || pre_len >= (int)sizeof( pre ) )
        return p;
    memcpy( pre, sig_start, (size_t)pre_len );
    pre[ pre_len ] = '\0';
    trim( pre );

    /* walk backwards to isolate the function name (the last identifier) */
    int end = rg_str_len( pre );
    while ( end > 0 && !is_ident_char( (unsigned char)pre[ end - 1 ] ) ) end--;
    int name_end = end;
    while ( end > 0 && is_ident_char( (unsigned char)pre[ end - 1 ] ) ) end--;
    int name_start = end;

    if ( name_start >= name_end )
        return p;

    char func_name[ RG_MAX_NAME ] = { 0 };
    {
        int len = name_end - name_start;
        if ( len >= RG_MAX_NAME ) len = RG_MAX_NAME - 1;
        memcpy( func_name, pre + name_start, (size_t)len );
        func_name[ len ] = '\0';
    }

    char ret_type[ RG_MAX_NAME * 2 ] = { 0 };
    {
        if ( name_start > 0 && name_start < (int)sizeof( ret_type ) )
        {
            memcpy( ret_type, pre, (size_t)name_start );
            ret_type[ name_start ] = '\0';
        }
        trim( ret_type );
    }

    /* read the parameter list */
    char        params[ RG_MAX_PARAM_STR ] = { 0 };
    const char* after = read_paren_block( paren, params, RG_MAX_PARAM_STR );
    trim( params );

    /* derive field_name by stripping the "<module>_" prefix */
    char field_name[ RG_MAX_NAME ] = { 0 };
    if ( mod->name[ 0 ] )
    {
        int mlen = rg_str_len( mod->name );
        if ( strncmp( func_name, mod->name, (size_t)mlen ) == 0 && func_name[ mlen ] == '_' )
            rg_str_copy( field_name, func_name + mlen + 1, RG_MAX_NAME );
    }
    if ( !field_name[ 0 ] )
        rg_str_copy( field_name, func_name, RG_MAX_NAME );

    /* store */
    rg_api_func_t* f = &mod->funcs[ mod->func_count++ ];
    memset( f, 0, sizeof *f );
    rg_str_copy( f->ret_type,   ret_type,   sizeof f->ret_type );
    rg_str_copy( f->name,       func_name,  RG_MAX_NAME );
    rg_str_copy( f->field_name, field_name, RG_MAX_NAME );
    rg_str_copy( f->params,     params,     RG_MAX_PARAM_STR );

    return after;
}

/*----------------------------------------------------------------------------------------------
    Body parsers
----------------------------------------------------------------------------------------------*/

/* Scan a RS_STRUCT body for RS_PROP markers and parse each field declaration.
   `body_start` points just after '{'; `body_end` points at the closing '}'. */

static void
parse_struct_body( const char* body_start, const char* body_end, const char* buf_start, rg_decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end && ( p = (const char*)memchr( p, 'R', (size_t)( body_end - p ) ) ) != NULL )
    {
        if ( p[ 1 ] != 'S' || p[ 2 ] != '_' )                               { p++; continue; }
        if ( p > buf_start && is_ident_char( (unsigned char)p[ -1 ] ) )      { p++; continue; }
        if ( strncmp( p + 3, "PROP", 4 ) != 0 || is_ident_char( (unsigned char)p[ 7 ] ) ) { p++; continue; }

        p += 7;
        p = skip_ws( p );

        char args[ 1024 ] = { 0 };
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof args );

        if ( type->field_count >= RG_MAX_FIELDS_PER_TYPE )
        {
            while ( p < body_end && *p && *p != ';' ) p++;
            if ( *p == ';' )
                p++;
            continue;
        }

        rg_decl_field_t* f = &type->fields[ type->field_count ];
        memset( f, 0, sizeof *f );
        parse_attr_args( args, f->attrs, RG_MAX_ATTRS_PER_ITEM, &f->attr_count );
        p = parse_field_decl( p, f );

        if ( f->name[ 0 ] && f->base_type[ 0 ] )
            type->field_count++;
    }
}

/*--------------------------------------------------------------------------------------------*/
/* Parse a RS_ENUM / RS_BITSET body. Extract `NAME [= VALUE]` pairs.                          */

static void
parse_enum_body( const char* body_start, const char* body_end, rg_decl_type_t* type )
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

        char name[ RG_MAX_NAME ];
        p                         = read_ident( p, name, RG_MAX_NAME );
        p                         = skip_ws( p );

        char value[ RG_MAX_NAME ] = { 0 };
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
                if ( vn < RG_MAX_NAME - 1 )
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

        if ( type->enum_count < RG_MAX_ENUMS_PER_TYPE && name[ 0 ] )
        {
            rg_enum_t* e = &type->enums[ type->enum_count++ ];
            rg_str_copy( e->name, name, RG_MAX_NAME );
            rg_str_copy( e->value_expr, value, RG_MAX_NAME );
            e->has_value = has_value;
        }
    }
}

/*----------------------------------------------------------------------------------------------
    Top-level marker dispatch
----------------------------------------------------------------------------------------------*/

/* api_only: when non-zero, only RS_MODULE and RS_API markers are processed.
   Used for .c file passes to avoid double-counting RS_STRUCT/RS_ENUM types
   that also appear in the module's .h through #include. */
static void
parse_buffer( const char* buf, rg_parse_data_t* out, int api_only )
{
    const char* p = buf;
    while ( ( p = strchr( p, 'R' ) ) != NULL )
    {
        /* pre-filter: all our markers begin with RS_ and skip everything else. */
        if ( p[ 1 ] != 'S' || p[ 2 ] != '_' ) { p++; continue; }

        /* check leading character before 'R' invalidaetes it PARSE, or xRS_, etc */
        if ( p > buf && is_ident_char( (unsigned char)p[ -1 ] ) ) { p++; continue; }

        const char* s = p + 3; /* skip RS_ */

        /* dispatch on the suffix; trailing boundary checked per match. */        

        int kind = -1;
        int marker_len = 0;

        if      ( !api_only && strncmp( s, "STRUCT", 6 ) == 0 && !is_ident_char( (unsigned char)s[ 6 ] ) ) { kind = RG_KIND_STRUCT; marker_len = 9; }
        else if ( !api_only && strncmp( s, "BITSET", 6 ) == 0 && !is_ident_char( (unsigned char)s[ 6 ] ) ) { kind = RG_KIND_BITSET; marker_len = 9; }
        else if ( !api_only && strncmp( s, "ENUM",   4 ) == 0 && !is_ident_char( (unsigned char)s[ 4 ] ) ) { kind = RG_KIND_ENUM;   marker_len = 7; }
        else if ( strncmp( s, "MODULE", 6 ) == 0 && !is_ident_char( (unsigned char)s[ 6 ] ) )
        {
            /* RS_MODULE( name ) — declares the module name for API generation */
            p += 9; /* advance past "RS_MODULE" */
            p = skip_ws( p );
            char mod_name[ RG_MAX_NAME ] = { 0 };
            if ( *p == '(' )
                p = read_paren_block( p, mod_name, RG_MAX_NAME );
            trim( mod_name );
            if ( mod_name[ 0 ] )
            {
                rg_str_copy( out->module_api.name, mod_name, RG_MAX_NAME );
                out->module_api.has_module = 1;
            }
            continue;
        }
        else if ( strncmp( s, "API", 3 ) == 0 && !is_ident_char( (unsigned char)s[ 3 ] ) )
        {
            /* RS_API() func_sig — adds a function to the generated module API struct */
            p += 6; /* advance past "RS_API" */
            p = skip_ws( p );
            char dummy[ 16 ] = { 0 };
            if ( *p == '(' )
                p = read_paren_block( p, dummy, sizeof dummy ); /* consume the () */
            p = skip_ws( p );
            p = parse_api_func( p, &out->module_api );
            continue;
        }

        if ( kind < 0 ) { p++; continue; } /* move to next 'R' and try again */

        p += marker_len;
        p  = skip_ws( p );

        /* optional attribute args in parentheses, e.g. RS_STRUCT( my_attr="value", other_attr=123 ) */

        char args[ 1024 ] = { 0 };
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof( args ));

        if ( out->type_count >= RG_MAX_TYPES )
            continue;

        /* Found a valid marker. Parse it and add to the output list. 
           We may still discard it later if we can't find a body or name,
           but we'll count it as a type for now to avoid overflowing 
           the output array with too many partial matches. */

        /* new type entry, zero-initialised except for kind and attrs filled from marker args. */

        rg_decl_type_t* t = &out->types[ out->type_count ];
        memset( t, 0, sizeof *t );
        t->kind = kind;

        parse_attr_args( args, t->attrs, RG_MAX_ATTRS_PER_ITEM, &t->attr_count );

        /* Walk forward past optional 'typedef struct tag' preamble to the opening '{'.
           Skip a trailing ';' so that RS_ENUM(); on its own line works correctly. */

        p = skip_ws( p );
        if ( *p == ';' ) { p++; p = skip_ws( p ); }
        while ( is_ident_start( ( unsigned char )*p ) )
        {
            char tmp[ RG_MAX_NAME ];
            p = read_ident( p, tmp, RG_MAX_NAME );
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
                    char tmp2[ RG_MAX_NAME ];
                    p = read_ident( p, tmp2, RG_MAX_NAME );
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

        if ( !read_typedef_name( body_close, t->name, RG_MAX_NAME ) )
            continue;

        const char* body_start = body_open + 1;
        const char* body_end   = body_close;

        if ( kind == RG_KIND_STRUCT )
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
    Public entry
----------------------------------------------------------------------------------------------*/

void
rg_parse( const rg_file_list_t* files, rg_parse_data_t* out )
{
    out->type_count            = 0;
    out->struct_count          = 0;
    out->enum_count            = 0;
    out->header_count          = 0;
    out->module_api.has_module = 0;
    out->module_api.func_count = 0;
    out->module_api.name[ 0 ]  = '\0';

    /* Single reusable read buffer - files are processed one at a time. */
    static char s_buf[ 8 * 1024 * 1024 + 1 ];

    for ( int i = 0; i < files->count; i++ )
    {
        FILE* f = fopen( files->paths[ i ], "rb" );
        if ( !f )
            continue;

        /* Determine file size by seeking to end, then back to start.
           If the file is empty or too large for our buffer, skip it. */

        fseek( f, 0, SEEK_END );
        long size = ftell( f );
        fseek( f, 0, SEEK_SET );

        if ( size <= 0 || size >= ( long )sizeof s_buf )
        {
            fclose( f );
            continue;
        }

        /* Read the whole file into the buffer, then parse it. */

        size_t nread = fread( s_buf, 1, ( size_t )size, f );
        fclose( f );
        s_buf[ nread ] = '\0';
        strip_comments( s_buf );

        /* parse_buffer may find multiple types in the same file, but we only add the file to the
           header list if at least one new type is found (to avoid spurious includes from files
           that only have forward declarations or attributes). */

        int before_types = out->type_count;        // RS_STRUCT/RS_ENUM count before parsing
        int before_funcs = out->module_api.func_count; // RS_API count before parsing

        parse_buffer( s_buf, out, 0 /* full pass: types + API */ );

        /* Add the file to the include list if any new types or API functions were found.
           This ensures the generated .c can see the declarations for both struct types
           and RS_API()-annotated functions. */

        int new_types = out->type_count > before_types;
        int new_funcs = out->module_api.func_count > before_funcs;

        if ( ( new_types || new_funcs ) && out->header_count < RG_MAX_FILES )
        {
            make_include_path( files->paths[ i ], out->headers[ out->header_count ], RG_MAX_PATH );
            out->header_count++;
        }
    }

    /* Second pass: scan .c files in the same directory for RS_MODULE / RS_API only.
       RS_STRUCT/RS_ENUM are skipped to avoid double-counting types that also appear
       in the .h files via #include.  Functions found here get forward declarations
       emitted into the generated .c so the struct initialiser can reference them. */

    static char s_c_paths[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int c_count = rg_platform_scan_dir( files->source_dir, s_c_paths, RG_MAX_FILES );

    for ( int i = 0; i < c_count; i++ )
    {
        if ( !rg_str_ends_with( s_c_paths[ i ], ".c" ) )
            continue;

        FILE* f = fopen( s_c_paths[ i ], "rb" );
        if ( !f )
            continue;

        fseek( f, 0, SEEK_END );
        long size = ftell( f );
        fseek( f, 0, SEEK_SET );

        if ( size <= 0 || size >= ( long )sizeof s_buf )
        {
            fclose( f );
            continue;
        }

        size_t nread = fread( s_buf, 1, ( size_t )size, f );
        fclose( f );
        s_buf[ nread ] = '\0';
        strip_comments( s_buf );

        int before_funcs = out->module_api.func_count;

        parse_buffer( s_buf, out, 1 /* api_only */ );

        /* Mark found functions as coming from a .c source so the output can emit
           forward declarations instead of relying on a header include. */

        for ( int fi = before_funcs; fi < out->module_api.func_count; fi++ )
            out->module_api.funcs[ fi ].src_is_c = 1;
    }
}

/*--------------------------------------------------------------------------------------------*/