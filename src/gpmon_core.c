#include "gpmon_core.h"

void gpmon_core_init(gpmon_core_t *core)
{
    if (core == NULL) return;
    if (!gpmon_queue_init(&core->queue)) return;
    gpmon_framer_init(&core->framer);
}

void gpmon_core_rx_burst(gpmon_core_t *core, const uint8_t *data,
                         uint32_t length, uint64_t monotonic_us)
{
    if (core == NULL) return;
    gpmon_framer_feed(&core->framer, &core->queue, data, length, monotonic_us);
}

void gpmon_core_tick(gpmon_core_t *core, uint64_t monotonic_us)
{
    if (core == NULL) return;
    gpmon_framer_tick(&core->framer, &core->queue, monotonic_us);
}
