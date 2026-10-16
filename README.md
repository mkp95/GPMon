# GPMon

**UTL Gamma Plus Data Monitoring** using a BL602-based Wi-Fi/BLE controller.

GPMon captures data from two UART inputs, frames incoming byte streams into records, buffers records in RAM and flash, and forwards them to a TCP receiver. The firmware also provides a web dashboard and receiver-configuration page.

## Features

- **Dual-UART capture:** captures from main-board UART1 and daughter-board UART0.
- **Shared ordered FIFO:** both inputs feed one sequence-numbered queue; UART1 is serviced first.
- **Packet framing:** a record is completed after a configured interval of UART silence.
- **RAM-to-flash spill:** at 350 queued records (or queue capacity if smaller), the oldest RAM records spill to LittleFS on the `media` flash partition.
- **Backlog-first delivery:** older flash-spooled records are sent before newer RAM records.
- **TCP ACK/retry handling:** a record is retired after a matching server ACK. If the connection fails after server receipt but before the ACK reaches the device, the record may be replayed.
- **Automatic or manual receiver selection:** automatic mode uses the current Wi-Fi gateway on TCP port 9100; manual mode allows an IPv4 address and port to be configured.
- **Web interface:** live monitoring dashboard and `/serverconfig` receiver settings page.
- **Persistent queue and sequence state:** LittleFS spool and persistent sequence allocation support recovery across reconnects and reboots.
- **Runtime diagnostics:** UART byte/error/overflow counters, RAM queue depth/high-water mark, flash backlog, flash write/corruption counters, and memory statistics are printed to the console.
- **Version identification:** startup console and dashboard identify the firmware as `GPMon v1.0_17102026`.

## UART pin mappings

The table below describes the firmware's default BL602 pin configuration. Confirm the board schematic and voltage levels before wiring.

| Input | UART peripheral | Source ID | RX pin (BL602 GPIO) | TX pin (BL602 GPIO) | Serial format |
|---|---|---:|---:|---:|---|
| Main board | UART1 | `1` | GPIO11 | GPIO12 | 115200 baud, 8 data bits, no parity, 1 stop bit (8-N-1) |
| Daughter board | UART0 | `0` | GPIO7 | GPIO16 | 115200 baud, 8 data bits, no parity, 1 stop bit (8-N-1) |

**Wiring notes**
- Connect the sender's TX to the corresponding GPMon RX, and connect ground between devices.
- RX/TX labels above are from the GPMon/BL602 point of view. TX is initialized by the firmware, but capture is performed on RX.
- Use compatible logic levels; do not connect RS-232 voltage levels directly to BL602 GPIO.
- UART1 is initialized first and is the critical capture input. If UART0 initialization fails, firmware continues with UART1.

## Receiver and web configuration

1. Connect the device to Wi-Fi using the firmware's Wi-Fi provisioning interface.
2. Open the device's web interface and navigate to `/serverconfig`.
3. Choose **Automatic gateway** to send to the current Wi-Fi station gateway on port `9100`, or choose **Manual** and enter the receiver IPv4 address and TCP port.
4. Configure the receiver to accept GPMon TCP records and return the matching ACK.

Useful endpoints:
- `/serverconfig` — receiver mode and manual destination form.
- `GET /serverget` — reports the effective IP, port, and mode.
- `GET /serverset?mode=auto` — persist automatic gateway mode.
- `GET /serverset?mode=manual&ip=192.168.1.50&port=9100` — example manual destination (replace the IP and port with your receiver's settings).

## Buffering, ordering, and delivery notes

- Both UART inputs feed a common FIFO and share the sequence-numbered record stream.
- The flash backlog is sent before newer RAM records to preserve ordering.
- Flash capacity is finite. A long receiver outage, full spool, or flash write failure can still result in dropped capture records; watch the runtime counters.
- Delivery is retryable, not strictly exactly-once. If a packet reaches the receiver but its ACK is lost, the packet can be retransmitted.
- The receiver should deduplicate records using **device ID + sequence number**.
- Flash ACK cursor checkpoints are persisted periodically to reduce flash wear. Following a reset before the next checkpoint, a small number of already-ACKed flash records may be replayed; receiver deduplication is therefore important.
- Timestamps are based on the device's monotonic timer. Wall-clock epoch time depends on time synchronization from the receiver; records captured without a valid epoch synchronization may have no usable epoch timestamp.

See [FIFO and protocol notes](docs/FIFO_AND_PROTOCOL.md) for record ordering, source IDs, retry semantics, and server endpoints.

## Firmware release

The initial firmware binary is distributed as `GPMon_v1.0_17102026.bin` from the [GitHub Releases page](https://github.com/mkp95/GPMon/releases). Use the firmware image and flashing procedure appropriate for your exact BL602 board and flash layout; do not assume a raw `.bin` can be used as an OTA update image.

## Build

The repository's GitHub Actions workflow builds the BL602 firmware. See the workflow and project configuration for the required SDK/toolchain setup and build outputs.
