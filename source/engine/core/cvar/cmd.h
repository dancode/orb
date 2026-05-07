/*==============================================================================================
 
    command parsing

    Tokenize a string into '\0' separated strings that make up a single command.
    All commands are expected to execute via the generated command args.

==============================================================================================*/


// typedef hash_t argv_t;
// 
// cstr_t         cmd_str( i32 arg_id );    //
// 
// void           cmd_copy_args( str_t* sb );                        //
// void           cmd_copy_args_from( i32 arg_start, str_t* sb );    //
// 
// void           cmd_tokenize_string( cstr_t command_string );    //
// void           cmd_execute_string( cstr_t command_string );     //

/*============================================================================================

:   Command Buffer

:   Textual command processing buffer.
:   Commands are stored in a single buffer separated by a '\n' or ';' line break.

    Q2: Game adds with Cbuf_AddText( "exec quake.rc\n"); Cbuf_Execute();

============================================================================================*/

// enum    // how to execute added text into the command buffer.
// {
//     EXEC_NOW,       // execute now, skip buffer insertion.
//     EXEC_INSERT,    // insert at front of buffer, but don't run yet.
//     EXEC_APPEND     // append to end of buffer (default).
// };
// 
// void cbuf_init();
// void cbuf_append_text( cstr_t text, b32 terminate );
// void cbuf_insert_text( cstr_t* text );
// void cbuf_execute();

/*============================================================================================

:   Command Functions

:   Translates textual arguments into executable functions.
:   Commands are in a '/0' separated token stream with assocaited argc and argv data.

============================================================================================*/

// typedef void ( *cmd_func_t )();
// 
// typedef struct
// {                       // 32 bytes
//     cmd_func_t func;    // function to call on command execution.
//     hash_t     name;    // name of command
//     ci8*       desc;    // description of command
// 
// } cmd_t;
// 
// void   cmd_init();
// void   cmd_add( cmd_func_t in_func, ci8* in_name, ci8* in_desc );
// void   cmd_remove( ci8* in_name );
// cmd_t* cmd_find( cstr_t in_name );


/*============================================================================================*/


/*============================================================================================*/

// typedef void ( *cmd_fn )( int argc, char** argv, void* ctx );
// 
//                                 // initialize command system
// void        Cmd_Init            ( void );
// 
//                                 // shutdown command system
// void        Cmd_Shutdown        ( void );
// 
//                                 // register a console command           
// void        Cmd_Add             ( const char* name, cmd_fn fn, const char* help, void* ctx );
// 
//                                 // 
// void        Cmd_Remove          ( const char* name );
// 
//                                 // returns nonzero if handled
// int         Cmd_ExecuteString   ( const char* text );    
// 
//                                 // push into execution queue
// void        Cmd_QueueText       ( const char* text );     
// 
//                                 // called in main loop
// void        Cmd_ProcessQueue    ( void );                 

/*============================================================================================*/