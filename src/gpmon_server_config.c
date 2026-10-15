#include "gpmon_server_config.h"

#include <string.h>
#include "lfs_kv.h"
#include <lwip/ip4_addr.h>

static int s_storage_ready;

void gpmon_server_config_init(int storage_ready)
{
    s_storage_ready = storage_ready ? 1 : 0;
}

int gpmon_server_config_get(gpmon_server_config_t *out)
{
    size_t saved_len = 0;
    size_t read_len;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (!s_storage_ready) return -1;
    read_len = lfs_kv_get_blob(GPMON_SERVER_CONFIG_KEY, out,
                               sizeof(*out), &saved_len);
    if (read_len != sizeof(*out) || saved_len != sizeof(*out) ||
        out->magic != GPMON_SERVER_CONFIG_MAGIC ||
        out->version != GPMON_SERVER_CONFIG_VERSION ||
        out->ip[0] == '\0' ||
        strnlen(out->ip, sizeof(out->ip)) >= sizeof(out->ip) ||
        out->port == 0) {
        memset(out, 0, sizeof(*out));
        out->magic = GPMON_SERVER_CONFIG_MAGIC;
        out->version = GPMON_SERVER_CONFIG_VERSION;
        out->port = 9100;
        return 1; /* No saved configuration yet; defaults supplied. */
    }
    return 0;
}

int gpmon_server_config_set(const char *ip, uint16_t port)
{
    gpmon_server_config_t config;
    ip4_addr_t parsed;

    if (!s_storage_ready || ip == NULL || port == 0 ||
        strnlen(ip, GPMON_SERVER_IP_MAX) >= GPMON_SERVER_IP_MAX ||
        !ip4addr_aton(ip, &parsed)) {
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.magic = GPMON_SERVER_CONFIG_MAGIC;
    config.version = GPMON_SERVER_CONFIG_VERSION;
    memcpy(config.ip, ip, strlen(ip) + 1);
    config.port = port;

    return lfs_kv_set_blob(GPMON_SERVER_CONFIG_KEY,
                           &config, sizeof(config)) == LFS_KV_OK ? 0 : -1;
}
