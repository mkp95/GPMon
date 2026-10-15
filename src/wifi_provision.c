#include <stdio.h>
#include <string.h>
#include "wifi_credentials.h"

#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "bflb_mtd.h"
#include "lfs_kv.h"
#include "rfparam_adapter.h"
#include "async_event.h"
#include "wifi_mgmr_ext.h"
#include <lwip/tcpip.h>

#include "web_config.h"
#include "gpmon_server_config.h"

void wifi_event_handler(async_input_event_t ev, void *priv);

#define GPMON_AP_SSID    "GPMon_Config"
#define GPMON_AP_PASSWD  NULL
#define GPMON_AP_CHANNEL 1

static wifi_conf_t s_wifi_conf = { .country_code = "CN" };
static web_config_t s_web_ctx;
static gpmon_wifi_config_t s_saved_wifi;
static int s_saved_wifi_valid;
static int s_wifi_storage_ready;
static TaskHandle_t s_credential_task;

static void gpmon_wifi_load_saved(void)
{
    size_t saved_len = 0;
    size_t read_len;

    memset(&s_saved_wifi, 0, sizeof(s_saved_wifi));
    s_saved_wifi_valid = 0;

    read_len = lfs_kv_get_blob(
        GPMON_WIFI_CONFIG_KEY, &s_saved_wifi,
        sizeof(s_saved_wifi), &saved_len);

    if (read_len != sizeof(s_saved_wifi) ||
        saved_len != sizeof(s_saved_wifi) ||
        s_saved_wifi.magic != GPMON_WIFI_CONFIG_MAGIC ||
        s_saved_wifi.version != GPMON_WIFI_CONFIG_VERSION ||
        s_saved_wifi.ssid[0] == '\0' ||
        strnlen(s_saved_wifi.ssid, sizeof(s_saved_wifi.ssid))
            >= sizeof(s_saved_wifi.ssid) ||
        strnlen(s_saved_wifi.password, sizeof(s_saved_wifi.password))
            >= sizeof(s_saved_wifi.password)) {
        memset(&s_saved_wifi, 0, sizeof(s_saved_wifi));
        printf("GPMon: no valid saved Wi-Fi credentials\r\n");
        return;
    }

    s_saved_wifi_valid = 1;
    printf("GPMon: saved Wi-Fi credentials loaded\r\n");
}

static void gpmon_wifi_credential_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_wifi_storage_ready) {
            continue;
        }

        gpmon_wifi_config_t config;
        wifi_mgmr_sta_connect_ind_stat_info_t info;
        memset(&info, 0, sizeof(info));

        wifi_mgmr_sta_connect_ind_stat_get(&info);

        size_t ssid_len = strnlen(info.ssid, sizeof(info.ssid));
        size_t password_len = strnlen(info.passphr, sizeof(info.passphr));

        if (ssid_len == 0 ||
            ssid_len >= sizeof(config.ssid) ||
            password_len >= sizeof(config.password)) {
            printf("GPMon: connected Wi-Fi credentials invalid\r\n");
            continue;
        }

        memset(&config, 0, sizeof(config));
        config.magic = GPMON_WIFI_CONFIG_MAGIC;
        config.version = GPMON_WIFI_CONFIG_VERSION;
        memcpy(config.ssid, info.ssid, ssid_len);
        memcpy(config.password, info.passphr, password_len);

        if (s_saved_wifi_valid &&
            strcmp(s_saved_wifi.ssid, config.ssid) == 0 &&
            strcmp(s_saved_wifi.password, config.password) == 0) {
            continue;
        }

        if (lfs_kv_set_blob(GPMON_WIFI_CONFIG_KEY,
                            &config, sizeof(config)) != LFS_KV_OK) {
            printf("GPMon: failed to save Wi-Fi credentials\r\n");
            continue;
        }

        s_saved_wifi = config;
        s_saved_wifi_valid = 1;
        printf("GPMon: Wi-Fi credentials saved\r\n");
    }
}

#ifdef BL602
extern void wifi_task_create(void);
extern int fhost_init(void);
#endif

static void wifi_init_task(void *arg)
{
    (void)arg;

    async_register_event_filter(EV_WIFI, wifi_event_handler, NULL);
    wifi_task_create();
    fhost_init();

    vTaskDelete(NULL);
}

