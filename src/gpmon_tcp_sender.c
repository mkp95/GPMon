
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include "gpmon_tcp_sender.h"

#include "gpmon_server_config.h"
#include "gpmon_config.h"
#include "capture_queue.h"
#include "bflb_mtimer.h"
#include "bflb_sec_trng.h"
#include "lfs_kv.h"
#include "gpmon_flash_queue.h"
#include "web_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#define GPMON_MAGIC       "GPM1"
#define GPMON_VERSION     1u
#define GPMON_TYPE_DATA   1u
#define GPMON_TYPE_ACK    2u
#define GPMON_ENVELOPE    6u
#define GPMON_DATA_HEADER 29u
#define GPMON_ACK_SIZE    20u
#define GPMON_FRAME_MAX   (GPMON_ENVELOPE + GPMON_DATA_HEADER + GPMON_MAX_PACKET_BYTES)

static uint8_t s_device_id[2];
static uint64_t s_epoch_base;
static uint64_t s_uptime_base;
static int s_epoch_valid;

#define GPMON_SEQ_KEY     "gpmon/tx_sequence"
#define GPMON_SEQ_MAGIC   0x47505351u
#define GPMON_SEQ_VERSION 1u
#define GPMON_SEQ_BLOCK   10u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t device_id[2];
    uint8_t reserved[2];
    uint32_t next_reserved;
} gpmon_seq_store_t;

static uint32_t s_next_sequence;
static uint32_t s_sequence_limit;

static int persist_sequence_store(const gpmon_seq_store_t *state)
{
    return lfs_kv_set_blob(GPMON_SEQ_KEY, state, sizeof(*state)) == LFS_KV_OK
           ? 0 : -1;
}

/* Wait for LittleFS KV to be ready, then load/create the persistent identity. */
static int sequence_storage_init(void)
{
    gpmon_seq_store_t state;
    size_t saved_len = 0;
    size_t got;

    for (;;) {
        gpmon_server_config_t cfg;
        int rc = gpmon_server_config_get(&cfg);

        if (rc >= 0)
            break;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    memset(&state, 0, sizeof(state));
    got = lfs_kv_get_blob(GPMON_SEQ_KEY, &state, sizeof(state), &saved_len);

    if (got != sizeof(state) || saved_len != sizeof(state) ||
        state.magic != GPMON_SEQ_MAGIC ||
        state.version != GPMON_SEQ_VERSION) {
        uint32_t random_value;

        if (bflb_trng_readlen((uint8_t *)&random_value,
                              sizeof(random_value)) != 0)
            return -1;

        memset(&state, 0, sizeof(state));
        state.magic = GPMON_SEQ_MAGIC;
        state.version = GPMON_SEQ_VERSION;
        state.device_id[0] = (uint8_t)(random_value >> 8);
        state.device_id[1] = (uint8_t)random_value;
        state.next_reserved = 0;
    }

    memcpy(s_device_id, state.device_id, sizeof(s_device_id));

    /* Skip the remainder of the previous reservation after reboot. */
    s_next_sequence = state.next_reserved;
    state.next_reserved += GPMON_SEQ_BLOCK;

    if (persist_sequence_store(&state) != 0)
        return -1;

    s_sequence_limit = state.next_reserved;
    return 0;
}

static int reserve_more_sequences(void)
{
    gpmon_seq_store_t state;
    size_t saved_len = 0;
    size_t got;

    memset(&state, 0, sizeof(state));
    got = lfs_kv_get_blob(GPMON_SEQ_KEY, &state, sizeof(state), &saved_len);
    if (got != sizeof(state) || saved_len != sizeof(state) ||
        state.magic != GPMON_SEQ_MAGIC ||
        state.version != GPMON_SEQ_VERSION ||
        memcmp(state.device_id, s_device_id, sizeof(s_device_id)) != 0)
        return -1;

    state.next_reserved += GPMON_SEQ_BLOCK;
    if (persist_sequence_store(&state) != 0) return -1;
    s_sequence_limit = state.next_reserved;
    return 0;
}

static int s_sequence_ready;

int gpmon_tcp_sequence_init(void)
{
    if (s_sequence_ready) return 0;
    if (sequence_storage_init() != 0) return -1;
    s_sequence_ready = 1;
    return 0;
}

int gpmon_tcp_sequence_next(uint32_t *out)
{
    if (!s_sequence_ready || out == NULL) return -1;
    if (s_next_sequence == s_sequence_limit && reserve_more_sequences() != 0)
        return -1;
    *out = s_next_sequence++;
    return 0;
}

int gpmon_tcp_sequence_ready(void)
{
    return s_sequence_ready;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56u - 8u * i));
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static uint32_t crc32_bytes(const uint8_t *data, uint16_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint16_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return ~crc;
}

