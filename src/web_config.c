#include "web_config.h"
#include <string.h>
#include <stdlib.h>
#include "gpmon_server_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include <lwip/apps/httpd.h>
#include <lwip/ip4_addr.h>
#include "cJSON.h"
#include "wifi_mgmr_ext.h"

#define DBG_TAG "WEB"
#include "log.h"

#define SCAN_AP_MAX 30
#define AP_CLOSE_DELAY_MS 10000
#define LIVE_HISTORY_DEPTH 6

static gpmon_record_t g_live_history[LIVE_HISTORY_DEPTH];
static gpmon_record_t g_live_snapshot[LIVE_HISTORY_DEPTH];
static uint32_t g_live_next = 0;
static uint32_t g_live_count = 0;
static uint32_t g_live_packet_count = 0;
static gpmon_uart_capture_stats_t g_live_stats;
static uint32_t g_live_queue_depth = 0;
static uint32_t g_live_queue_high_water = 0;

static web_config_t *g_ctx = NULL;
static TimerHandle_t g_ap_close_timer = NULL;
static volatile int g_scanning = 0;

typedef struct {
    wifi_mgmr_ap_item_t items[SCAN_AP_MAX];
    uint16_t count;
} scan_result_t;

static scan_result_t g_scan_cache = {0};

