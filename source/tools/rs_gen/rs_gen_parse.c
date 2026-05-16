/*==============================================================================================

    rs_gen_parse.c - parse annotated headers for RS_STRUCT / RS_ENUM / RS_BITSET / RS_PROP

    Hand-written recursive-descent declarator parser. No regex, no external libs.
    Whitespace, line-comments (//) and block-comments (/ * ... * /) are skipped.

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
#define RG_MOD_NONE         0
#define RG_MOD_PTR          1
#define RG_MOD_ARRAY        2
#define RG_MOD_FUNCTION     3
#define RG_MOD_SLOT( op, c ) ( ( ( op ) & 0x3 ) | ( ( ( c ) & 0x1 ) << 2 ) )

/*----------------------------------------------------------------------------------------------
    Character / token helpers
----------------------------------------------------------------------------------------------*/

static int
is_space( int c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int
is_digit( int c )
{
    return c >= '0' && c <= '9';
}

static int
is_ident_start( int c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}

static int
is_ident_char( int c )
{
    return is_ident_start( c ) || is_digit( c );
}

/* Advance past whitespace, // line comments, and / * block * / comments. */
static const char*
skip_ws( const char* p )
{
    for ( ;; )
    {
        while ( *p && is_space( (unsigned char)*p ) )
            p++;

        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            p += 2;
            while ( *p && *p != '\n' )
                p++;
            continue;
        }
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p += 2;
            while ( *p && !( p[ 0 ] == '*' && p[ 1 ] == '/' ) )
                p++;
            if ( *p )
                p += 2;
            continue;
        }
        break;
    }
    return p;
}

/* Match the literal `kw` at *p with a non-identifier boundary on both sides. */
static int
match_word( const char* p, const char* prev_start, const char* kw )
{
    int kl = rg_str_len( kw );
    if ( p > prev_start && is_ident_char( (unsigned char)p[ -1 ] ) )
        return 0;
    if ( strncmp( p, kw, (size_t)kl ) != 0 )
        return 0;
    if ( is_ident_char( (unsigned char)p[ kl ] ) )
        return 0;
    return 1;
}

/* Read an identifier into out (NUL-terminated). Returns advanced cursor. */
static const char*
read_ident( const char* p, char* out, int max )
{
    int n = 0;
    if ( !is_ident_start( (unsigned char)*p ) )
    {
        if ( max > 0 )
            out[ 0 ] = '\0';
        return p;
    }
    while ( is_ident_char( (unsigned char)*p ) && n < max - 1 )
        out[ n++ ] = *p++;
    out[ n ] = '\0';
    while ( is_ident_char( (unsigned char)*p ) )    /* drain overflow */
        p++;
    return p;
}

/* Parse `(...)` starting at the opening paren. Writes the inner text into out
   (without the parens), returns the position AFTER the closing paren. Handles
   nested parens and string literals. */
static const char*
read_paren_block( const char* p, char* out, int max )
{
    if ( *p != '(' )
    {
        if ( max > 0 )
            out[ 0 ] = '\0';
        return p;
    }
    p++;
    int depth = 1;
    int n     = 0;
    while ( *p && depth > 0 )
    {
        char c = *p;
        if ( c == '"' )
        {
            if ( n < max - 1 )
                out[ n++ ] = c;
            p++;
            while ( *p && *p != '"' )
            {
                if ( *p == '\\' && p[ 1 ] )
                {
                    if ( n < max - 1 )
                        out[ n++ ] = *p;
                    p++;
                }
                if ( n < max - 1 )
                    out[ n++ ] = *p;
                p++;
            }
            if ( *p )
            {
                if ( n < max - 1 )
                    out[ n++ ] = *p;
                p++;
            }
            continue;
        }
        if ( c == '(' )
            depth++;
        else if ( c == ')' )
        {
            depth--;
            if ( depth == 0 )
            {
                p++;
                break;
            }
        }
        if ( n < max - 1 )
            out[ n++ ] = c;
        p++;
    }
    out[ n < max ? n : max - 1 ] = '\0';
    return p;
}

/*----------------------------------------------------------------------------------------------
    Attribute argument parser

    Input:  contents between the parens of RS_STRUCT( ... ) / RS_PROP( ... ).
    Grammar (comma-separated entries; first entry sets the attribute name):

        entry     := IDENT [ '=' value ]
        value     := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL | IDENT
        continuation:  for `name=v1, v2, v3` the v2, v3 are emitted as repeated
                       entries under the same `name` (matches rs_attrib runs).
----------------------------------------------------------------------------------------------*/