static void ap_start_task(void *arg)
{
    wifi_interface_t wifi_if;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_mgmr_sta_enable();
    wifi_if = wifi_mgmr_ap_enable();
    wifi_mgmr_ap_start(wifi_if, GPMON_AP_SSID, 0,
                       GPMON_AP_PASSWD, GPMON_AP_CHANNEL);

    printf("GPMon: starting setup access point '%s'\r\n",
           GPMON_AP_SSID);

    if (s_saved_wifi_valid) {
        vTaskDelay(pdMS_TO_TICKS(500));
        char *password = s_saved_wifi.password[0]
                             ? s_saved_wifi.password : NULL;

        printf("GPMon: attempting saved Wi-Fi connection\r\n");
        int rc = wifi_mgmr_sta_connect(
            NULL, s_saved_wifi.ssid, password, NULL, NULL, 0, 0);

        if (rc != 0) {
            printf("GPMon: saved connection request failed: %d\r\n", rc);
        }
    }

    vTaskDelete(NULL);
}

static void web_init_task(void *arg)
{
    (void)arg;

    /* Match the SDK example's scan and HTTP startup sequence. */
    web_config_do_scan();
    vTaskDelay(pdMS_TO_TICKS(4500));

    if (web_config_init(&s_web_ctx) == 0) {
        printf("GPMon: Wi-Fi setup web server initialized\r\n");
        printf("GPMon: try http://192.168.169.1/\r\n");
        printf("GPMon: if that address fails, check the AP interface IP\r\n");
    } else {
        printf("GPMon: web configuration initialization failed\r\n");
    }

    vTaskDelete(NULL);
}

void wifi_event_handler(async_input_event_t ev, void *priv)
{
    (void)priv;

    switch (ev->code) {
    case CODE_WIFI_ON_INIT_DONE:
        wifi_mgmr_init(&s_wifi_conf);
        break;

    case CODE_WIFI_ON_MGMR_DONE:
        if (xTaskCreate(ap_start_task, "gpmon_ap", 1024,
                         NULL, 15, NULL) != pdPASS) {
            printf("GPMon: failed to create AP task\r\n");
        }
        break;

    case CODE_WIFI_ON_GOT_IP:
        printf("GPMon: Wi-Fi obtained an IP address\r\n");
        if (s_credential_task != NULL) {
            xTaskNotifyGive(s_credential_task);
        }
        web_config_notify_result(1);
        break;

    case CODE_WIFI_ON_AP_STARTED:
        if (xTaskCreate(web_init_task, "gpmon_web", 1024,
                         NULL, 14, NULL) != pdPASS) {
            printf("GPMon: failed to create web task\r\n");
        }
        break;

    default:
        break;
    }
}

/*
 * Call after board_init(), before vTaskStartScheduler().
 * A Wi-Fi initialization failure is reported to the caller; UART capture
 * can continue independently.
 */
int gpmon_wifi_provision_start(void)
{
    bflb_mtd_init();

    if (lfs_kv_init() == LFS_KV_OK) {
        s_wifi_storage_ready = 1;
        gpmon_server_config_init(1);
        gpmon_wifi_load_saved();
    } else {
        s_wifi_storage_ready = 0;
        gpmon_server_config_init(0);
        printf("GPMon: LittleFS KV init failed; credentials will not persist\r\n");
    }

    if (rfparam_init(0, NULL, 0) != 0) {
        printf("GPMon: RF parameter initialization failed; Wi-Fi disabled\r\n");
        return -1;
    }

    tcpip_init(NULL, NULL);

    if (xTaskCreate(gpmon_wifi_credential_task, "gpmon_cred",
                    1024, NULL, 9, &s_credential_task) != pdPASS) {
        printf("GPMon: failed to create credential task\r\n");
        return -1;
    }

    if (xTaskCreate(wifi_init_task, "gpmon_wifi", 1024,
                    NULL, 10, NULL) != pdPASS) {
        printf("GPMon: failed to create Wi-Fi initialization task\r\n");
        return -1;
    }

    printf("GPMon: Wi-Fi provisioning startup requested\r\n");
    return 0;
}
