#ifndef GPMON_CAPTURE_QUEUE_H
#define GPMON_CAPTURE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gpmon_config.h"

#define CAP_FLAG_TRUNCATED   0x01u
#define CAP_FLAG_UART_ERROR  0x02u
#define CAP_FLAG_LOSS_BEFORE 0x04u

typedef struct {
    uint64_t first_byte_us;
    uint32_t sequence;
    uint16_t length;
    uint16_t dropped_before;
    uint8_t flags;
    uint8_t payload[GPMON_MAX_PACKET_BYTES];
} gpmon_record_t;

typedef struct {
    gpmon_record_t *slots;
    uint32_t capacity;
    volatile uint32_t head; /* producer publishes; consumer reads */
    volatile uint32_t tail; /* consumer publishes; producer reads */
    uint32_t high_water; /* producer-owned */
} gpmon_queue_t;

bool gpmon_queue_init(gpmon_queue_t *q);
bool gpmon_queue_push(gpmon_queue_t *q, const gpmon_record_t *record);
const gpmon_record_t *gpmon_queue_peek(const gpmon_queue_t *q);
void gpmon_queue_release(gpmon_queue_t *q);
uint32_t gpmon_queue_count(const gpmon_queue_t *q);
uint32_t gpmon_queue_high_water(const gpmon_queue_t *q);

#endif