static int
classify_literal( const char* s )
{
    /* s is already trimmed; '\0' terminated */
    if ( s[ 0 ] == '"' )
        return RG_ATTR_STRING;
    int has_digit = 0;
    int has_dot   = 0;
    int i         = 0;
    if ( s[ 0 ] == '-' || s[ 0 ] == '+' )
        i = 1;
    for ( ; s[ i ]; i++ )
    {
        char c = s[ i ];
        if ( c == '.' )
            has_dot = 1;
        else if ( is_digit( c ) )
            has_digit = 1;
        else if ( c == 'f' || c == 'F' )
            has_dot = 1;
        else
            return RG_ATTR_TAG;
    }
    if ( !has_digit )
        return RG_ATTR_TAG;
    return has_dot ? RG_ATTR_FLOAT : RG_ATTR_INT;
}

/* Extract content of a string literal (drop surrounding quotes; leave escapes raw). */
static void
unquote_string( char* s )
{
    int n = rg_str_len( s );
    if ( n >= 2 && s[ 0 ] == '"' && s[ n - 1 ] == '"' )
    {
        memmove( s, s + 1, (size_t)( n - 2 ) );
        s[ n - 2 ] = '\0';
    }
}

static void
trim( char* s )
{
    int n = rg_str_len( s );
    while ( n > 0 && is_space( (unsigned char)s[ n - 1 ] ) )
        s[ --n ] = '\0';
    int i = 0;
    while ( s[ i ] && is_space( (unsigned char)s[ i ] ) )
        i++;
    if ( i > 0 )
        memmove( s, s + i, (size_t)( rg_str_len( s + i ) + 1 ) );
}

static void
parse_attr_args( const char* args, rg_attr_t* out, int max, int* out_count )
{
    *out_count = 0;
    if ( !args || !*args )
        return;

    char current_name[ RG_MAX_NAME ] = { 0 };
    int  in_value                    = 0;
    char token[ RG_MAX_NAME ]        = { 0 };
    int  tn                          = 0;
    int  depth                       = 0;

    const char* p = args;
    while ( 1 )
    {
        char c = *p;
        if ( ( c == ',' || c == '\0' ) && depth == 0 )
        {
            token[ tn ] = '\0';
            trim( token );
            if ( token[ 0 ] )
            {
                if ( !in_value )
                {
                    /* tag attribute (no '=') */
                    if ( *out_count < max )
                    {
                        rg_attr_t* a = &out[ ( *out_count )++ ];
                        rg_str_copy( a->name, token, RG_MAX_NAME );
                        a->kind     = RG_ATTR_TAG;
                        a->value[ 0 ] = '\0';
                        rg_str_copy( current_name, token, RG_MAX_NAME );
                    }
                }
                else
                {
                    if ( *out_count < max )
                    {
                        rg_attr_t* a = &out[ ( *out_count )++ ];
                        rg_str_copy( a->name, current_name, RG_MAX_NAME );
                        a->kind = classify_literal( token );
                        rg_str_copy( a->value, token, RG_MAX_NAME );
                        if ( a->kind == RG_ATTR_STRING )
                            unquote_string( a->value );
                    }
                }
            }
            tn        = 0;
            token[ 0 ] = '\0';
            if ( c == '\0' )
                break;
            p++;
            /* keep in_value across commas so `range=0, 100` repeats `range` */
            continue;
        }
        if ( c == '=' && depth == 0 && !in_value )
        {
            token[ tn ] = '\0';
            trim( token );
            rg_str_copy( current_name, token, RG_MAX_NAME );
            if ( *out_count < max )
            {
                rg_attr_t* a = &out[ ( *out_count )++ ];
                rg_str_copy( a->name, current_name, RG_MAX_NAME );
                a->kind       = RG_ATTR_TAG;          /* placeholder, overwritten by RHS */
                a->value[ 0 ] = '\0';
                /* Mark that the placeholder name-only entry exists; the upcoming
                   value will be appended as a fresh entry, so drop the placeholder. */
                ( *out_count )--;
            }
            in_value  = 1;
            tn        = 0;
            token[ 0 ] = '\0';
            p++;
            continue;
        }
        if ( c == '(' || c == '[' || c == '{' )
            depth++;
        else if ( c == ')' || c == ']' || c == '}' )
            depth--;
        if ( c == '"' )
        {
            /* consume quoted string verbatim into token */
            if ( tn < RG_MAX_NAME - 1 )
                token[ tn++ ] = c;
            p++;
            while ( *p && *p != '"' )
            {
                if ( *p == '\\' && p[ 1 ] )
                {
                    if ( tn < RG_MAX_NAME - 1 )
                        token[ tn++ ] = *p;
                    p++;
                }
                if ( tn < RG_MAX_NAME - 1 )
                    token[ tn++ ] = *p;
                p++;
            }
            if ( *p == '"' )
            {
                if ( tn < RG_MAX_NAME - 1 )
                    token[ tn++ ] = *p;
                p++;
            }
            continue;
        }
        if ( tn < RG_MAX_NAME - 1 )
            token[ tn++ ] = c;
        p++;
    }
}

