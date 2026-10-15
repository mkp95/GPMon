#ifndef GPMON_WIFI_CREDENTIALS_H
#define GPMON_WIFI_CREDENTIALS_H

#include <stdint.h>

#define GPMON_WIFI_CONFIG_KEY     "gpmon/wifi_config"
#define GPMON_WIFI_CONFIG_MAGIC   0x47504D4FUL
#define GPMON_WIFI_CONFIG_VERSION 1UL

typedef struct {
    uint32_t magic;
    uint32_t version;
    char ssid[33];
    char password[65];
} gpmon_wifi_config_t;

#endif
