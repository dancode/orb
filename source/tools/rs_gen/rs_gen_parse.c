/*==============================================================================================

    rs_gen_parse.c - field/body parsers and top-level file dispatcher

    Hand-written recursive-descent declarator parser. No regex, no external libs.
    Lexer helpers live in rs_gen_lex.c; attribute parsing lives in rs_gen_attr.c.

    Field declarations supported (one declarator per RS_PROP):

        T        name;          -> base=T,  no mods
        T*       name;          -> base=T,  mods=[PTR]
        T**      name;          -> base=T,  mods=[PTR, PTR]
        T* const name;          -> base=T,  mods=[CONST_PTR]
        const T  name;          -> base=T,  base_const=1
        T        name[N];       -> base=T,  mods=[ARRAY],      aux=N
        T*       name[N];       -> base=T,  mods=[PTR, ARRAY], aux=N

    Multi-declarator statements (e.g. `float x, y, z;`) are NOT supported per
    RS_PROP; place one RS_PROP marker per field.

==============================================================================================*/

#include "rs_gen_internal.h"

/* mirror of rs.h modifier slot encoding (kept local so the tool has zero engine deps) */
#define RG_MOD_NONE     0
#define RG_MOD_PTR      1
#define RG_MOD_ARRAY    2
#define RG_MOD_SLOT( op, c ) ( ( ( op ) & 0x3 ) | ( ( ( c ) & 0x1 ) << 2 ) )

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
    uint8_t  slots[ 4 ]               = { 0, 0, 0, 0 };
    int      slot_count               = 0;
    uint16_t array_count              = 0;
    int      seen_star                = 0;

    while ( 1 )
    {
        p = skip_ws( p );
        if ( !*p || *p == ';' )
            break;
        if ( *p == ',' )
        {
            /* multi-declarator not supported; skip rest of statement */
            while ( *p && *p != ';' )
                p++;
            break;
        }
        if ( *p == '*' )
        {
            if ( slot_count < 4 )
                slots[ slot_count++ ] = (uint8_t)RG_MOD_SLOT( RG_MOD_PTR, 0 );
            seen_star = 1;
            p++;
            continue;
        }
        if ( is_ident_start( (unsigned char)*p ) )
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
                    slots[ slot_count - 1 ] = (uint8_t)( ( slots[ slot_count - 1 ] & 0x3 ) | ( 1 << 2 ) );
                }
                else
                    base_const = 1;
                p = np;
                continue;
            }
            if ( strcmp( tok, "volatile" ) == 0 || strcmp( tok, "restrict" ) == 0
                 || strcmp( tok, "_Atomic" ) == 0 || strcmp( tok, "struct" ) == 0
                 || strcmp( tok, "enum" ) == 0   || strcmp( tok, "union" ) == 0 )
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
                q        = skip_ws( q );
                uint32_t n   = 0;
                int      any = 0;
                while ( is_digit( (unsigned char)*q ) )
                {
                    n = n * 10 + (uint32_t)( *q - '0' );
                    q++;
                    any = 1;
                }
                if ( any )
                    array_count = (uint16_t)n;
                while ( *q && *q != ']' )
                    q++;
                if ( *q == ']' )
                    q++;
                p = q;
                if ( slot_count < 4 )
                    slots[ slot_count++ ] = (uint8_t)RG_MOD_SLOT( RG_MOD_ARRAY, 0 );
            }
            continue;
        }
        p++;   /* unknown character - skip */
    }

    if ( *p == ';' )
        p++;

    rg_str_copy( out->name, field_name, RG_MAX_NAME );
    rg_str_copy( out->base_type, base_type, RG_MAX_NAME );
    out->array_count = array_count;
    out->base_const  = (uint8_t)base_const;

    uint16_t mods = 0;
    for ( int i = 0; i < 4; i++ )
        mods = (uint16_t)( mods | ( (uint16_t)slots[ i ] << ( i * 4 ) ) );
    out->mods = mods;

    return p;
}

/*----------------------------------------------------------------------------------------------
    Body parsers
----------------------------------------------------------------------------------------------*/

/* Scan a RS_STRUCT body for RS_PROP markers and parse each field declaration.
   `body_start` points just after '{'; `body_end` points at the closing '}'. */