static uint64_t packet_epoch(uint64_t packet_us)
{
    uint64_t delta_seconds;

    if (!s_epoch_valid)
        return 0;

    /*
     * s_epoch_base is the server epoch sampled at s_uptime_base.
     * Queued packets may be older than that sample (especially after an
     * outage). Subtract their age instead of clamping every old packet to
     * the same epoch, which would erase timestamp differences in the backlog.
     */
    if (packet_us <= s_uptime_base) {
        delta_seconds = (s_uptime_base - packet_us) / 1000000ull;
        return delta_seconds > s_epoch_base ? 0 : s_epoch_base - delta_seconds;
    }

    return s_epoch_base + (packet_us - s_uptime_base) / 1000000ull;
}

static size_t make_data_frame(const gpmon_record_t *r, uint8_t *frame)
{
    uint16_t len = r->length;

    memcpy(frame, GPMON_MAGIC, 4);
    frame[4] = GPMON_VERSION;
    frame[5] = GPMON_TYPE_DATA;

    put_u32(frame + 6, r->sequence);
    frame[10] = s_device_id[0];
    frame[11] = s_device_id[1];
    frame[12] = r->source_id; /* 0 = daughter UART0, 1 = main-board UART1 */
    put_u16(frame + 13, len);
    put_u64(frame + 15, packet_epoch(r->first_byte_us));
    put_u64(frame + 23, r->first_byte_us);
    put_u32(frame + 31, crc32_bytes(r->payload, len));
    memcpy(frame + 35, r->payload, len);

    return 35u + len;
}

