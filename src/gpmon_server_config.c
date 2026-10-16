#include "gpmon_server_config.h"

#include <string.h>
#include "lfs_kv.h"
#include "wifi_mgmr_ext.h"
#include <lwip/ip4_addr.h>

static int s_storage_ready;

void gpmon_server_config_init(int storage_ready)
{
    s_storage_ready = storage_ready ? 1 : 0;
}

static void resolve_gateway(gpmon_server_config_t *out)
{
    uint32_t ip = 0, gateway = 0, mask = 0;
    ip4_addr_t addr;
    out->ip[0] = '\0';
    out->port = 9100;
    wifi_mgmr_sta_ip_get(&ip, &gateway, &mask);
    if (gateway != 0) {
        addr.addr = gateway;
        const char *text = ip4addr_ntoa(&addr);
        if (text != NULL && strnlen(text, GPMON_SERVER_IP_MAX) < GPMON_SERVER_IP_MAX)
            memcpy(out->ip, text, strlen(text) + 1u);
    }
}

static void set_default_auto(gpmon_server_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = GPMON_SERVER_CONFIG_MAGIC;
    out->version = GPMON_SERVER_CONFIG_VERSION;
    out->port = 9100;
    out->mode = GPMON_SERVER_MODE_AUTO;
    resolve_gateway(out);
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
        out->port == 0 ||
        (out->mode != GPMON_SERVER_MODE_AUTO &&
         out->mode != GPMON_SERVER_MODE_MANUAL)) {
        set_default_auto(out);
        return 1;
    }

    if (out->mode == GPMON_SERVER_MODE_AUTO) {
        resolve_gateway(out);
    } else if (out->ip[0] == '\0' ||
               strnlen(out->ip, sizeof(out->ip)) >= sizeof(out->ip)) {
        set_default_auto(out);
        return 1;
    }
    return 0;
}

int gpmon_server_config_set_auto(void)
{
    gpmon_server_config_t config;
    if (!s_storage_ready) return -1;
    memset(&config, 0, sizeof(config));
    config.magic = GPMON_SERVER_CONFIG_MAGIC;
    config.version = GPMON_SERVER_CONFIG_VERSION;
    config.port = 9100;
    config.mode = GPMON_SERVER_MODE_AUTO;
    return lfs_kv_set_blob(GPMON_SERVER_CONFIG_KEY, &config, sizeof(config)) == LFS_KV_OK ? 0 : -1;
}

int gpmon_server_config_set(const char *ip, uint16_t port)
{
    gpmon_server_config_t config;
    ip4_addr_t parsed;
    if (!s_storage_ready || ip == NULL || port == 0 ||
        strnlen(ip, GPMON_SERVER_IP_MAX) >= GPMON_SERVER_IP_MAX ||
        !ip4addr_aton(ip, &parsed)) return -1;

    memset(&config, 0, sizeof(config));
    config.magic = GPMON_SERVER_CONFIG_MAGIC;
    config.version = GPMON_SERVER_CONFIG_VERSION;
    memcpy(config.ip, ip, strlen(ip) + 1u);
    config.port = port;
    config.mode = GPMON_SERVER_MODE_MANUAL;
    return lfs_kv_set_blob(GPMON_SERVER_CONFIG_KEY, &config, sizeof(config)) == LFS_KV_OK ? 0 : -1;
}
