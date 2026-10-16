#include "uart_capture.h"

#include "board.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "bflb_uart.h"
#include "gpmon_config.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define RX_RING_SIZE 512u
#define RX_RING_MASK (RX_RING_SIZE - 1u)
#define UART_PORT_COUNT 2u
#define UART0_RX_PIN 7u
#define UART0_TX_PIN 16u

#if (RX_RING_SIZE & RX_RING_MASK) != 0
#error "RX_RING_SIZE must be a power of two"
#endif

typedef struct {
    uint32_t time_us_low;
    uint8_t byte;
} rx_item_t;

typedef struct {
    struct bflb_device_s *uart;
    rx_item_t ring[RX_RING_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t rx_bytes;
    volatile uint32_t ring_overflows;
    volatile uint32_t uart_errors;
    uint32_t seen_uart_errors;
    uint32_t seen_ring_overflows;
    uint8_t source_id;
    uint8_t rx_pin;
    uint8_t tx_pin;
    uint8_t rx_func;
    uint8_t tx_func;
    gpmon_framer_t *framer;
} uart_port_t;

static gpmon_core_t *s_core;
static gpmon_framer_t s_uart0_framer;
static uart_port_t s_ports[UART_PORT_COUNT] = {
    { .source_id = 1u, .rx_pin = GPMON_UART_RX_PIN,
      .tx_pin = GPMON_UART_TX_PIN,
      .rx_func = GPIO_UART_FUNC_UART1_RX,
      .tx_func = GPIO_UART_FUNC_UART1_TX },
    { .source_id = 0u, .rx_pin = UART0_RX_PIN,
      .tx_pin = UART0_TX_PIN,
      .rx_func = GPIO_UART_FUNC_UART0_RX,
      .tx_func = GPIO_UART_FUNC_UART0_TX }
};

static void rx_ring_push_isr(uart_port_t *port, uint8_t byte,
                             uint32_t time_us_low)
{
    uint32_t head = port->head;
    uint32_t next = (head + 1u) & RX_RING_MASK;
    if (next == port->tail) {
        port->ring_overflows++;
        return;
    }
    port->ring[head].byte = byte;
    port->ring[head].time_us_low = time_us_low;
    __sync_synchronize();
    port->head = next;
    port->rx_bytes++;
}

static void uart_isr(int irq, void *arg)
{
    uintptr_t port_index = (uintptr_t)arg;
    uart_port_t *port;
    uint32_t status;
    (void)irq;
    if (port_index >= UART_PORT_COUNT) return;
    port = &s_ports[port_index];
    if (port->uart == NULL) return;

    status = bflb_uart_get_intstatus(port->uart);
    if (status & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) {
        while (bflb_uart_rxavailable(port->uart)) {
            uint8_t byte = (uint8_t)bflb_uart_getchar(port->uart);
            uint32_t timestamp = (uint32_t)bflb_mtimer_get_time_us();
            rx_ring_push_isr(port, byte, timestamp);
        }
    }
    if (status & UART_INTSTS_RTO)
        bflb_uart_int_clear(port->uart, UART_INTCLR_RTO);
    if (status & (UART_INTSTS_PCE | UART_INTSTS_TX_FER | UART_INTSTS_RX_FER)) {
        port->uart_errors++;
        if (status & UART_INTSTS_PCE)
            bflb_uart_int_clear(port->uart, UART_INTCLR_PCE);
    }
}

static int init_port(struct bflb_device_s *gpio, uart_port_t *port,
                     uint32_t index)
{
    struct bflb_uart_config_s cfg;
    char name[8];
    (void)name;
    port->uart = bflb_device_get_by_name(index == 0u ? "uart1" : "uart0");
    if (port->uart == NULL) return -1;

    bflb_gpio_uart_init(gpio, port->rx_pin, port->rx_func);
    bflb_gpio_uart_init(gpio, port->tx_pin, port->tx_func);
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
    bflb_uart_init(port->uart, &cfg);

    bflb_uart_rxint_mask(port->uart, true);
    bflb_irq_attach(port->uart->irq_num, uart_isr, (void *)(uintptr_t)index);
    bflb_irq_enable(port->uart->irq_num);
    bflb_uart_rxint_mask(port->uart, false);
    return 0;
}

bool gpmon_uart_capture_init(gpmon_core_t *core)
{
    struct bflb_device_s *gpio;
    if (core == NULL || core->queue.slots == NULL) return false;
    s_core = core;
    memset(&s_uart0_framer, 0, sizeof(s_uart0_framer));
    gpmon_framer_init_source(&s_core->framer, 1u);
    gpmon_framer_init_source(&s_uart0_framer, 0u);
    for (uint32_t i = 0; i < UART_PORT_COUNT; i++) {
        s_ports[i].head = s_ports[i].tail = 0;
        s_ports[i].rx_bytes = s_ports[i].ring_overflows = 0;
        s_ports[i].uart_errors = 0;
        s_ports[i].seen_uart_errors = s_ports[i].seen_ring_overflows = 0;
        s_ports[i].framer = i == 0u ? &s_core->framer : &s_uart0_framer;
    }
    gpio = bflb_device_get_by_name("gpio");
    if (gpio == NULL) return false;
    /* UART1 is initialized first and always serviced first by the poller. */
    if (init_port(gpio, &s_ports[0], 0u) != 0) return false;
    if (init_port(gpio, &s_ports[1], 1u) != 0) return false;
    return true;
}

static uint64_t expand_timestamp(uint32_t low, uint64_t now_us)
{
    uint64_t candidate = (now_us & 0xFFFFFFFF00000000ull) | low;
    if (candidate > now_us && candidate - now_us > 0x80000000ull)
        candidate -= 0x100000000ull;
    else if (now_us > candidate && now_us - candidate > 0x80000000ull)
        candidate += 0x100000000ull;
    return candidate;
}

static void poll_port(uart_port_t *port)
{
    for (;;) {
        uint32_t tail = port->tail;
        rx_item_t item;
        if (tail == port->head) break;
        __sync_synchronize();
        item = port->ring[tail];
        port->tail = (tail + 1u) & RX_RING_MASK;
        gpmon_framer_feed(port->framer, &s_core->queue, &item.byte, 1u,
                          expand_timestamp(item.time_us_low,
                                           bflb_mtimer_get_time_us()));
    }
    if (port->seen_ring_overflows != port->ring_overflows) {
        port->seen_ring_overflows = port->ring_overflows;
        gpmon_framer_note_uart_error(port->framer);
    }
    if (port->seen_uart_errors != port->uart_errors) {
        uint32_t delta = port->uart_errors - port->seen_uart_errors;
        while (delta--) gpmon_framer_note_uart_error(port->framer);
        port->seen_uart_errors = port->uart_errors;
    }
    gpmon_framer_tick(port->framer, &s_core->queue,
                      bflb_mtimer_get_time_us());
}

void gpmon_uart_capture_poll(void)
{
    if (s_core == NULL) return;
    /* Main-board UART1 is serviced first; both framers feed one sequence FIFO. */
    poll_port(&s_ports[0]);
    poll_port(&s_ports[1]);
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
    out->rx_bytes = s_ports[0].rx_bytes + s_ports[1].rx_bytes;
    out->rx_ring_overflows = s_ports[0].ring_overflows + s_ports[1].ring_overflows;
    out->uart_error_events = s_ports[0].uart_errors + s_ports[1].uart_errors;
}
