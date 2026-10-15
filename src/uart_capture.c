#include "uart_capture.h"

#include "board.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "bflb_uart.h"
#include "gpmon_config.h"

#include <stddef.h>
#include <string.h>

#define RX_RING_SIZE 512u
#define RX_RING_MASK (RX_RING_SIZE - 1u)

#if (RX_RING_SIZE & RX_RING_MASK) != 0
#error "RX_RING_SIZE must be a power of two"
#endif

typedef struct {
    uint32_t time_us_low;
    uint8_t byte;
} rx_item_t;

static struct bflb_device_s *s_uart;
static gpmon_core_t *s_core;
static rx_item_t s_rx_ring[RX_RING_SIZE];
static volatile uint32_t s_rx_head;
static volatile uint32_t s_rx_tail;
static volatile uint32_t s_rx_bytes;
static volatile uint32_t s_ring_overflows;
static volatile uint32_t s_uart_errors;
static uint32_t s_seen_uart_errors;

static void rx_ring_push_isr(uint8_t byte, uint32_t time_us_low)
{
    uint32_t head = s_rx_head;
    uint32_t next = (head + 1u) & RX_RING_MASK;

    if (next == s_rx_tail) {
        s_ring_overflows++;
        return;
    }

    s_rx_ring[head].byte = byte;
    s_rx_ring[head].time_us_low = time_us_low;
    /* Publish head only after the item contents are written. */
    s_rx_head = next;
    s_rx_bytes++;
}

static void uart1_isr(int irq, void *arg)
{
    uint32_t status;
    (void)irq;
    (void)arg;

    status = bflb_uart_get_intstatus(s_uart);

    if (status & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) {
        while (bflb_uart_rxavailable(s_uart)) {
            uint8_t byte = (uint8_t)bflb_uart_getchar(s_uart);
            uint32_t timestamp = (uint32_t)bflb_mtimer_get_time_us();
            rx_ring_push_isr(byte, timestamp);
        }
    }

    if (status & UART_INTSTS_RTO) {
        bflb_uart_int_clear(s_uart, UART_INTCLR_RTO);
    }

    if (status & (UART_INTSTS_PCE | UART_INTSTS_TX_FER | UART_INTSTS_RX_FER)) {
        s_uart_errors++;
        if (status & UART_INTSTS_PCE) {
            bflb_uart_int_clear(s_uart, UART_INTCLR_PCE);
        }
    }
}

bool gpmon_uart_capture_init(gpmon_core_t *core)
{
    struct bflb_device_s *gpio;
    struct bflb_uart_config_s cfg;

    if (core == NULL) return false;

    s_core = core;
    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_bytes = 0;
    s_ring_overflows = 0;
    s_uart_errors = 0;
    s_seen_uart_errors = 0;

    gpio = bflb_device_get_by_name("gpio");
    s_uart = bflb_device_get_by_name("uart1");
    if (gpio == NULL || s_uart == NULL) return false;

    /* Use the requested UART1 pin mapping, independent of board defaults. */
    bflb_gpio_uart_init(gpio, GPMON_UART_RX_PIN, GPIO_UART_FUNC_UART1_RX);
    bflb_gpio_uart_init(gpio, GPMON_UART_TX_PIN, GPIO_UART_FUNC_UART1_TX);

    memset(&cfg, 0, sizeof(cfg));
    cfg.baudrate = GPMON_UART_BAUD;
    cfg.direction = UART_DIRECTION_TXRX;
    cfg.data_bits = UART_DATA_BITS_8;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.parity = UART_PARITY_NONE;
    cfg.flow_ctrl = UART_FLOWCTRL_NONE;
    cfg.tx_fifo_threshold = 7;
    cfg.rx_fifo_threshold = 1;
    cfg.bit_order = UART_LSB_FIRST;
    bflb_uart_init(s_uart, &cfg);

    /* Install the handler before enabling RX interrupts. */
    bflb_uart_rxint_mask(s_uart, true);
    bflb_irq_attach(s_uart->irq_num, uart1_isr, NULL);
    bflb_irq_enable(s_uart->irq_num);
    bflb_uart_rxint_mask(s_uart, false);
    return true;
}

static uint64_t expand_timestamp(uint32_t low, uint64_t now_us)
{
    uint64_t candidate = (now_us & 0xFFFFFFFF00000000ull) | low;
    if (candidate > now_us && candidate - now_us > 0x80000000ull) {
        candidate -= 0x100000000ull;
    } else if (now_us > candidate && now_us - candidate > 0x80000000ull) {
        candidate += 0x100000000ull;
    }
    return candidate;
}

void gpmon_uart_capture_poll(void)
{
    if (s_core == NULL) return;

    for (;;) {
        uint32_t tail = s_rx_tail;
        rx_item_t item;
        uint64_t now_us;

        if (tail == s_rx_head) break;
        item = s_rx_ring[tail];
        s_rx_tail = (tail + 1u) & RX_RING_MASK;

        now_us = bflb_mtimer_get_time_us();
        gpmon_core_rx_burst(s_core, &item.byte, 1u,
                            expand_timestamp(item.time_us_low, now_us));
    }

    if (s_seen_uart_errors != s_uart_errors) {
        uint32_t delta = s_uart_errors - s_seen_uart_errors;
        while (delta--) gpmon_framer_note_uart_error(&s_core->framer);
        s_seen_uart_errors = s_uart_errors;
    }

    gpmon_core_tick(s_core, bflb_mtimer_get_time_us());
}

const gpmon_record_t *gpmon_uart_capture_peek(void)
{
    return s_core == NULL ? NULL : gpmon_queue_peek(&s_core->queue);
}

void gpmon_uart_capture_release(void)
{
    if (s_core != NULL) gpmon_queue_release(&s_core->queue);
}

void gpmon_uart_capture_get_stats(gpmon_uart_capture_stats_t *out)
{
    if (out == NULL) return;
    out->rx_bytes = s_rx_bytes;
    out->rx_ring_overflows = s_ring_overflows;
    out->uart_error_events = s_uart_errors;
}
