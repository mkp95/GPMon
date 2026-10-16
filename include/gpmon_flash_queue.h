#ifndef GPMON_FLASH_QUEUE_H
#define GPMON_FLASH_QUEUE_H

#include <stdint.h>
#include "capture_queue.h"

/* Must be called after bflb_mtd_init(). Returns 0 only when the media
 * partition is mounted and the durable packet spool is ready. */
int gpmon_flash_queue_init(void);

/* Returns 0 on success and writes the pending count. On error, count is set
 * nonzero if this runtime knows a flash backlog exists, preventing newer RAM
 * packets from overtaking it. */
int gpmon_flash_queue_pending(uint32_t *count);

int gpmon_flash_queue_push(const gpmon_record_t *record);
void gpmon_flash_queue_spill_task(void *arg);
/* peek: 1 = record returned, 0 = empty, -1 = storage error. */
int gpmon_flash_queue_peek(gpmon_record_t *record);
/* Call only after the corresponding record receives a valid server ACK. */
int gpmon_flash_queue_ack(void);

uint32_t gpmon_flash_queue_write_failures(void);
uint32_t gpmon_flash_queue_corrupt_records(void);
uint32_t gpmon_flash_queue_used_bytes(void);

#endif
