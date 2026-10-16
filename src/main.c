#include "board.h"
#include "mm.h"
#include "bflb_mtimer.h"
#include "gpmon_core.h"
#include "uart_capture.h"
#include "gpmon_config.h"
#include "wifi_provision.h"
#include "web_config.h"
#include "gpmon_tcp_sender.h"
#include "gpmon_flash_queue.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <stdint.h>
#include <stdint.h>
#include "bflb_sec_trng.h"

int bl_rand(void)
{
    uint32_t value;

    if (bflb_trng_readlen((uint8_t *)&value, sizeof(value)) != 0) {
        return -1;
    }

    value >>= 1; /* keep sign bit clear */
    return (int)value;
}


static gpmon_core_t s_core;
static size_t s_min_free_ocram;

static unsigned long memory_allocation_failures(void)
{
#if CONFIG_MM_ENABLE_STATISTICS
    return (unsigned long)g_mem_manager.stats.total_malloc_failures;
#else
    return 0ul;
#endif
}

static void print_memory_stats(void)
{
    size_t free_bytes = kfree_size(MM_HEAP_OCRAM_0);

    if (s_min_free_ocram == 0u || free_bytes < s_min_free_ocram) {
        s_min_free_ocram = free_bytes;
    }

    printf("Memory OCRAM: free=%lu min_observed_free=%lu alloc_failures=%lu\r\n",
           (unsigned long)free_bytes,
           (unsigned long)s_min_free_ocram,
           memory_allocation_failures());
}

/* Pool walking for largest-block statistics disables interrupts, so do this
 * once before the scheduler starts and UART RX interrupts are enabled. */
static void print_startup_memory_stats(void)
{
    size_t free_bytes = kfree_size(MM_HEAP_OCRAM_0);

    s_min_free_ocram = free_bytes;
    printf("Memory OCRAM startup: free=%lu largest_block=%lu alloc_failures=%lu\r\n",
           (unsigned long)free_bytes,
           (unsigned long)kmax_free_size(MM_HEAP_OCRAM_0),
           memory_allocation_failures());
}

#if GPMON_VERBOSE_PACKET_LOG
static void print_packet(const gpmon_record_t *record)
{
    printf("\r\nPACKET seq=%lu len=%u flags=0x%02x first_us=%llu\r\n",
           (unsigned long)record->sequence,
           (unsigned int)record->length,
           (unsigned int)record->flags,
           (unsigned long long)record->first_byte_us);

    printf("HEX:");
    for (uint32_t i = 0; i < record->length; ++i) {
        printf(" %02X", record->payload[i]);
    }

    printf("\r\nASCII:");
    for (uint32_t i = 0; i < record->length; ++i) {
        uint8_t c = record->payload[i];
        putchar((c >= 32u && c <= 126u) ? (int)c : '.');
    }
    printf("\r\n");
}
#endif

static void uart_capture_task(void *arg)
{
    uint64_t next_stats_us;

    (void)arg;

    while (gpmon_tcp_sequence_init() != 0) {
        printf("Sequence storage not ready; retrying\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    gpmon_core_init(&s_core);
    if (s_core.queue.slots == NULL) {
        printf("ERROR: unable to allocate UART packet queue; capture not started\r\n");
        vTaskDelete(NULL);
        return;
    }
    printf("RAM capture queue: capacity=%lu records (%lu bytes)\r\n",
           (unsigned long)s_core.queue.capacity,
           (unsigned long)(s_core.queue.capacity * sizeof(gpmon_record_t)));

    printf("\r\nGPMon_v1 UART1 test\r\n");
    printf("UART1: 115200 8-N-1, RX=GPIO%u, TX=GPIO%u\r\n",
           (unsigned int)GPMON_UART_RX_PIN,
           (unsigned int)GPMON_UART_TX_PIN);
    printf("Send bytes to UART1 RX. A packet ends after %llu us silence.\r\n",
           (unsigned long long)GPMON_FRAME_GAP_US);

    if (!gpmon_uart_capture_init(&s_core)) {
        printf("ERROR: UART1/GPIO device lookup or init failed\r\n");
        vTaskDelete(NULL);
        return;
    }

    printf("UART1 RX interrupts enabled.\r\n");
    { gpmon_uart_capture_stats_t initial_stats = {0};
      web_config_publish_stats(&initial_stats, 0, 0); }
    next_stats_us = bflb_mtimer_get_time_us() + 1000000ull;

    for (;;) {
        gpmon_uart_capture_stats_t stats;

        /* The sender owns the single RAM backlog. Never drain it here:
           unacknowledged records remain queued until TCP ACK or flash spill. */
        gpmon_uart_capture_poll();

        if (bflb_mtimer_get_time_us() >= next_stats_us) {
            gpmon_uart_capture_get_stats(&stats);
            web_config_publish_stats(&stats,
                                     gpmon_queue_count(&s_core.queue),
                                     gpmon_queue_high_water(&s_core.queue));
            uint32_t flash_pending = 0;
            (void)gpmon_flash_queue_pending(&flash_pending);
            printf("UART stats: rx_bytes=%lu ring_overflows=%lu uart_errors=%lu "
                   "capture_queue=%lu/%lu high_water=%lu capture_bytes=%lu "
                   "capture_drops=%lu flash_pending=%lu flash_used=%lu "
                   "flash_write_failures=%lu flash_corrupt=%lu\r\n",
                   (unsigned long)stats.rx_bytes,
                   (unsigned long)stats.rx_ring_overflows,
                   (unsigned long)stats.uart_error_events,
                   (unsigned long)gpmon_queue_count(&s_core.queue),
                   (unsigned long)s_core.queue.capacity,
                   (unsigned long)gpmon_queue_high_water(&s_core.queue),
                   (unsigned long)(s_core.queue.capacity * sizeof(gpmon_record_t)),
                   (unsigned long)s_core.framer.stats.packets_dropped,
                   (unsigned long)flash_pending,
                   (unsigned long)gpmon_flash_queue_used_bytes(),
                   (unsigned long)gpmon_flash_queue_write_failures(),
                   (unsigned long)gpmon_flash_queue_corrupt_records());
            print_memory_stats();
            next_stats_us += 1000000ull;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void)
{
    board_init();

    printf("\r\nGPMon v1.0_17102026\r\n");

    if (xTaskCreate(uart_capture_task,
                    "uart_capture",
                    2048,
                    NULL,
                    12,
                    NULL) != pdPASS) {
        printf("ERROR: failed to create UART capture task\r\n");
        for (;;) {
            bflb_mtimer_delay_ms(1000);
        }
    }

    if (gpmon_wifi_provision_start() != 0) {
        printf("WARNING: Wi-Fi provisioning unavailable; UART capture will continue\r\n");
    }

    if (xTaskCreate(gpmon_flash_queue_spill_task,
                    "gpmon_spill",
                    1024,
                    &s_core.queue,
                    10,
                    NULL) != pdPASS) {
        printf("WARNING: flash FIFO spill task creation failed\\r\\n");
    }

    if (xTaskCreate(gpmon_tcp_sender_task,
                    "gpmon_tcp",
                    2048,
                    &s_core.queue,
                    8,
                    NULL) != pdPASS) {
        printf("WARNING: TCP sender task creation failed\r\n");
    }

    print_startup_memory_stats();

    vTaskStartScheduler();

    printf("ERROR: FreeRTOS scheduler returned\r\n");
    for (;;) {
        bflb_mtimer_delay_ms(1000);
    }
}
