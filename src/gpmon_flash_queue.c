#include "gpmon_flash_queue.h"

#include "lfs.h"
#include "lfs_port.h"
#include "bflb_mtd.h"
#include "bflb_flash.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define SPOOL_LOG_PATH   "/gpmon_spool.bin"
#define SPOOL_TMP_PATH   "/gpmon_spool_new.bin"
#define SPOOL_OLD_PATH   "/gpmon_spool_old.bin"
#define SPOOL_STATE_A    "/gpmon_spool_state_a.bin"
#define SPOOL_STATE_B    "/gpmon_spool_state_b.bin"
#define SPOOL_ENTRY_MAGIC 0x47504D52u
#define SPOOL_STATE_MAGIC 0x47504D53u
#define SPOOL_VERSION     1u
#define SPOOL_BLOCK_SIZE  4096u

typedef struct {
    uint32_t magic;
    uint32_t crc;
    gpmon_record_t record;
} spool_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t generation;
    uint32_t head_offset;
    uint32_t crc;
} spool_state_t;

static struct lfs_context s_lfs_ctx = { .partition_name = "media" };
static struct lfs_config s_lfs_cfg = {
    .read_size = 256,
    .prog_size = 256,
    .lookahead_size = 256,
    .cache_size = 512,
    .block_size = SPOOL_BLOCK_SIZE,
    .block_cycles = 500
};
static lfs_t *s_lfs;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_lfs_lock;
static spool_state_t s_state;
static uint32_t s_pending;
static uint32_t s_file_size;
static uint32_t s_write_failures;
static uint32_t s_corrupt_records;
static int s_ready;
static int s_spill_latched;

#ifdef LFS_THREADSAFE
static int spool_lfs_lock(const struct lfs_config *cfg)
{
    (void)cfg;
    return (s_lfs_lock != NULL &&
            xSemaphoreTake(s_lfs_lock, portMAX_DELAY) == pdTRUE)
           ? 0 : LFS_ERR_IO;
}

static int spool_lfs_unlock(const struct lfs_config *cfg)
{
    (void)cfg;
    if (s_lfs_lock != NULL) xSemaphoreGive(s_lfs_lock);
    return 0;
}

#endif

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc;
}

static uint32_t record_crc(const gpmon_record_t *r)
{
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc_update(crc, (const uint8_t *)&r->first_byte_us, sizeof(r->first_byte_us));
    crc = crc_update(crc, (const uint8_t *)&r->sequence, sizeof(r->sequence));
    crc = crc_update(crc, (const uint8_t *)&r->length, sizeof(r->length));
    crc = crc_update(crc, (const uint8_t *)&r->dropped_before, sizeof(r->dropped_before));
    crc = crc_update(crc, (const uint8_t *)&r->flags, sizeof(r->flags));
    /* Source ID occupies prior structure padding; exclude it from the legacy CRC. */
    crc = crc_update(crc, r->payload, r->length);
    return ~crc;
}

static uint32_t state_crc(const spool_state_t *state)
{
    return ~crc_update(0xFFFFFFFFu, (const uint8_t *)state,
                       offsetof(spool_state_t, crc));
}

static int file_size(const char *path, uint32_t *size)
{
    struct lfs_info info;
    int rc = lfs_stat(s_lfs, path, &info);
    if (rc == LFS_ERR_NOENT) {
        *size = 0;
        return 0;
    }
    if (rc != 0 || info.type != LFS_TYPE_REG)
        return -1;
    *size = (uint32_t)info.size;
    return 0;
}

static int read_state_file(const char *path, spool_state_t *state)
{
    lfs_file_t file;
    spool_state_t candidate;
    int rc = lfs_file_open(s_lfs, &file, path, LFS_O_RDONLY);
    if (rc != 0) return -1;
    int n = lfs_file_read(s_lfs, &file, &candidate, sizeof(candidate));
    int close_rc = lfs_file_close(s_lfs, &file);
    if (n != (int)sizeof(candidate) || close_rc != 0 ||
        candidate.magic != SPOOL_STATE_MAGIC ||
        candidate.version != SPOOL_VERSION ||
        candidate.crc != state_crc(&candidate))
        return -1;
    *state = candidate;
    return 0;
}

