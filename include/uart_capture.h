#ifndef GPMON_UART_CAPTURE_H
#define GPMON_UART_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "gpmon_core.h"

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_ring_overflows;
    uint32_t uart_error_events;
} gpmon_uart_capture_stats_t;

/* Configure main UART1 (GPIO11/12) and daughter UART0 (GPIO7/16). */
bool gpmon_uart_capture_init(gpmon_core_t *core);
/* Drain both RX rings, servicing main-board UART1 first. Both feed one FIFO. */
void gpmon_uart_capture_poll(void);
/* Pop one complete packet from the core's RAM queue. */
const gpmon_record_t *gpmon_uart_capture_peek(void);
void gpmon_uart_capture_release(void);
void gpmon_uart_capture_get_stats(gpmon_uart_capture_stats_t *out);

#endif
