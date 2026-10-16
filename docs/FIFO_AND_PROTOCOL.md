# GPMon FIFO, source IDs, and server destination

## Record ordering and buffering

- The RAM capture queue remains the common FIFO for both UART inputs.
- At 350 queued records (or at the queue capacity when capacity is smaller), the spill task starts moving the oldest RAM head to the LittleFS `media` partition.
- A RAM head is claimed while it is copied to flash or awaiting a TCP ACK. The claim prevents the spill task and sender from consuming the same head concurrently.
- A RAM record is removed only after its flash append succeeds or the server ACKs it.
- If flash has older records, the sender transmits flash first. New captures continue to use RAM when there is space.
- If RAM fills while flash cannot accept a record, capture records are counted as dropped; the firmware does not append a newer record ahead of a RAM head being spilled.

## UART source IDs

- `source_id = 0`: daughter-board UART0, default BL602DK pins RX GPIO7 / TX GPIO16.
- `source_id = 1`: main-board UART1, RX GPIO11 / TX GPIO12. UART1 is serviced first.
- Both framers feed the same sequence-numbered RAM FIFO.
- The source ID is carried in the existing one-byte source/UART field at frame offset 12; the frame size and offsets after that field are unchanged.

## Automatic and manual receiver selection

The default mode is automatic. It uses the current Wi-Fi station gateway address as the server IP and port 9100. The gateway is resolved on each sender selection, so a Wi-Fi AP/gateway change updates the destination.

- `GET /serverget` reports the effective `ip`, `port`, and `mode`.
- `GET /serverset?mode=auto` persists automatic gateway mode.
- `GET /serverset?mode=manual&ip=192.168.1.50&port=9100` persists a manual receiver.
- For compatibility with the existing manual settings form, omitting `mode` while supplying `ip` and `port` also saves manual mode.

The repository contains the CGI handlers but not the HTML/JavaScript asset that renders the Wi-Fi dashboard. Therefore the automatic/manual selection API is implemented here, but a visible mode selector must be added to the dashboard asset supplied by the SDK/build environment if it is not already customizable there.

## Delivery semantics

TCP ACKs remove records from the active queue only after the ACK matches device ID and sequence. If the connection drops after the server received a packet but before the ACK reaches the device, that record may be replayed. The receiver should deduplicate by device ID + sequence for effectively-once processing. Flash capacity is finite; prolonged outages or flash write failures can still exhaust buffering.