static void
parse_struct_body( const char* body_start, const char* body_end,
                   const char* buf_start, rg_decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end )
    {
        if ( !match_word( p, buf_start, "RS_PROP" ) )
        {
            p++;
            continue;
        }

        p += 7;
        p = skip_ws( p );

        char args[ 1024 ] = { 0 };
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof args );

        if ( type->field_count >= RG_MAX_FIELDS_PER_TYPE )
        {
            while ( p < body_end && *p && *p != ';' )
                p++;
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

/* Parse a RS_ENUM / RS_BITSET body. Extract `NAME [= VALUE]` pairs. */
static void
parse_enum_body( const char* body_start, const char* body_end, rg_decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end )
    {
        p = skip_ws( p );
        if ( p >= body_end || !is_ident_start( (unsigned char)*p ) )
        {
            p++;
            continue;
        }

        char name[ RG_MAX_NAME ];
        p = read_ident( p, name, RG_MAX_NAME );
        p = skip_ws( p );

        char value[ RG_MAX_NAME ] = { 0 };
        int  has_value            = 0;
        if ( *p == '=' )
        {
            p++;
            p      = skip_ws( p );
            int vn = 0, depth = 0;
            while ( p < body_end && *p && ( depth > 0 || ( *p != ',' && *p != '}' ) ) )
            {
                if ( *p == '(' )      depth++;
                else if ( *p == ')' ) depth--;
                if ( vn < RG_MAX_NAME - 1 )
                    value[ vn++ ] = *p;
                p++;
            }
            value[ vn ] = '\0';
            trim( value );
            has_value = value[ 0 ] != '\0';
        }

        while ( p < body_end && *p && *p != ',' && *p != '}' )
            p++;
        if ( *p == ',' )
            p++;

        if ( type->enum_count < RG_MAX_ENUMS_PER_TYPE && name[ 0 ] )
        {
            rg_enumerator_t* e = &type->enumerators[ type->enum_count++ ];
            rg_str_copy( e->name, name, RG_MAX_NAME );
            rg_str_copy( e->value_expr, value, RG_MAX_NAME );
            e->has_value = has_value;
        }
    }
}

/*----------------------------------------------------------------------------------------------
    Top-level marker dispatch
----------------------------------------------------------------------------------------------*/

static void
parse_buffer( const char* buf, rg_parse_data_t* out ){
    const char* p = buf;
    while ( *p )
    {
        int         kind       = -1;
        int         marker_len = 0;

        if ( match_word( p, buf, "RS_STRUCT" ) )
        {
            kind = RG_KIND_STRUCT; marker_len = 9;
        }
        else if ( match_word( p, buf, "RS_BITSET" ) )
        {
            kind = RG_KIND_BITSET; marker_len = 9;
        }
        else if ( match_word( p, buf, "RS_ENUM" ) )
        {
            kind = RG_KIND_ENUM; marker_len = 7;
        }

        if ( kind < 0 )
        {
            p++;
            continue;
        }

        p += marker_len;
        p = skip_ws( p );

        char args[ 1024 ] = { 0 };
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof args );

        if ( out->type_count >= RG_MAX_TYPES )
            continue;

        rg_decl_type_t* t = &out->types[ out->type_count ];
        memset( t, 0, sizeof *t );
        t->kind = kind;
        parse_attr_args( args, t->attrs, RG_MAX_ATTRS_PER_ITEM, &t->attr_count );

        /* Walk forward past optional 'typedef struct tag' preamble to the opening '{'. */
        p = skip_ws( p );
        while ( is_ident_start( (unsigned char)*p ) )
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
                while ( is_ident_start( (unsigned char)*p ) )
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
        while ( *p && *p != ';' )
            p++;
        if ( *p == ';' )
            p++;
    }
}

/*----------------------------------------------------------------------------------------------
    Public entry
----------------------------------------------------------------------------------------------*/

void
rg_parse( const rg_file_list_t* files, rg_parse_data_t* out )
{
    out->type_count   = 0;
    out->struct_count = 0;
    out->enum_count   = 0;
    out->header_count = 0;

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

        if ( size <= 0 || size >= (long)sizeof s_buf )
        {
            fclose( f );
            continue;
        }
        
        /* Read the whole file into the buffer, then parse it. */

        size_t nread  = fread( s_buf, 1, (size_t)size, f );
        fclose( f );
        s_buf[ nread ] = '\0';

        int before = out->type_count;
        parse_buffer( s_buf, out );
        if ( out->type_count > before && out->header_count < RG_MAX_FILES )
        {
            make_include_path( files->paths[ i ],
                               out->headers[ out->header_count ],
                               RG_MAX_PATH );
            out->header_count++;
        }
    }
}