static int store_state_locked(void)
{
    spool_state_t next = s_state;
    lfs_file_t file;
    const char *path;

    next.magic = SPOOL_STATE_MAGIC;
    next.version = SPOOL_VERSION;
    next.generation = s_state.generation + 1u;
    next.crc = state_crc(&next);
    path = (next.generation & 1u) ? SPOOL_STATE_B : SPOOL_STATE_A;

    int rc = lfs_file_open(s_lfs, &file, path,
                           LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC);
    if (rc != 0) return -1;
    int n = lfs_file_write(s_lfs, &file, &next, sizeof(next));
    int close_rc = lfs_file_close(s_lfs, &file);
    if (n != (int)sizeof(next) || close_rc != 0) return -1;
    s_state = next;
    return 0;
}

static int partition_is_blank(uint32_t address, uint32_t size)
{
    uint8_t buf[256];
    for (uint32_t off = 0; off < size; off += sizeof(buf)) {
        uint32_t n = size - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        if (bflb_flash_read(address + off, buf, n) != 0) return 0;
        for (uint32_t i = 0; i < n; i++)
            if (buf[i] != 0xFFu) return 0;
    }
    return 1;
}

static int ensure_log_file_locked(void)
{
    uint32_t ignored;
    lfs_file_t file;

    /* Recover interrupted compaction. A missing main file plus a new file
       means the rename of the old log already happened. */
    if (file_size(SPOOL_LOG_PATH, &ignored) != 0) return -1;
    uint32_t main_size = ignored;
    uint32_t tmp_size = 0, old_size = 0;
    if (file_size(SPOOL_TMP_PATH, &tmp_size) != 0 ||
        file_size(SPOOL_OLD_PATH, &old_size) != 0) return -1;

    if (main_size == 0 && old_size != 0 && tmp_size == 0) {
        if (lfs_rename(s_lfs, SPOOL_OLD_PATH, SPOOL_LOG_PATH) != 0)
            return -1;
        old_size = 0;
        main_size = old_size;
    } else if (main_size == 0 && tmp_size != 0) {
        if (lfs_rename(s_lfs, SPOOL_TMP_PATH, SPOOL_LOG_PATH) != 0)
            return -1;
        main_size = tmp_size;
        tmp_size = 0;
    } else if (tmp_size != 0) {
        (void)lfs_remove(s_lfs, SPOOL_TMP_PATH);
    }

    if (old_size != 0) (void)lfs_remove(s_lfs, SPOOL_OLD_PATH);

    int rc = lfs_file_open(s_lfs, &file, SPOOL_LOG_PATH,
                           LFS_O_RDWR | LFS_O_CREAT);
    if (rc != 0) return -1;
    int32_t size = lfs_file_size(s_lfs, &file);
    if (size < 0) {
        lfs_file_close(s_lfs, &file);
        return -1;
    }

    uint32_t aligned = ((uint32_t)size / sizeof(spool_entry_t)) * sizeof(spool_entry_t);
    if (aligned != (uint32_t)size &&
        lfs_file_truncate(s_lfs, &file, aligned) != 0) {
        lfs_file_close(s_lfs, &file);
        return -1;
    }
    int close_rc = lfs_file_close(s_lfs, &file);
    if (close_rc != 0) return -1;
    s_file_size = aligned;
    return 0;
}

