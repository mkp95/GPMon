# GPMon_v1 buffering update

This source update targets BL602 and makes the following focused changes:

- Limits framed packet payloads to 80 bytes (`GPMON_MAX_PACKET_BYTES`). Longer silence-delimited frames are truncated to 80 bytes and counted by the existing framer statistics.
- Caps the capture queue allocation at 50 KiB **including record metadata**. Its actual capacity is calculated from `sizeof(gpmon_record_t)` and the heap available when the capture queue initializes, while preserving the existing 16 KiB reserve and 60% budget rule.
- Reduces the separate TCP staging queue to 8 records so it does not consume most of the FreeRTOS heap before the capture queue is created.
- Disables per-packet HEX/ASCII console dumps by default (`GPMON_VERBOSE_PACKET_LOG=0`) because synchronous console output can delay draining the UART RX ring. Set it to `1` for debugging if needed.
- Expands periodic UART console statistics with capture queue capacity, high-water mark, allocation bytes, capture drops and TCP staging drops.

The configured silence gap remains 100 ms, and UART pins/baud/framing are unchanged from this ZIP. The queue uses drop-newest behavior when full; it does not overwrite unread records. Sequence numbers are assigned before enqueue, so a packet dropped at enqueue still consumes its sequence number.

## Build

From the extracted project directory, with the same SDK/toolchain layout expected by this project:

```sh
export BL_SDK_BASE=/home/malay/bouffalo_sdk
export PATH=/home/malay/thead_toolchain_min/bin:$PATH
make CHIP=bl602 BOARD=bl602dk
```

Alternatively, use the SDK's normal CMake build workflow for the installed SDK version. If the paths differ, set `BL_SDK_BASE` and `PATH` to the actual installed SDK/toolchain locations.

## Important verification notes

- The project ZIP does not include the Bouffalo SDK or RISC-V toolchain, so this update has not been verified by a BL602 firmware build in the current environment.
- Record size is compiler/ABI dependent. Confirm the actual size in the target build; the startup/statistics log reports `capacity * sizeof(gpmon_record_t)` for the allocated capture queue.
- The 50 KiB value is a ceiling/target, not a promise: the capture queue may allocate less based on available heap and reserves. The additional 8-record TCP queue and its FreeRTOS overhead are separate allocations.
- Hardware UART throughput, Wi-Fi stability, and TCP delivery still require device testing. No firmware was flashed.