/*----------------------------------------------------------------------------------------------
    Skip a balanced brace block. p must point at the opening '{'. Returns position
    after the closing '}'. Skips strings, char literals, and nested braces.
----------------------------------------------------------------------------------------------*/

static const char*
skip_brace_block( const char* p )
{
    if ( *p != '{' )
        return p;
    p++;
    int depth = 1;
    while ( *p && depth > 0 )
    {
        if ( p[ 0 ] == '/' && p[ 1 ] == '/' )
        {
            while ( *p && *p != '\n' )
                p++;
            continue;
        }
        if ( p[ 0 ] == '/' && p[ 1 ] == '*' )
        {
            p += 2;
            while ( *p && !( p[ 0 ] == '*' && p[ 1 ] == '/' ) )
                p++;
            if ( *p )
                p += 2;
            continue;
        }
        if ( *p == '"' || *p == '\'' )
        {
            char q = *p++;
            while ( *p && *p != q )
            {
                if ( *p == '\\' && p[ 1 ] )
                    p++;
                p++;
            }
            if ( *p )
                p++;
            continue;
        }
        if ( *p == '{' )
            depth++;
        else if ( *p == '}' )
            depth--;
        p++;
    }
    return p;
}

/*----------------------------------------------------------------------------------------------
    Find matching closing brace of a body that starts with '{' at p.
    Returns pointer to the closing '}' (or end of buffer).
----------------------------------------------------------------------------------------------*/

static const char*
find_close_brace( const char* p )
{
    if ( *p != '{' )
        return p;
    const char* end = skip_brace_block( p );
    if ( end > p )
        return end - 1;
    return p;
}

/*----------------------------------------------------------------------------------------------
    After a struct/enum body `} TYPENAME;`, read the typedef name.
    `p` should point at the closing '}'.
----------------------------------------------------------------------------------------------*/

static int
read_typedef_name( const char* close_brace, char* out, int max )
{
    const char* p = close_brace;
    if ( *p == '}' )
        p++;
    p = skip_ws( p );
    /* Skip __attribute__((...)) etc. (any number of identifier(...) pairs). */
    while ( is_ident_start( (unsigned char)*p ) )
    {
        char tmp[ RG_MAX_NAME ];
        const char* save = p;
        p = read_ident( p, tmp, RG_MAX_NAME );
        const char* q = skip_ws( p );
        if ( *q == '(' )
        {
            char dummy[ 4 ];
            p = read_paren_block( q, dummy, sizeof dummy );
            p = skip_ws( p );
            continue;
        }
        /* identifier with no paren = the typedef name */
        rg_str_copy( out, tmp, max );
        return 1;
        (void)save;
    }
    out[ 0 ] = '\0';
    return 0;
}

/*----------------------------------------------------------------------------------------------
    Parse a single field declaration starting just past the closing paren of RS_PROP(...).
    Stops at the terminating `;`. Returns position after `;` (or '\0').
----------------------------------------------------------------------------------------------*/

