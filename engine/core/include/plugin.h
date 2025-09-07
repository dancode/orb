/*============================================================================================*/

#pragma once
struct api_registry;

/*============================================================================================*/

// plugin entry signature used by all modules
#ifdef _WIN32
#    define ORB_API __declspec( dllexport )
#else
#    define ORB_API
#endif

ORB_API void load_plugin( struct api_registry* registry );


/*============================================================================================*/
