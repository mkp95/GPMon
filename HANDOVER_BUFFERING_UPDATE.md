# GPMon_v1 buffering update

This source update targets BL602 and makes the following focused changes:

- Limits framed packet payloads to 80 bytes (`GPMON_MAX_PACKET_BYTES`). Longer silence-delimited frames are truncated to 80 bytes and counted by the existing framer statistics.
- Caps the capture queue request at 50 KiB **including record metadata**. Capacity is calculated using `sizeof(gpmon_record_t)`; if allocation fails, the queue retries at 75% of the previous capacity until it reaches the configured minimum depth.
- The selected FreeRTOS `heap_3` allocator delegates to libc `malloc/free` and does not implement `xPortGetFreeHeapSize()`. Therefore, this build cannot query free heap to calculate a 16 KiB reserve or 60% free-heap budget. Allocation is best-effort and falls back to smaller requests; this does not guarantee a fixed heap reserve.
- Reduces the separate TCP staging queue to 8 records so it does not consume most of the FreeRTOS heap before the capture queue is created.
- Disables per-packet HEX/ASCII console dumps by default (`GPMON_VERBOSE_PACKET_LOG=0`) because synchronous console output can delay draining the UART RX ring. Set it to `1` for debugging if needed.
- Expands periodic UART console statistics with capture queue capacity, high-water mark, allocation bytes, capture drops and TCP staging drops.

The configured silence gap remains 100 ms, and UART pins/baud/framing are unchanged. The queue uses drop-newest behavior when full; it does not overwrite unread records. Sequence numbers are assigned before enqueue, so a packet dropped at enqueue still consumes its sequence number.

## Build

From the project directory with the SDK/toolchain layout expected by this project:

```sh
export BL_SDK_BASE=/home/malay/bouffalo_sdk
export PATH=/home/malay/thead_toolchain_min/bin:$PATH
make CHIP=bl602 BOARD=bl602dk
```

GitHub Actions also runs this build for pull requests and pushes to `main`; it installs the SDK and toolchain on an Ubuntu runner.

## Important verification notes

- Record size is compiler/ABI dependent. Confirm the actual size in the target build; the startup/statistics log reports `capacity * sizeof(gpmon_record_t)` for the allocated capture queue.
- The 50 KiB value is a ceiling/target, not a promise: the capture queue may allocate less if the allocator cannot satisfy the full request. The additional 8-record TCP queue and its FreeRTOS overhead are separate allocations.
- Because `heap_3` provides no free-heap query, the code cannot guarantee a specific remaining heap reserve. Review runtime memory behavior on the BL602 target.
- Hardware UART throughput, Wi-Fi stability, and TCP delivery still require device testing. CI builds firmware only; it does not flash hardware.
