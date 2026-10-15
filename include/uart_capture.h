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

/* Configure UART1 RX=GPIO11, TX=GPIO12 and enable RX interrupts. */
bool gpmon_uart_capture_init(gpmon_core_t *core);
/* Drain ISR data and advance the configured silence-based framer. Non-blocking. */
void gpmon_uart_capture_poll(void);
/* Pop one complete packet from the core's RAM queue. */
const gpmon_record_t *gpmon_uart_capture_peek(void);
void gpmon_uart_capture_release(void);
void gpmon_uart_capture_get_stats(gpmon_uart_capture_stats_t *out);

#endif
