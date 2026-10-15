#include "capture_queue.h"
#include "FreeRTOS.h"
#include <string.h>

_Static_assert(GPMON_MAX_PACKET_BYTES > 0u,
               "maximum packet size must be positive");

/*
 * The selected FreeRTOS heap_3 allocator delegates to libc malloc/free and
 * does not provide xPortGetFreeHeapSize(). Bound the request to the configured
 * target and fall back in smaller steps if the allocator cannot satisfy it.
 * This is best-effort: heap_3 exposes no portable free-heap query, so a fixed
 * reserve cannot be measured or guaranteed here.
 */
bool gpmon_queue_init(gpmon_queue_t *q)
{
    size_t target_bytes;
    uint32_t capacity;

    if (q == NULL) return false;
    memset(q, 0, sizeof(*q));

    /* Includes record metadata, not just packet payload bytes. */
    target_bytes = GPMON_CAPTURE_QUEUE_TARGET_BYTES;
    capacity = (uint32_t)(target_bytes / sizeof(gpmon_record_t));
    if (capacity < GPMON_QUEUE_DEPTH) return false;

    while (capacity >= GPMON_QUEUE_DEPTH) {
        size_t allocation_bytes = (size_t)capacity * sizeof(gpmon_record_t);

        q->slots = (gpmon_record_t *)pvPortMalloc(allocation_bytes);
        if (q->slots != NULL) {
            q->capacity = capacity;
            memset(q->slots, 0, allocation_bytes);
            return true;
        }

        /* Retry at 75% of the previous size after an allocation failure. */
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

    /* Publish record bytes before making the slot visible to the sender task. */
    __sync_synchronize();
    q->head = head + 1u;
    if (used + 1u > q->high_water) q->high_water = used + 1u;
    return true;
}

const gpmon_record_t *gpmon_queue_peek(const gpmon_queue_t *q)
{
    if (q == NULL || q->slots == NULL || q->tail == q->head) return NULL;
    /* Pair with the producer's publish barrier before reading slot contents. */
    __sync_synchronize();
    return &q->slots[q->tail % q->capacity];
}

void gpmon_queue_release(gpmon_queue_t *q)
{
    if (q != NULL && q->tail != q->head) {
        /* Ensure the consumer has finished reading before freeing the slot. */
        __sync_synchronize();
        q->tail++;
    }
}
