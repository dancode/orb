#ifndef APP_H
#define APP_H
/*==============================================================================================

    app.h

==============================================================================================*/

typedef bool ( *app_frame_fn )( void* user, float dt );

typedef struct app_loop_s
{
    app_frame_fn on_frame;
    void*        user;
    int          target_fps; /* 0 = uncapped */

} app_loop_t;

void app_loop_run( const app_loop_t* loop );

/*============================================================================================*/
#endif    // APP_H