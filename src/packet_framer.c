#include "packet_framer.h"
#include "gpmon_tcp_sender.h"
#include "gpmon_flash_queue.h"
#include <string.h>

void gpmon_framer_init(gpmon_framer_t *f)
{
    gpmon_framer_init_source(f, 1u);
}

void gpmon_framer_init_source(gpmon_framer_t *f, uint8_t source_id)
{
    if (f != NULL) { memset(f, 0, sizeof(*f)); f->source_id = source_id; }
}

static void close_packet(gpmon_framer_t *f, gpmon_queue_t *q)
{
    gpmon_record_t record;
    memset(&record, 0, sizeof(record));
    record.first_byte_us = f->first_byte_us;
    record.source_id = f->source_id;
    if (gpmon_tcp_sequence_next(&record.sequence) != 0) {
        f->stats.packets_dropped++;
        f->stats.bytes_dropped += f->total_length;
        if (f->dropped_before != UINT16_MAX) f->dropped_before++;
        f->open = false;
        return;
    }
    f->next_sequence = record.sequence + 1u;
    record.length = f->length;
    record.dropped_before = f->dropped_before;
    record.flags = f->flags | (f->dropped_before ? CAP_FLAG_LOSS_BEFORE : 0u);
    memcpy(record.payload, f->payload, f->length);

    f->stats.packets_completed++;
    if ((f->flags & CAP_FLAG_TRUNCATED) != 0u) {
        f->stats.packets_truncated++;
        f->stats.bytes_truncated += f->total_length - f->length;
    }

    /* New records stay in RAM while space remains. A separate task moves
       the oldest records to flash, preserving one global FIFO. */
    queued = gpmon_queue_push(q, &record) ? 1 : 0;
    if (!queued) queued = gpmon_flash_queue_push(&record) == 0;

    if (queued) {
        f->stats.packets_queued++;
        f->dropped_before = 0;
    } else {
        f->stats.packets_dropped++;
        f->stats.bytes_dropped += f->total_length;
        if (f->dropped_before != UINT16_MAX) f->dropped_before++;
    }
    f->open = false;
}

void gpmon_framer_feed(gpmon_framer_t *f, gpmon_queue_t *q,
                       const uint8_t *data, uint32_t length, uint64_t now_us)
{
    if (f == NULL || q == NULL || data == NULL || length == 0u) return;

    /* If the prior burst ended >=10ms ago, close it before this burst. */
    if (f->open && now_us >= f->last_byte_us &&
        now_us - f->last_byte_us >= GPMON_FRAME_GAP_US) {
        close_packet(f, q);
    }

    if (!f->open) {
        f->open = true;
        f->length = 0;
        f->total_length = 0;
        f->first_byte_us = now_us;
        f->flags = f->pending_flags;
        f->pending_flags = 0;
    }

    for (uint32_t i = 0; i < length; i++) {
        if (f->length < GPMON_MAX_PACKET_BYTES) {
            f->payload[f->length++] = data[i];
        } else {
            f->flags |= CAP_FLAG_TRUNCATED;
        }
        f->total_length++;
    }
    f->last_byte_us = now_us;
    f->stats.bytes_received += length;
}

void gpmon_framer_tick(gpmon_framer_t *f, gpmon_queue_t *q, uint64_t now_us)
{
    if (f == NULL || q == NULL) return;
    if (f->open && now_us >= f->last_byte_us &&
        now_us - f->last_byte_us >= GPMON_FRAME_GAP_US)
        close_packet(f, q);
}

void gpmon_framer_note_uart_error(gpmon_framer_t *f)
{
    if (f == NULL) return;
    f->stats.uart_error_events++;
    if (f->open) f->flags |= CAP_FLAG_UART_ERROR;
    else f->pending_flags |= CAP_FLAG_UART_ERROR;
}
