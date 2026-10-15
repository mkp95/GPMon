#ifndef GPMON_CORE_H
#define GPMON_CORE_H
#include "capture_queue.h"
#include "packet_framer.h"

typedef struct {
    gpmon_queue_t queue;
    gpmon_framer_t framer;
} gpmon_core_t;

void gpmon_core_init(gpmon_core_t *core);
void gpmon_core_rx_burst(gpmon_core_t *core, const uint8_t *data,
                         uint32_t length, uint64_t monotonic_us);
void gpmon_core_tick(gpmon_core_t *core, uint64_t monotonic_us);

#endif