static const char*
parse_field_decl( const char* p, rg_decl_field_t* out )
{
    /* Tokenize until ';'. Build base-type, modifier slots, name, optional array. */
    int      base_const   = 0;
    char     base_type[ RG_MAX_NAME ] = { 0 };
    int      have_base    = 0;
    char     field_name[ RG_MAX_NAME ] = { 0 };
    uint8_t  slots[ 4 ]   = { 0, 0, 0, 0 };
    int      slot_count   = 0;
    uint16_t array_count  = 0;

    /* Collect pending `const` between '*'s. Once we see '*', append a slot.
       After the field name we may see '[N]'. */
    int pending_const = 0;
    int seen_star     = 0;

    while ( 1 )
    {
        p = skip_ws( p );
        if ( !*p || *p == ';' )
            break;
        if ( *p == ',' )
        {
            /* multi-declarator not supported: skip rest of line */
            while ( *p && *p != ';' )
                p++;
            break;
        }
        if ( *p == '*' )
        {
            if ( slot_count < 4 )
            {
                slots[ slot_count++ ] = (uint8_t)RG_MOD_SLOT( RG_MOD_PTR, 0 );
            }
            pending_const = 0;
            seen_star     = 1;
            p++;
            continue;
        }
        if ( is_ident_start( (unsigned char)*p ) )
        {
            char tok[ RG_MAX_NAME ];
            const char* np = read_ident( p, tok, RG_MAX_NAME );

            /* keyword handling */
            if ( strcmp( tok, "const" ) == 0 )
            {
                if ( !have_base )
                    base_const = 1;
                else if ( seen_star && slot_count > 0 )
                {
                    /* `T* const` - turn the last PTR slot into a CONST_PTR */
                    uint8_t s = slots[ slot_count - 1 ];
                    s         = (uint8_t)( ( s & 0x3 ) | ( 1 << 2 ) );
                    slots[ slot_count - 1 ] = s;
                }
                else
                {
                    /* `T const` -> applies to base */
                    base_const = 1;
                }
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
                q = skip_ws( q );
                /* read integer literal (allow trailing ident like 'N' but treat as 0) */
                uint32_t n = 0;
                int      any = 0;
                while ( is_digit( (unsigned char)*q ) )
                {
                    n = n * 10 + (uint32_t)( *q - '0' );
                    q++;
                    any = 1;
                }
                if ( any )
                {
                    array_count = (uint16_t)n;
                }
                /* skip to ']' */
                while ( *q && *q != ']' )
                    q++;
                if ( *q == ']' )
                    q++;
                p = q;

                if ( slot_count < 4 )
                    slots[ slot_count++ ] = (uint8_t)RG_MOD_SLOT( RG_MOD_ARRAY, 0 );
            }
            (void)pending_const;
            continue;
        }
        /* unknown character - skip */
        p++;
    }

    /* advance past ';' if present */
    if ( *p == ';' )
        p++;

    /* commit */
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
    Parse the body of a RS_STRUCT - scan for RS_PROP markers and parse each field.
    `body_start` points just after the opening '{'; `body_end` points at the closing '}'.
----------------------------------------------------------------------------------------------*/

static void
parse_struct_body( const char* body_start, const char* body_end,
                   const char* buf_start, rg_decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end )
    {
        /* find RS_PROP at a word boundary */
        if ( match_word( p, buf_start, "RS_PROP" ) )
        {
            p += 7;
            p = skip_ws( p );

            char args[ 1024 ];
            args[ 0 ] = '\0';
            if ( *p == '(' )
                p = read_paren_block( p, args, sizeof args );

            if ( type->field_count >= RG_MAX_FIELDS_PER_TYPE )
            {
                /* skip remainder of this field decl */
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
            continue;
        }
        p++;
    }
}

/*----------------------------------------------------------------------------------------------
    Parse the body of an RS_ENUM / RS_BITSET. Extract `NAME [= VALUE]` pairs.
----------------------------------------------------------------------------------------------*/

static void
parse_enum_body( const char* body_start, const char* body_end, rg_decl_type_t* type )
{
    const char* p = body_start;
    while ( p < body_end )
    {
        p = skip_ws( p );
        if ( p >= body_end )
            break;
        if ( !is_ident_start( (unsigned char)*p ) )
        {
            p++;
            continue;
        }

        char name[ RG_MAX_NAME ];
        p = read_ident( p, name, RG_MAX_NAME );
        p = skip_ws( p );

        char value[ RG_MAX_NAME ] = { 0 };
        int  has_value           = 0;
        if ( *p == '=' )
        {
            p++;
            p          = skip_ws( p );
            int vn     = 0;
            int depth  = 0;
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
        /* skip to next ',' or '}' */
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
    Top-level marker dispatch for one file buffer.
----------------------------------------------------------------------------------------------*/

static void
parse_buffer( const char* buf, rg_parse_data_t* out )
{
    const char* p = buf;

    while ( *p )
    {
        int          kind        = -1;
        const char*  marker      = NULL;
        int          marker_len  = 0;

        if ( match_word( p, buf, "RS_STRUCT" ) )
        {
            kind = RG_KIND_STRUCT; marker = "RS_STRUCT"; marker_len = 9;
        }
        else if ( match_word( p, buf, "RS_BITSET" ) )
        {
            kind = RG_KIND_BITSET; marker = "RS_BITSET"; marker_len = 9;
        }
        else if ( match_word( p, buf, "RS_ENUM" ) )
        {
            kind = RG_KIND_ENUM;   marker = "RS_ENUM";   marker_len = 7;
        }

        if ( kind < 0 )
        {
            p++;
            continue;
        }

        p += marker_len;
        p = skip_ws( p );

        char args[ 1024 ];
        args[ 0 ] = '\0';
        if ( *p == '(' )
            p = read_paren_block( p, args, sizeof args );

        if ( out->type_count >= RG_MAX_TYPES )
            continue;

        rg_decl_type_t* t = &out->types[ out->type_count ];
        memset( t, 0, sizeof *t );
        t->kind = kind;
        parse_attr_args( args, t->attrs, RG_MAX_ATTRS_PER_ITEM, &t->attr_count );

        /* Walk forward to the body brace, then find the typedef name. */
        p = skip_ws( p );
        /* skip optional 'typedef', 'struct', 'enum', tag identifier */
        while ( is_ident_start( (unsigned char)*p ) )
        {
            char tmp[ RG_MAX_NAME ];
            const char* save = p;
            p = read_ident( p, tmp, RG_MAX_NAME );
            p = skip_ws( p );
            if ( *p == '{' )
                break;
            /* allow `: type_tag` (enum underlying type) - rare in C */
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
            (void)save;
        }

        if ( *p != '{' )
            continue;

        const char* body_open  = p;
        const char* body_close = find_close_brace( p );
        if ( body_close == body_open )
            continue;

        /* Read typedef name after the closing brace. */
        if ( !read_typedef_name( body_close, t->name, RG_MAX_NAME ) )
            continue;

        /* Parse the body. */
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

        /* Advance past closing brace and trailing `;`. */
        p = body_close + 1;
        while ( *p && *p != ';' )
            p++;
        if ( *p == ';' )
            p++;
    }
}

/*----------------------------------------------------------------------------------------------
    Derive an include path from an absolute file path by trimming everything up to and
    including the first "/source/" segment. Backslashes are normalized to forward slashes.
----------------------------------------------------------------------------------------------*/

static void
make_include_path( const char* abs_path, char* out, int max )
{
    char tmp[ RG_MAX_PATH ];
    int  n = 0;
    for ( int i = 0; abs_path[ i ] && n < RG_MAX_PATH - 1; i++ )
        tmp[ n++ ] = abs_path[ i ] == '\\' ? '/' : abs_path[ i ];
    tmp[ n ] = '\0';

    const char* sep = strstr( tmp, "/source/" );
    if ( sep )
        rg_str_copy( out, sep + 8, max ); /* skip "/source/" */
    else
        rg_str_copy( out, tmp, max );
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

    for ( int i = 0; i < files->count; i++ )
    {
        FILE* f = fopen( files->paths[ i ], "rb" );
        if ( !f )
            continue;

        fseek( f, 0, SEEK_END );
        long size = ftell( f );
        fseek( f, 0, SEEK_SET );

        if ( size <= 0 || size > 8 * 1024 * 1024 )
        {
            fclose( f );
            continue;
        }

        char* buf = (char*)malloc( (size_t)( size + 1 ) );
        if ( !buf )
        {
            fclose( f );
            continue;
        }

        size_t nread = fread( buf, 1, (size_t)size, f );
        fclose( f );
        buf[ nread ] = '\0';

        int before = out->type_count;
        parse_buffer( buf, out );
        if ( out->type_count > before && out->header_count < RG_MAX_FILES )
        {
            make_include_path( files->paths[ i ],
                               out->headers[ out->header_count ],
                               RG_MAX_PATH );
            out->header_count++;
        }

        free( buf );
    }
}
