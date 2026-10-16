# GPMon
UTL Gamma Plus Data Monitoring using BL602 based WIFI &amp; BLE Controller

## Capture and server configuration

- Main-board UART1 uses `source_id=1`; daughter UART0 uses `source_id=0`. Both feed one FIFO, with UART1 serviced first.
- The oldest RAM records spill to LittleFS flash at 350 queued records. Flash backlog is transmitted before newer RAM records.
- The server destination defaults to the current Wi-Fi gateway on port 9100. Visit `/serverconfig` to choose automatic gateway mode or save a manual IPv4 address and port.
- See [FIFO and protocol notes](docs/FIFO_AND_PROTOCOL.md) for ordering, source IDs, retry semantics, and server endpoints.
