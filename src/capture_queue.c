#include "capture_queue.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

_Static_assert(GPMON_MAX_PACKET_BYTES > 0u,
               "maximum packet size must be positive");

/*
 * Keep a reserve for Wi-Fi/TCP activity and task/runtime allocations. This
 * queue is created from the UART task after the system's other tasks exist.
 */
#define GPMON_QUEUE_HEAP_RESERVE_BYTES (16u * 1024u)
#define GPMON_QUEUE_HEAP_FRACTION_NUM  60u
#define GPMON_QUEUE_HEAP_FRACTION_DEN  100u

bool gpmon_queue_init(gpmon_queue_t *q)
{
    size_t free_bytes, budget, target_bytes;
    uint32_t capacity;

    if (q == NULL) return false;
    memset(q, 0, sizeof(*q));

    free_bytes = (size_t)xPortGetFreeHeapSize();
    if (free_bytes <= GPMON_QUEUE_HEAP_RESERVE_BYTES) return false;

    budget = free_bytes - GPMON_QUEUE_HEAP_RESERVE_BYTES;
    target_bytes = (budget * GPMON_QUEUE_HEAP_FRACTION_NUM) /
                   GPMON_QUEUE_HEAP_FRACTION_DEN;
    /* The requested 50 KiB includes metadata, not payload bytes alone. */
    if (target_bytes > GPMON_CAPTURE_QUEUE_TARGET_BYTES)
        target_bytes = GPMON_CAPTURE_QUEUE_TARGET_BYTES;
    capacity = (uint32_t)(target_bytes / sizeof(gpmon_record_t));
    if (capacity < GPMON_QUEUE_DEPTH) return false;

    /* Be defensive against heap fragmentation and allocator overhead. */
    while (capacity >= GPMON_QUEUE_DEPTH) {
        q->slots = (gpmon_record_t *)pvPortMalloc(
            (size_t)capacity * sizeof(gpmon_record_t));
        if (q->slots != NULL) {
            q->capacity = capacity;
            memset(q->slots, 0, (size_t)capacity * sizeof(gpmon_record_t));
            return true;
        }
        capacity = (capacity * 3u) / 4u;
    }

    q->capacity = 0;
    return false;
}

uint32_t gpmon_queue_count(const gpmon_queue_t *q)
{
    return q != NULL ? q->head - q->tail : 0;
}

uint32_t gpmon_queue_high_water(const gpmon_queue_t *q)
{
    return q != NULL ? q->high_water : 0;
}

bool gpmon_queue_push(gpmon_queue_t *q, const gpmon_record_t *record)
{
    uint32_t head, tail, used;
    gpmon_record_t *slot;

    if (q == NULL || q->slots == NULL || q->capacity == 0 ||
        record == NULL || record->length > GPMON_MAX_PACKET_BYTES)
        return false;

    head = q->head;
    tail = q->tail;
    used = head - tail;
    if (used >= q->capacity) return false;

    slot = &q->slots[head % q->capacity];
    slot->first_byte_us = record->first_byte_us;
    slot->sequence = record->sequence;
    slot->length = record->length;
    slot->dropped_before = record->dropped_before;
    slot->flags = record->flags;
    memcpy(slot->payload, record->payload, record->length);

    /* Single producer publishes the fully populated slot last. */
    q->head = head + 1u;
    if (used + 1u > q->high_water) q->high_water = used + 1u;
    return true;
}

const gpmon_record_t *gpmon_queue_peek(const gpmon_queue_t *q)
{
    if (q == NULL || q->slots == NULL || q->tail == q->head) return NULL;
    return &q->slots[q->tail % q->capacity];
}

void gpmon_queue_release(gpmon_queue_t *q)
{
    if (q != NULL && q->tail != q->head) q->tail++;
}