static int compact_locked(void)
{
    lfs_file_t source, target;
    uint32_t source_size;
    uint32_t remaining;
    int rc;

    if (s_pending == 0 || s_state.head_offset < 16384u ||
        s_state.head_offset < s_file_size / 4u)
        return 0;

    rc = lfs_file_open(s_lfs, &source, SPOOL_LOG_PATH, LFS_O_RDONLY);
    if (rc != 0) return -1;
    rc = lfs_file_open(s_lfs, &target, SPOOL_TMP_PATH,
                       LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC);
    if (rc != 0) {
        lfs_file_close(s_lfs, &source);
        return -1;
    }

    source_size = s_file_size;
    remaining = s_file_size - s_state.head_offset;
    if (lfs_file_seek(s_lfs, &source, s_state.head_offset, LFS_SEEK_SET) < 0) {
        lfs_file_close(s_lfs, &source);
        lfs_file_close(s_lfs, &target);
        return -1;
    }

    uint32_t copied = 0;
    spool_entry_t entry;
    while (copied < remaining) {
        int n = lfs_file_read(s_lfs, &source, &entry, sizeof(entry));
        if (n != (int)sizeof(entry) ||
            lfs_file_write(s_lfs, &target, &entry, sizeof(entry)) != (int)sizeof(entry)) {
            lfs_file_close(s_lfs, &source);
            lfs_file_close(s_lfs, &target);
            (void)lfs_remove(s_lfs, SPOOL_TMP_PATH);
            return -1;
        }
        copied += sizeof(entry);
    }
    int source_close = lfs_file_close(s_lfs, &source);
    int target_close = lfs_file_close(s_lfs, &target);
    if (source_close != 0 || target_close != 0) {
        (void)lfs_remove(s_lfs, SPOOL_TMP_PATH);
        return -1;
    }

    /* Persist head=0 before renaming. A reset at this point can only cause
       duplicates, never loss of still-pending records. */
    uint32_t old_head = s_state.head_offset;
    s_state.head_offset = 0;
    if (store_state_locked() != 0) {
        s_state.head_offset = old_head;
        (void)lfs_remove(s_lfs, SPOOL_TMP_PATH);
        return -1;
    }

    (void)lfs_remove(s_lfs, SPOOL_OLD_PATH);
    if (lfs_rename(s_lfs, SPOOL_LOG_PATH, SPOOL_OLD_PATH) != 0) {
        s_file_size = source_size;
        s_pending = source_size / sizeof(spool_entry_t);
        s_spill_latched = s_pending != 0;
        return -1;
    }
    if (lfs_rename(s_lfs, SPOOL_TMP_PATH, SPOOL_LOG_PATH) != 0) {
        (void)lfs_rename(s_lfs, SPOOL_OLD_PATH, SPOOL_LOG_PATH);
        s_file_size = source_size;
        s_pending = source_size / sizeof(spool_entry_t);
        s_spill_latched = s_pending != 0;
        return -1;
    }
    (void)lfs_remove(s_lfs, SPOOL_OLD_PATH);
    s_file_size = remaining;
    s_state.head_offset = 0;
    s_spill_latched = s_pending != 0;
    return 0;
}

int gpmon_flash_queue_init(void)
{
    bflb_mtd_handle_t handle;
    bflb_mtd_info_t info;
    spool_state_t a, b;
    int have_a, have_b;
    int rc;

    if (s_ready) return 0;
    s_lock = xSemaphoreCreateMutex();
    s_lfs_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_lfs_lock == NULL) return -1;

    rc = bflb_mtd_open("media", &handle, BFLB_MTD_OPEN_FLAG_BUSADDR);
    if (rc < 0) {
        printf("GPMon flash spool: media partition not found\r\n");
        return -1;
    }
    memset(&info, 0, sizeof(info));
    rc = bflb_mtd_info(handle, &info);
    bflb_mtd_close(handle);
    if (rc < 0 || info.size < SPOOL_BLOCK_SIZE) return -1;

    s_lfs_ctx.flash_addr = info.offset;
    s_lfs_cfg.block_count = info.size / SPOOL_BLOCK_SIZE;
    s_lfs_cfg.context = &s_lfs_ctx;
    s_lfs_cfg.read = lfs_xip_flash_read;
    s_lfs_cfg.prog = lfs_xip_flash_prog;
    s_lfs_cfg.erase = lfs_xip_flash_erase;
    s_lfs_cfg.sync = lfs_xip_flash_sync;
#ifdef LFS_THREADSAFE
    s_lfs_cfg.lock = spool_lfs_lock;
    s_lfs_cfg.unlock = spool_lfs_unlock;
