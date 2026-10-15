#ifndef __WEB_CONFIG_H__
#define __WEB_CONFIG_H__

#include <stdint.h>
#include "capture_queue.h"
#include "uart_capture.h"

typedef enum {
    CONN_STATUS_IDLE = 0,
    CONN_STATUS_CONNECTING,
    CONN_STATUS_SUCCESS,
    CONN_STATUS_FAILED,
    CONN_STATUS_AP_CLOSED,
} conn_status_t;

typedef struct {
    volatile conn_status_t conn_status;
} web_config_t;

int web_config_init(web_config_t *ctx);
void web_config_notify_result(int success);
void web_config_do_scan(void);

/* Publish capture data for the read-only live dashboard. */
void web_config_publish_packet(const gpmon_record_t *record);
void web_config_publish_stats(const gpmon_uart_capture_stats_t *stats,
                              uint32_t queue_depth,
                              uint32_t queue_high_water);

#endif /* __WEB_CONFIG_H__ */