static int send_all(int fd, const uint8_t *data, size_t length)
{
    size_t done = 0;

    while (done < length) {
        int n = send(fd, data + done, length - done, 0);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *data, size_t length)
{
    size_t done = 0;

    while (done < length) {
        int n = recv(fd, data + done, length - done, 0);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int check_ack(const uint8_t *ack, const gpmon_record_t *r,
                     uint64_t *server_epoch)
{
    if (memcmp(ack, GPMON_MAGIC, 4) != 0 ||
        ack[4] != GPMON_VERSION ||
        ack[5] != GPMON_TYPE_ACK ||
        ack[6] != s_device_id[0] ||
        ack[7] != s_device_id[1] ||
        get_u32(ack + 8) != r->sequence) {
        return -1;
    }

    *server_epoch = get_u64(ack + 12);
    return 0;
}

static void close_socket(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int connect_server(const gpmon_server_config_t *cfg)
{
    struct sockaddr_in addr;
    struct timeval timeout;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;

    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    addr.sin_addr.s_addr = inet_addr(cfg->ip);

    if (addr.sin_addr.s_addr == INADDR_NONE ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void gpmon_tcp_sender_task(void *arg)
{
    gpmon_queue_t *queue = (gpmon_queue_t *)arg;
    gpmon_record_t record;
    uint8_t frame[GPMON_FRAME_MAX];
    uint8_t ack[GPMON_ACK_SIZE];
    gpmon_server_config_t cfg;
    char connected_ip[GPMON_SERVER_IP_MAX] = "";
    uint16_t connected_port = 0;
    int fd = -1;

    while (!gpmon_tcp_sequence_ready() || queue == NULL || queue->slots == NULL)
        vTaskDelay(pdMS_TO_TICKS(100));

    printf("TCP sender device ID: %02X%02X, next sequence=%lu\r\n",
           s_device_id[0], s_device_id[1],
           (unsigned long)s_next_sequence);
    printf("TCP sender: consuming shared RAM capture queue; flash is overflow backlog\r\n");

    for (;;) {
        uint32_t flash_pending = 0;
        (void)gpmon_flash_queue_pending(&flash_pending);
        int source = 0; /* 0 = RAM queue, 1 = flash spool */

        /* Flash always contains older records than RAM while the spill
           worker is active, so it must be drained first. */
        if (flash_pending != 0u) {
            int flash_rc = gpmon_flash_queue_peek(&record);
            if (flash_rc <= 0) {
                vTaskDelay(pdMS_TO_TICKS(flash_rc < 0 ? 250 : 10));
                continue;
            }
            source = 1;
        } else if (!gpmon_queue_claim_head(queue, &record)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (record.length == 0 || record.length > GPMON_MAX_PACKET_BYTES) {
            printf("TCP: invalid queued record; discarding source=%d\r\n", source);
            if (source == 0) gpmon_queue_finish_claim(queue);
            else (void)gpmon_flash_queue_ack();
            continue;
        }

        web_config_publish_packet(&record);
        size_t frame_len = make_data_frame(&record, frame);

        int retry_select = 0;
        for (;;) {
            if (gpmon_server_config_get(&cfg) < 0 ||
                cfg.ip[0] == '\0' || cfg.port == 0) {
                close_socket(&fd);
                if (source == 0) gpmon_queue_cancel_claim(queue);
                vTaskDelay(pdMS_TO_TICKS(2000));
                retry_select = 1;
                break;
            }

            if (fd >= 0 &&
                (strcmp(cfg.ip, connected_ip) != 0 ||
                 cfg.port != connected_port)) {
                close_socket(&fd);
            }

            if (fd < 0) {
                fd = connect_server(&cfg);
                if (fd < 0) {
                    printf("TCP: connect to %s:%u failed; retrying\r\n",
                           cfg.ip, (unsigned)cfg.port);
                    if (source == 0) gpmon_queue_cancel_claim(queue);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    retry_select = 1;
                    break;
                }

                strncpy(connected_ip, cfg.ip, sizeof(connected_ip) - 1);
                connected_ip[sizeof(connected_ip) - 1] = '\0';
                connected_port = cfg.port;
                printf("TCP: connected to %s:%u\r\n",
                       connected_ip, (unsigned)connected_port);
            }

            if (send_all(fd, frame, frame_len) == 0 &&
                recv_all(fd, ack, sizeof(ack)) == 0) {
                uint64_t server_epoch;

                if (check_ack(ack, &record, &server_epoch) == 0) {
                    s_epoch_base = server_epoch;
                    s_uptime_base = bflb_mtimer_get_time_us();
                    s_epoch_valid = 1;
                    printf("TCP: ACK seq=%lu source=%s\r\n",
                           (unsigned long)record.sequence,
                           source == 0 ? "ram" : "flash");
                    if (source == 0) {
                        gpmon_queue_finish_claim(queue);
                    } else if (gpmon_flash_queue_ack() != 0) {
                        printf("TCP: flash ACK cursor save failed; duplicate replay may occur after reset\r\n");
                    }
                    break;
                }
            }

            printf("TCP: send/ACK failed for seq=%lu; retrying\r\n",
                   (unsigned long)record.sequence);
            close_socket(&fd);
            if (source == 0) gpmon_queue_cancel_claim(queue);
            vTaskDelay(pdMS_TO_TICKS(1000));
            retry_select = 1;
            break;
        }
        if (retry_select) continue;
    }
}