#endif

    s_lfs = &s_lfs_ctx.lfs;
    rc = lfs_mount(s_lfs, &s_lfs_cfg);
    if (rc != 0) {
        if (!partition_is_blank(info.offset, info.size)) {
            printf("GPMon flash spool: media is nonblank but LittleFS mount failed; refusing to format\r\n");
            return -1;
        }
        rc = lfs_format(s_lfs, &s_lfs_cfg);
        if (rc != 0 || lfs_mount(s_lfs, &s_lfs_cfg) != 0) {
            printf("GPMon flash spool: LittleFS format/mount failed\r\n");
            return -1;
        }
    }

    /* No other task can access this module before the scheduler starts. */
    if (ensure_log_file_locked() != 0) {
        printf("GPMon flash spool: cannot open spool log\r\n");
        return -1;
    }

    have_a = read_state_file(SPOOL_STATE_A, &a) == 0;
    have_b = read_state_file(SPOOL_STATE_B, &b) == 0;
    if (have_a && have_b)
        s_state = (a.generation >= b.generation) ? a : b;
    else if (have_a)
        s_state = a;
    else if (have_b)
        s_state = b;
    else {
        memset(&s_state, 0, sizeof(s_state));
        s_state.magic = SPOOL_STATE_MAGIC;
        s_state.version = SPOOL_VERSION;
    }

    if (s_state.head_offset > s_file_size ||
        (s_state.head_offset % sizeof(spool_entry_t)) != 0)
        s_state.head_offset = 0; /* preserve data; may replay duplicates */

    s_pending = (s_file_size - s_state.head_offset) / sizeof(spool_entry_t);
    s_spill_latched = s_pending != 0;

    if (s_pending == 0 && s_file_size != 0) {
        s_state.head_offset = 0;
        if (store_state_locked() == 0) {
            lfs_file_t file;
            if (lfs_file_open(s_lfs, &file, SPOOL_LOG_PATH, LFS_O_RDWR) == 0) {
                (void)lfs_file_truncate(s_lfs, &file, 0);
                (void)lfs_file_close(s_lfs, &file);
                s_file_size = 0;
            }
        }
    } else if (!have_a && !have_b) {
        (void)store_state_locked();
    }

    s_ready = 1;
    printf("GPMon flash spool: ready, partition=%lu bytes, pending=%lu records\r\n",
           (unsigned long)info.size, (unsigned long)s_pending);
    return 0;
}

int gpmon_flash_queue_pending(uint32_t *count)
{
    if (count == NULL) return -1;
    if (!s_ready) {
        *count = s_spill_latched ? 1u : 0u;
        return -1;
    }
    *count = s_spill_latched ? 1u : 0u;
    *count = s_spill_latched ? 1u : 0u;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return -1;
    *count = s_pending;
    if (s_spill_latched && *count == 0) *count = 1u;
    xSemaphoreGive(s_lock);
    return 0;
}

int gpmon_flash_queue_push(const gpmon_record_t *record)
{
    lfs_file_t file;
    spool_entry_t entry;
    int rc;

    if (!s_ready || record == NULL || record->length == 0 ||
        record->length > GPMON_MAX_PACKET_BYTES)
        return -1;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return -1;

    if (s_pending == 0 && s_file_size != 0) {
        s_state.head_offset = 0;
        if (store_state_locked() != 0) {
            xSemaphoreGive(s_lock);
            s_write_failures++;
            return -1;
        }
        rc = lfs_file_open(s_lfs, &file, SPOOL_LOG_PATH, LFS_O_RDWR);
        if (rc != 0 || lfs_file_truncate(s_lfs, &file, 0) != 0) {
            if (rc == 0) lfs_file_close(s_lfs, &file);
            xSemaphoreGive(s_lock);
            s_write_failures++;
            return -1;
        }
        if (lfs_file_close(s_lfs, &file) != 0) {
            xSemaphoreGive(s_lock);
            s_write_failures++;
            return -1;
        }
        s_file_size = 0;
    }

    memset(&entry, 0, sizeof(entry));
    entry.magic = SPOOL_ENTRY_MAGIC;
    entry.record = *record;
    entry.crc = record_crc(&entry.record);

    rc = lfs_file_open(s_lfs, &file, SPOOL_LOG_PATH, LFS_O_RDWR | LFS_O_CREAT);
    if (rc != 0) {
        s_write_failures++;
        xSemaphoreGive(s_lock);
        return -1;
    }
    int32_t size = lfs_file_size(s_lfs, &file);
    if (size < 0) {
        (void)lfs_file_close(s_lfs, &file);
        s_write_failures++;
        xSemaphoreGive(s_lock);
        return -1;
    }
    uint32_t aligned_size = ((uint32_t)size / sizeof(spool_entry_t)) * sizeof(spool_entry_t);
    if ((uint32_t)size != aligned_size &&
        lfs_file_truncate(s_lfs, &file, aligned_size) != 0) {
        (void)lfs_file_close(s_lfs, &file);
        s_write_failures++;
        xSemaphoreGive(s_lock);
        return -1;
    }
    if (lfs_file_seek(s_lfs, &file, aligned_size, LFS_SEEK_SET) < 0 ||
        lfs_file_write(s_lfs, &file, &entry, sizeof(entry)) != (int)sizeof(entry)) {
        (void)lfs_file_close(s_lfs, &file);
        s_write_failures++;
        xSemaphoreGive(s_lock);
        return -1;
    }
    rc = lfs_file_close(s_lfs, &file);
    if (rc != 0) {
        s_write_failures++;
        xSemaphoreGive(s_lock);
        return -1;
    }

    s_file_size = aligned_size + sizeof(entry);
    s_pending++;
    s_spill_latched = 1;
    xSemaphoreGive(s_lock);
    return 0;
}

