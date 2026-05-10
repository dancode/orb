#ifndef HOST_COMMON_H
#define HOST_COMMON_H

#include "runtime/host/runtime_host.h"

// Fills the config with defaults, then overrides them with CLI arguments
void host_common_parse_args( int argc, char** argv, runtime_config_t* out_config );

// Optional: Any platform-specific early setup (like Windows console UTF-8)
void host_common_early_setup( void );

#endif