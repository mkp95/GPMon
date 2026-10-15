#ifndef GPMON_SERVER_CONFIG_H
#define GPMON_SERVER_CONFIG_H

#include <stdint.h>

#define GPMON_SERVER_CONFIG_KEY "gpmon/server_config"
#define GPMON_SERVER_CONFIG_MAGIC 0x47505343UL
#define GPMON_SERVER_CONFIG_VERSION 1UL
#define GPMON_SERVER_IP_MAX 16u

typedef struct {
    uint32_t magic;
    uint32_t version;
    char ip[GPMON_SERVER_IP_MAX];
    uint16_t port;
    uint16_t reserved;
} gpmon_server_config_t;

/* Call after lfs_kv_init(). */
void gpmon_server_config_init(int storage_ready);
int gpmon_server_config_get(gpmon_server_config_t *out);
int gpmon_server_config_set(const char *ip, uint16_t port);

#endif
