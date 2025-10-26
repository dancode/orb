/*============================================================================================*/


/*============================================================================================*/

typedef void ( *cmd_fn )( int argc, char** argv, void* ctx );

                                // initialize command system
void        Cmd_Init            ( void );

                                // shutdown command system
void        Cmd_Shutdown        ( void );

                                // register a console command           
void        Cmd_Add             ( const char* name, cmd_fn fn, const char* help, void* ctx );

                                // 
void        Cmd_Remove          ( const char* name );

                                // returns nonzero if handled
int         Cmd_ExecuteString   ( const char* text );    

                                // push into execution queue
void        Cmd_QueueText       ( const char* text );     

                                // called in main loop
void        Cmd_ProcessQueue    ( void );                 

/*============================================================================================*/