#ifndef GPMON_PACKET_FRAMER_H
#define GPMON_PACKET_FRAMER_H

#include <stdbool.h>
#include <stdint.h>
#include "capture_queue.h"

typedef struct {
    uint32_t bytes_received;
    uint32_t packets_completed;
    uint32_t packets_queued;
    uint32_t packets_dropped;
    uint32_t bytes_dropped;
    uint32_t packets_truncated;
    uint32_t bytes_truncated;
    uint32_t uart_error_events;
} gpmon_framer_stats_t;

typedef struct {
    uint8_t payload[GPMON_MAX_PACKET_BYTES];
    uint16_t length;
    uint32_t total_length;
    uint64_t first_byte_us;
    uint64_t last_byte_us;
    uint8_t flags;
    uint8_t pending_flags;
    uint8_t source_id;
    bool open;
    uint16_t dropped_before;
    uint32_t next_sequence;
    gpmon_framer_stats_t stats;
} gpmon_framer_t;

void gpmon_framer_init(gpmon_framer_t *f);
void gpmon_framer_init_source(gpmon_framer_t *f, uint8_t source_id);
/* Each feed call must represent a contiguous RX burst. If the UART hardware
 * reports a receive-timeout boundary, feed/tick must preserve that boundary. */
void gpmon_framer_feed(gpmon_framer_t *f, gpmon_queue_t *q,
                       const uint8_t *data, uint32_t length, uint64_t now_us);
void gpmon_framer_tick(gpmon_framer_t *f, gpmon_queue_t *q, uint64_t now_us);
void gpmon_framer_note_uart_error(gpmon_framer_t *f);

#endif