int gpmon_flash_queue_peek(gpmon_record_t *record)
{
    if (!s_ready || record == NULL) return -1;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return -1;

    while (s_pending != 0) {
        lfs_file_t file;
        spool_entry_t entry;
        int rc = lfs_file_open(s_lfs, &file, SPOOL_LOG_PATH, LFS_O_RDONLY);
        if (rc != 0) {
            xSemaphoreGive(s_lock);
            return -1;
        }
        int32_t seek_rc = lfs_file_seek(s_lfs, &file, s_state.head_offset, LFS_SEEK_SET);
        int n = seek_rc < 0 ? -1 : lfs_file_read(s_lfs, &file, &entry, sizeof(entry));
        int close_rc = lfs_file_close(s_lfs, &file);
        if (n != (int)sizeof(entry) || close_rc != 0 ||
            entry.magic != SPOOL_ENTRY_MAGIC ||
            entry.record.length == 0 ||
            entry.record.length > GPMON_MAX_PACKET_BYTES ||
            entry.crc != record_crc(&entry.record)) {
            s_corrupt_records++;
            s_state.head_offset += sizeof(entry);
            s_pending--;
            if (store_state_locked() != 0) {
                xSemaphoreGive(s_lock);
                return -1;
            }
            continue;
        }
        *record = entry.record;
        /* Legacy spool entries predate source IDs; interpret them as UART1. */
        if (record->source_id_valid != 0xA5u) {
            record->source_id = 1u;
            record->source_id_valid = 0xA5u;
        }
        xSemaphoreGive(s_lock);
        return 1;
    }

    s_spill_latched = 0;
    xSemaphoreGive(s_lock);
    return 0;
}

int gpmon_flash_queue_ack(void)
{
    if (!s_ready) return -1;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return -1;
    if (s_pending == 0) {
        xSemaphoreGive(s_lock);
        return -1;
    }

    s_state.head_offset += sizeof(spool_entry_t);
    s_pending--;
    s_spill_latched = s_pending != 0;

    if (s_pending == 0) {
        /* Reset state first. A reset before truncation can only replay
           acknowledged records; it cannot skip unacknowledged ones. */
        s_state.head_offset = 0;
        int state_rc = store_state_locked();
        if (state_rc == 0) {
            lfs_file_t file;
            if (lfs_file_open(s_lfs, &file, SPOOL_LOG_PATH, LFS_O_RDWR) == 0) {
                if (lfs_file_truncate(s_lfs, &file, 0) == 0) s_file_size = 0;
                (void)lfs_file_close(s_lfs, &file);
            }
        }
        xSemaphoreGive(s_lock);
        return state_rc;
    }

    int state_rc = store_state_locked();
    if (state_rc == 0 && compact_locked() != 0)
        s_write_failures++;
    xSemaphoreGive(s_lock);
    return state_rc;
}

uint32_t gpmon_flash_queue_write_failures(void)
{
    return s_write_failures;
}

uint32_t gpmon_flash_queue_corrupt_records(void)
{
    return s_corrupt_records;
}

uint32_t gpmon_flash_queue_used_bytes(void)
{
    uint32_t used = 0;
    if (!s_ready || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return 0;
    used = s_file_size;
    xSemaphoreGive(s_lock);
    return used;
}


/* Migrate oldest RAM entries only after acquiring exclusive ownership of the
 * queue head. Release RAM only after the flash append has succeeded. */
void gpmon_flash_queue_spill_task(void *arg)
{
    gpmon_queue_t *queue = (gpmon_queue_t *)arg;
    if (queue == NULL) { vTaskDelete(NULL); return; }

    for (;;) {
        uint32_t pending = 0;
        (void)gpmon_flash_queue_pending(&pending);
        uint32_t threshold = queue->capacity < GPMON_FLASH_SPILL_THRESHOLD_RECORDS
                             ? queue->capacity : GPMON_FLASH_SPILL_THRESHOLD_RECORDS;
        uint32_t count = gpmon_queue_count(queue);

        if (count != 0u && count >= threshold) {
            gpmon_record_t record;
            if (gpmon_queue_claim_head(queue, &record)) {
                if (gpmon_flash_queue_push(&record) == 0) {
                    gpmon_queue_finish_claim(queue);
                } else {
                    gpmon_queue_cancel_claim(queue);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
