#ifndef GPMON_LWIPOPTS_USER_H
#define GPMON_LWIPOPTS_USER_H

/* Keep the Bouffalo SDK's complete lwIP configuration. */
#include "lwipopts.h"

/* Required by SDK code that calls lwIP statistics display functions. */
#ifndef LWIP_STATS_DISPLAY
#define LWIP_STATS_DISPLAY 1
#endif


/* HTTP provisioning page from the SDK smartconfig_softap example. */
#ifndef LWIP_HTTPD
#define LWIP_HTTPD 1
#endif
#ifndef LWIP_HTTPD_CGI
#define LWIP_HTTPD_CGI 1
#endif
#ifndef LWIP_HTTPD_DYNAMIC_HEADERS
#define LWIP_HTTPD_DYNAMIC_HEADERS 1
#endif
#ifndef LWIP_HTTPD_CUSTOM_FILES
#define LWIP_HTTPD_CUSTOM_FILES 1
#endif
#ifndef LWIP_HTTPD_DYNAMIC_FILE_READ
#define LWIP_HTTPD_DYNAMIC_FILE_READ 1
#endif
#ifndef LWIP_HTTPD_FILE_STATE
#define LWIP_HTTPD_FILE_STATE 1
#endif
#ifndef HTTPD_USE_CUSTOM_FSDATA
#define HTTPD_USE_CUSTOM_FSDATA 1
#endif

#endif
