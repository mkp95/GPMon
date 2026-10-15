#ifndef GPMON_TCP_SENDER_H
#define GPMON_TCP_SENDER_H

void gpmon_tcp_sender_task(void *arg);
int gpmon_tcp_sequence_init(void);
int gpmon_tcp_sequence_next(uint32_t *out);
int gpmon_tcp_sequence_ready(void);

#endif
