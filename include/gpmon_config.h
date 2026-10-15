#ifndef GPMON_CONFIG_H
#define GPMON_CONFIG_H

/* GPMon Milestone 1: portable capture core. These are safety limits, not
 * assumptions about the incoming protocol's packet length. */
#define GPMON_UART_BAUD             115200u
#define GPMON_UART_RX_PIN            11u
#define GPMON_UART_TX_PIN            12u
#define GPMON_FRAME_GAP_US 100000ull
#define GPMON_MAX_PACKET_BYTES        80u
#define GPMON_QUEUE_DEPTH             32u
/* Maximum capture-queue allocation, including record metadata. */
#define GPMON_CAPTURE_QUEUE_TARGET_BYTES (50u * 1024u)
/* Keep per-packet console dumps disabled in the latency-sensitive capture task. */
#define GPMON_VERBOSE_PACKET_LOG       0u
/* A small TCP staging queue leaves heap for capture, Wi-Fi and lwIP. */
#define GPMON_TCP_QUEUE_RECORDS        8u

#endif