static int hex_char_to_int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int h = hex_char_to_int(src[1]);
            int l = hex_char_to_int(src[2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)((h << 4) | l);
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
            continue;
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

static void scan_item_cb(wifi_mgmr_ap_item_t *env, uint32_t *param1, wifi_mgmr_ap_item_t *item)
{
    scan_result_t *result = (scan_result_t *)param1;
    if (result->count >= SCAN_AP_MAX) return;
    memcpy(&result->items[result->count], item, sizeof(wifi_mgmr_ap_item_t));
    result->count++;
}


static volatile int g_scan_error = 0;
static volatile int g_scan_ready = 0;

static void scan_complete_cb(void *data, void *param)
{
    scan_result_t temp = {0};

    (void)data;
    (void)param;

    wifi_mgmr_scan_ap_all(NULL, (uint32_t *)&temp, scan_item_cb);

    /* Publish fresh results even when the scan finds zero APs. */
    memcpy(&g_scan_cache, &temp, sizeof(g_scan_cache));

    LOG_I("Scan complete: found %d APs\r\n", temp.count);
    g_scan_ready = 1;
    g_scanning = 0;
}

void web_config_do_scan(void)
{
    int rc;

    if (g_scanning) {
        return;
    }

    g_scan_error = 0;
    g_scan_ready = 0;
    g_scanning = 1;

    rc = wifi_mgmr_scan(NULL, scan_complete_cb);
    if (rc != 0) {
        g_scan_error = 1;
        g_scanning = 0;
        LOG_E("Failed to start Wi-Fi scan: %d\r\n", rc);
    }
}


static const char *cgi_scan(int idx, int num, char *param[], char *val[])
{
    cJSON *obj;
    char *json;

    (void)idx;
    (void)num;
    (void)param;
    (void)val;

    /*
     * Start a scan only when there is neither a scan running
     * nor a completed result waiting to be collected.
     */
    if (!g_scanning && !g_scan_ready) {
        web_config_do_scan();

        if (g_scan_error) {
            obj = cJSON_CreateObject();
            if (!obj) return NULL;
            cJSON_AddStringToObject(obj, "error", "Failed to start scan");
            json = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
            return json;
        }
    }

    if (g_scanning) {
        obj = cJSON_CreateObject();
        if (!obj) return NULL;
        cJSON_AddBoolToObject(obj, "scanning", 1);
        json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        return json;
    }

    if (g_scan_ready) {
        g_scan_ready = 0;

        if (g_scan_error) {
            obj = cJSON_CreateObject();
            if (!obj) return NULL;
            cJSON_AddStringToObject(obj, "error", "Wi-Fi scan failed");
            json = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
            return json;
        }

        obj = cJSON_CreateArray();
        if (!obj) return NULL;

        if (g_scan_cache.count == 0) {
            cJSON_AddItemToArray(obj, cJSON_CreateString("null"));
        } else {
            for (int i = 0; i < g_scan_cache.count; i++) {
                cJSON *ap = cJSON_CreateObject();
                if (!ap) continue;

                cJSON_AddNumberToObject(ap, "chan",
                                        g_scan_cache.items[i].channel);
                cJSON_AddNumberToObject(ap, "rssi",
                                        g_scan_cache.items[i].rssi);
                cJSON_AddStringToObject(ap, "ssid",
                                        g_scan_cache.items[i].ssid);
                cJSON_AddNumberToObject(ap, "auth",
                                        g_scan_cache.items[i].auth);
                cJSON_AddItemToArray(obj, ap);
            }
        }

        json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        return json;
    }

    /* Scan may have completed between checks; poll again. */
    obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddBoolToObject(obj, "scanning", 1);
    json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static const char *cgi_connect(int idx, int num, char *param[], char *val[])
{
    char ssid[33] = {0}, passwd[65] = {0};
    int ok = 0;

    for (int i = 0; i < num; i++) {
        if (strcmp(param[i], "ssid") == 0) {
            url_decode(ssid, val[i], sizeof(ssid));
            ok = 1;
        } else if (strcmp(param[i], "password") == 0) {
            url_decode(passwd, val[i], sizeof(passwd));
        }
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    if (ok) {
        g_ctx->conn_status = CONN_STATUS_CONNECTING;
        wifi_mgmr_sta_connect(NULL, ssid, passwd, NULL, NULL, 0, 0);
        cJSON_AddStringToObject(obj, "state", "connecting");
    } else {
        cJSON_AddStringToObject(obj, "state", "failed");
    }

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}


void web_config_publish_packet(const gpmon_record_t *record)
{
    if (record == NULL || record->length > GPMON_MAX_PACKET_BYTES) return;

    taskENTER_CRITICAL();
    g_live_history[g_live_next] = *record;
    g_live_next = (g_live_next + 1u) % LIVE_HISTORY_DEPTH;
    if (g_live_count < LIVE_HISTORY_DEPTH) g_live_count++;
    g_live_packet_count++;
    taskEXIT_CRITICAL();
}

void web_config_publish_stats(const gpmon_uart_capture_stats_t *stats,
                              uint32_t queue_depth,
                              uint32_t queue_high_water)
{
    taskENTER_CRITICAL();
    if (stats != NULL) g_live_stats = *stats;
    g_live_queue_depth = queue_depth;
    g_live_queue_high_water = queue_high_water;
    taskEXIT_CRITICAL();
}

static const char *cgi_gpmonlive(int idx, int num, char *param[], char *val[])
{
    cJSON *root = NULL, *packets = NULL, *packet = NULL;
    char *json = NULL;
    uint32_t count, packet_total, queue_depth, queue_high_water;
    gpmon_uart_capture_stats_t stats;
    uint32_t i;
    conn_status_t wifi_state;

    (void)idx; (void)num; (void)param; (void)val;

    /* Copy a bounded snapshot quickly; JSON allocation happens outside the
       critical section so UART capture is never held off by serialization. */
    taskENTER_CRITICAL();
    count = g_live_count;
    packet_total = g_live_packet_count;
    stats = g_live_stats;
    queue_depth = g_live_queue_depth;
    queue_high_water = g_live_queue_high_water;
    wifi_state = g_ctx ? g_ctx->conn_status : CONN_STATUS_IDLE;
    for (i = 0; i < count; i++) {
        uint32_t pos = (g_live_next + LIVE_HISTORY_DEPTH - 1u - i)
                       % LIVE_HISTORY_DEPTH;
        g_live_snapshot[i] = g_live_history[pos];
    }
    taskEXIT_CRITICAL();

    root = cJSON_CreateObject();
    packets = cJSON_CreateArray();
    if (root == NULL || packets == NULL) goto cleanup;

    cJSON_AddNumberToObject(root, "packet_count", packet_total);
    cJSON_AddNumberToObject(root, "rx_bytes", stats.rx_bytes);
    cJSON_AddNumberToObject(root, "ring_overflows", stats.rx_ring_overflows);
    cJSON_AddNumberToObject(root, "uart_errors", stats.uart_error_events);
    cJSON_AddNumberToObject(root, "queue_depth", queue_depth);
    cJSON_AddNumberToObject(root, "queue_high_water", queue_high_water);
    cJSON_AddStringToObject(root, "wifi_state",
        wifi_state == CONN_STATUS_AP_CLOSED ? "ap_closed" :
        wifi_state == CONN_STATUS_SUCCESS ? "connected" :
        wifi_state == CONN_STATUS_CONNECTING ? "connecting" : "idle");

    for (i = 0; i < count; i++) {
        const gpmon_record_t *r = &g_live_snapshot[i];
        char ascii[GPMON_MAX_PACKET_BYTES + 1u];
        char hex[GPMON_MAX_PACKET_BYTES * 3u + 1u];
        size_t hp = 0;
        uint32_t j;

        for (j = 0; j < r->length; j++) {
            uint8_t c = r->payload[j];
            ascii[j] = (c >= 32u && c <= 126u) ? (char)c : '.';
            if (j != 0) hex[hp++] = ' ';
            static const char digits[] = "0123456789ABCDEF";
            hex[hp++] = digits[(c >> 4) & 0x0Fu];
            hex[hp++] = digits[c & 0x0Fu];
        }
        ascii[r->length] = '\0';
        hex[hp] = '\0';

        packet = cJSON_CreateObject();
        if (packet == NULL) continue;
        cJSON_AddNumberToObject(packet, "seq", r->sequence);
        cJSON_AddNumberToObject(packet, "source_id", r->source_id);
        cJSON_AddNumberToObject(packet, "len", r->length);
        cJSON_AddNumberToObject(packet, "flags", r->flags);
        cJSON_AddNumberToObject(packet, "first_us", (double)r->first_byte_us);
        cJSON_AddStringToObject(packet, "ascii", ascii);
        cJSON_AddStringToObject(packet, "hex", hex);
        cJSON_AddItemToArray(packets, packet);
        packet = NULL;
    }
    cJSON_AddItemToObject(root, "packets", packets);
    packets = NULL;
    json = cJSON_PrintUnformatted(root);

cleanup:
    if (packet) cJSON_Delete(packet);
    if (packets) cJSON_Delete(packets);
    if (root) cJSON_Delete(root);
    return json;
}


static const char *cgi_serverconfig(int idx, int num, char *param[], char *val[])
{
    static const char page[] =
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>GPMon Server</title><style>body{font:16px sans-serif;max-width:560px;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin:.8rem 0}input,select,button{font:inherit;padding:.5rem;width:100%;box-sizing:border-box}"
        "button{margin-top:1rem}#manual{padding:.5rem;background:#f3f4f6}#status{white-space:pre-wrap}</style></head><body>"
        "<h1>GPMon server settings</h1><p>Automatic mode uses the current Wi-Fi gateway on port 9100.</p>"
        "<label>Mode<select id='mode' onchange='showManual()'><option value='auto'>Automatic (Wi-Fi gateway)</option>"
        "<option value='manual'>Manual server IP</option></select></label>"
        "<div id='manual'><label>Server IPv4 address<input id='ip' inputmode='decimal' placeholder='192.168.1.50'></label>"
        "<label>Port<input id='port' type='number' min='1' max='65535' value='9100'></label></div>"
        "<button onclick='save()'>Save settings</button><p id='status'>Loading settings...</p>"
        "<script>function showManual(){document.getElementById('manual').style.display=document.getElementById('mode').value==='manual'?'block':'none'}"
        "async function load(){try{let r=await fetch('/serverget');let d=await r.json();document.getElementById('mode').value=d.mode||'auto';"
        "document.getElementById('ip').value=d.ip||'';document.getElementById('port').value=d.port||9100;"
        "document.getElementById('status').textContent='Current mode: '+(d.mode||'auto')+'\\nEffective destination: '+(d.ip||'(waiting for Wi-Fi gateway)')+':'+(d.port||9100);showManual()}"
        "catch(e){document.getElementById('status').textContent='Could not load settings: '+e}}"
        "async function save(){let m=document.getElementById('mode').value;let q=m==='auto'?'mode=auto':'mode=manual&ip='+encodeURIComponent(document.getElementById('ip').value)+'&port='+encodeURIComponent(document.getElementById('port').value);"
        "try{let r=await fetch('/serverset?'+q);let d=await r.json();document.getElementById('status').textContent=d.message||d.error||JSON.stringify(d);if(d.ok)load()}"
        "catch(e){document.getElementById('status').textContent='Save failed: '+e}}load();</script></body></html>";
    (void)idx; (void)num; (void)param; (void)val;
    return page;
}

static const char *cgi_serverget(int idx, int num, char *param[], char *val[])
{
    gpmon_server_config_t config;
    cJSON *obj = cJSON_CreateObject();
    char *json;
    int rc;
    (void)idx; (void)num; (void)param; (void)val;
    if (obj == NULL) return NULL;

    rc = gpmon_server_config_get(&config);
    cJSON_AddBoolToObject(obj, "ok", rc >= 0);
    cJSON_AddStringToObject(obj, "ip", config.ip);
    cJSON_AddNumberToObject(obj, "port", config.port ? config.port : 9100);
    cJSON_AddStringToObject(obj, "mode", config.mode == GPMON_SERVER_MODE_MANUAL ? "manual" : "auto");
    if (rc < 0) cJSON_AddStringToObject(obj, "error", "Persistent storage unavailable");

    json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static const char *cgi_serverset(int idx, int num, char *param[], char *val[])
{
    char ip[GPMON_SERVER_IP_MAX] = {0};
    char port_text[8] = {0};
    char mode[8] = {0};
    unsigned long port;
    int have_ip = 0, have_port = 0;
    cJSON *obj;
    char *json;
    (void)idx;
    LOG_I("serverset: num=%d\\r\\n", num);

    for (int i = 0; i < num; i++) {
        LOG_I("param[%d]='%s' val='%s'\\r\\n", i, param[i], val[i]);
        if (strcmp(param[i], "ip") == 0) {
            url_decode(ip, val[i], sizeof(ip));
            have_ip = 1;
        } else if (strcmp(param[i], "port") == 0) {
            url_decode(port_text, val[i], sizeof(port_text));
            have_port = 1;
        } else if (strcmp(param[i], "mode") == 0) {
            url_decode(mode, val[i], sizeof(mode));
        }
    }

    obj = cJSON_CreateObject();
    if (obj == NULL) return NULL;

    if (strcmp(mode, "auto") == 0) {
        if (gpmon_server_config_set_auto() != 0) {
            cJSON_AddBoolToObject(obj, "ok", 0);
            cJSON_AddStringToObject(obj, "error", "Could not save automatic gateway mode");
        } else {
            cJSON_AddBoolToObject(obj, "ok", 1);
            cJSON_AddStringToObject(obj, "message", "Automatic gateway mode saved");
        }
    } else if (!have_ip || !have_port || port_text[0] == '\0') {
        cJSON_AddBoolToObject(obj, "ok", 0);
        cJSON_AddStringToObject(obj, "error", "Manual mode requires server IP and port");
    } else {
        char *end = NULL;
        port = strtoul(port_text, &end, 10);
        if (end == port_text || *end != '\0' || port < 1 || port > 65535) {
            cJSON_AddBoolToObject(obj, "ok", 0);
            cJSON_AddStringToObject(obj, "error", "Port must be 1..65535");
        } else if (gpmon_server_config_set(ip, (uint16_t)port) != 0) {
            cJSON_AddBoolToObject(obj, "ok", 0);
            cJSON_AddStringToObject(obj, "error", "Invalid IPv4 address or save failed");
        } else {
            cJSON_AddBoolToObject(obj, "ok", 1);
            cJSON_AddStringToObject(obj, "message", "Manual server settings saved");
            LOG_I("Saved GPMon receiver %s:%u\r\n", ip, (unsigned int)port);
        }
    }

    json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static const char *cgi_status(int idx, int num, char *param[], char *val[])
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    conn_status_t st = g_ctx->conn_status;
    
    if (st == CONN_STATUS_AP_CLOSED || st == CONN_STATUS_SUCCESS) {
        uint32_t ip, gw, mask;
        ip4_addr_t addr;
        wifi_mgmr_sta_ip_get(&ip, &gw, &mask);
        addr.addr = ip;
        cJSON_AddStringToObject(obj, "state", st == CONN_STATUS_AP_CLOSED ? "ap_closed" : "connected");
        cJSON_AddStringToObject(obj, "ip", ip4addr_ntoa(&addr));
        cJSON_AddStringToObject(obj, "msg", st == CONN_STATUS_AP_CLOSED ? "AP Closed" : "AP will close in 10s");
    } else {
        cJSON_AddStringToObject(obj, "state", st == CONN_STATUS_CONNECTING ? "connecting" : "idle");
    }

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

static const tCGI cgi_handlers[] = {
    {"/wifiscan", cgi_scan},
    {"/wificonfig", cgi_connect},
    {"/wifiget", cgi_status},
    {"/serverconfig", cgi_serverconfig},
    {"/serverget", cgi_serverget},
    {"/serverset", cgi_serverset},
    {"/gpmonlive", cgi_gpmonlive},
};

static void ap_close_timer_cb(TimerHandle_t xTimer)
{
    LOG_I("Closing AP after successful connection\r\n");
    wifi_mgmr_ap_stop(NULL);
    if (g_ctx) {
        g_ctx->conn_status = CONN_STATUS_AP_CLOSED;
    }
}

void web_config_notify_result(int success)
{
    if (g_ctx && success) {
        g_ctx->conn_status = CONN_STATUS_SUCCESS;
        LOG_I("WiFi connected, AP will close in 10 seconds\r\n");
        
        if (g_ap_close_timer == NULL) {
            g_ap_close_timer = xTimerCreate("ap_close", pdMS_TO_TICKS(AP_CLOSE_DELAY_MS), 
                                            pdFALSE, NULL, ap_close_timer_cb);
        }
        if (g_ap_close_timer) {
            xTimerStart(g_ap_close_timer, 0);
        }
    }
}

int web_config_init(web_config_t *ctx)
{
    if (!ctx) return -1;
    g_ctx = ctx;
    ctx->conn_status = CONN_STATUS_IDLE;
    httpd_init();
    http_set_cgi_handlers(cgi_handlers, sizeof(cgi_handlers) / sizeof(cgi_handlers[0]));
    return 0;
}
