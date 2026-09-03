# fmesh-cli

An irssi-style terminal chat client for [Meshtastic](https://meshtastic.org) devices, written in C++17 with ncurses/PDCurses.

## Features

- **BLE connectivity** via BlueZ D-Bus on Linux (`sdbus-c++`) with auto-pairing (`org.bluez.Agent1`)
- **TCP + serial transport** via `--tcp` and `--serial` CLI options or `/connect`
- **Mesh Stream TCP/TLS networking**: connect multiple `fmesh-cli` instances together securely via TLS 1.2+ encrypted channels
- **Zstandard (zstd) Compression**: adaptive, high-efficiency compression (>60% bandwidth savings) for inter-CLI database synchronization and telemetry, featuring streaming fallback and memory bomb guards
- **irssi-style TUI**: multiple windows, status bar, unread activity indicators, Alt+N switching, colored prompts
- **Channels + DMs**: broadcast channel windows + direct-message query windows with PKI encryption support
- **Interactive nodelist**: `/nodes` opens a scrollable, sortable node list window with single-key DM opening and intelligent window reuse
- **Connection wizard**: `/scan` opens an interactive tabbed wizard for BLE scanning, TCP, serial, or Mesh Sync connections
- **Virtual Nodes**: seamlessly access, monitor, and route messages through remote physical LoRa radios attached to other networked `fmesh-cli` instances as if they were local hardware
- **Remote Radio Control & Device Selection**: select which physical LoRa device (local hardware or via mesh link) to control and transmit from using `/device` or `Ctrl+X`
- **Traceroute**: `/traceroute` (aliases: `/trace`, `/tr`) traces mesh routes and hop counts to remote nodes
- **Remote Config Management**: dynamically view and alter device configurations (`/config [key] [value]`, e.g. `lora.tx_power`, `display.screen_on_secs`) across local and virtual nodes using protobuf reflection
- **Mesh Synchronization**: multi-client star (hub-and-spoke) and multi-hop daisy chain topology synchronization with automatic SQLite replication, message propagation, and packet deduplication
- **Map Tracing & Coordinate Telemetry**: transparently decodes and logs `POSITION_APP` coordinate telemetry into SQLite to build historical trails
- **Multi-device support**: connect to multiple radios simultaneously via `--device` flag or `/connect`
- **Active device cycling**: `Ctrl+X` or `/device` switches the active device for context-sensitive commands
- **Window Management**: `/close` closes the current window and switches back to the previous active window
- **Auto-reconnect**: per-device automatic reconnection on disconnect (up to 6 attempts, 5s intervals)
- **Message history**: past sessions' messages reload from SQLite on startup
- **Raw packet view**: `/raw` displays hex dumps of received FromRadio packets, live raw window
- **Node inspection**: `/whois` shows detailed node info (ID, HW, battery, position, SNR, distance, flags)
- **SQLite persistence**: messages, nodes, channels, coordinates, and ACK state survive restarts
- **Color themes**: configurable color schemes with built-in presets (`/theme [name]`)
- **Fault tolerance**: comprehensive recovery from transient errors, bad file descriptors, write backoff retries, and bounded buffers

## Build

### Prerequisites (Ubuntu 24.04)

```sh
sudo apt install -y build-essential cmake git libncursesw5-dev libsqlite3-dev \
  libprotobuf-dev protobuf-compiler libsdbus-c++-dev libgtest-dev libssl-dev
sudo usermod -aG bluetooth $USER   # then re-login
```

### Prerequisites (Windows 10/11)

- Visual Studio 2022 (with "Desktop development with C++" workload)
- CMake (3.24+)
- Git
- OpenSSL-Win64 (optional, required for Mesh Server/Client TLS functionality)

*Note: Dependencies (SQLite, PDCurses, Protobuf, Zstandard) are automatically fetched and built by CMake.*

### Compile

```sh
git clone --recurse-submodules <repo-url> fmesh-cli
cd fmesh-cli
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

*(On Windows MSVC: `cmake --build build --config Release`)*

### Run tests

```sh
ctest --test-dir build -C Release --output-on-failure
# Or directly run the binary:
./build/fmesh-cli-tests
# (On Windows: .\build\Release\fmesh-cli-tests.exe)
```

## Usage

```sh
# Connect to the default test device (Fad3_0330, PIN 123456)
./build/fmesh-cli

# Pair first, then connect (Linux BLE)
./build/fmesh-cli pair

# Specify a BLE device by name or explicit address
./build/fmesh-cli --name MyNode --pin 000000
./build/fmesh-cli --addr 1C:DB:D4:A7:03:31

# Connect via TCP (e.g. Meshtastic device on WiFi, default port 4403)
./build/fmesh-cli --tcp 192.168.1.50:4403

# Connect via serial port
./build/fmesh-cli --serial /dev/ttyUSB0
./build/fmesh-cli --serial /dev/ttyACM0 --serial-baud 921600

# Connect to another fmesh-cli instance via Mesh Sync stream
./build/fmesh-cli --device mesh:192.168.1.100:28400

# Multi-device: connect to multiple radios simultaneously
./build/fmesh-cli --device ble:NodeA:123456 --device tcp:192.168.1.50 --device serial:/dev/ttyUSB0 --device mesh:10.0.0.5:28400

# Scan for BLE devices and exit
./build/fmesh-cli --scan

# Headless mode (for testing / bots — logs events, no TUI)
./build/fmesh-cli --headless --debug

# Device spec formats:
#   ble:<name>[:<pin>]          addr:<mac>[:<pin>]
#   tcp:<host>[:<port>]         serial:<path>[:<baud>]
#   mesh:<host>:<port>
```

## Key bindings

| Key | Action |
|-----|--------|
| Alt+1..0 | Switch to window 1-10 |
| Alt+q..p | Switch to window 11-20 |
| Alt+a | Next active window |
| Alt+n | Next window |
| Alt+Left / Alt+Right | Previous / next window |
| PgUp / PgDn | Scroll scrollback (navigate pages in nodelist) |
| Ctrl+X | Cycle active device (for multi-device / virtual nodes) |
| Ctrl+L | Redraw screen |
| Ctrl+C or /quit | Exit |

**Nodelist window keys (when `/nodes` is active):**

| Key | Action |
|-----|--------|
| ↑ / ↓ or j / k | Move selection |
| PgUp / PgDn | Page up/down |
| Enter | Show node info + open DM window |
| s | Cycle sort: Name → Last heard → Node ID → Battery → Hops → Distance |

**Connection wizard keys (`/scan`):**

| Key | Action |
|-----|--------|
| ← / → or Tab | Cycle tabs: BLE / TCP / Serial / Mesh Sync |
| ↑ / ↓ or j / k | Navigate device list / input fields |
| Enter | Connect / confirm |
| Tab | Switch field (PIN, host, port, etc.) |
| Esc | Go back / cancel |

## Commands

| Command | Aliases | Description |
|---------|---------|-------------|
| `/help` | `/h` | Show help and command overview |
| `/list` | `/windows` | List open windows with unread activity indicators |
| `/nodes` | `/who` | Open interactive node list window (arrows=select, enter=DM/info, s=sort) |
| `/query <node\|nick>` | `/q` | Open a DM window with a node (prefers active device) |
| `/msg <node\|nick> <text>` | `/m` | Send a DM without switching windows (prefers active device) |
| `/channel <n>` | `/ch` | Switch to or create channel window N (uses active device; Ctrl+X to cycle) |
| `/window <N>` | `/w` | Switch to window N |
| `/close` | `/c` | Close current channel/DM/nodelist window and return to previous window |
| `/clear` | | Clear current window's scrollback |
| `/info` | `/i` | Show connection info across all devices |
| `/me <text>` | | Send an action message (`*nick text*`) |
| `/reconnect [id]` | | Reconnect all devices, or a specific device by ID |
| `/config [section] [key=val]`| `/cfg` | View device configuration or modify settings via protobuf reflection |
| `/traceroute <node>` | `/trace`, `/tr` | Trace mesh route and hop count to target node |
| `/whois <node\|nick>` | `/wi` | Show detailed node information (prefers active device) |
| `/raw [N]` | | Show last N raw FromRadio packets (default 5) |
| `/stats` | `/st` | Show packet type statistics across all devices |
| `/topic` | `/t` | Show current channel/DM details |
| `/lastlog <pattern>` | `/l` | Search scrollback for pattern or text |
| `/connect <spec>` | | Connect a new device at runtime (`ble:`, `tcp:`, `serial:`, `mesh:`) |
| `/disconnect [id]` | `/dc` | Disconnect a device (no arg: list devices) |
| `/device [id]` | `/dev` | Show connected/virtual devices or switch active device |
| `/scan` | `/s` | Open the interactive connection wizard (BLE / TCP / Serial / Mesh Sync) |
| `/server [on\|off]` | | Open the Mesh Sync Stream Server config dialog or toggle server |
| `/theme [name]` | | Switch or list color themes (`dark`, `classic`) |
| `/quit` | `/exit` | Exit fmesh-cli |

Plain text (without leading `/`) sends to the current window's target: channel broadcast or DM.

### Input prompt

The input line shows a context-sensitive prompt so it's always clear where your message is routed:

| Prompt | Meaning |
|--------|---------|
| `cmd> ` | You typed `/` — input is a command, not a message |
| `#name> ` | Input goes to channel `name` as a broadcast |
| `nick> ` | Input goes to `nick` as a direct message |
| `status> ` | Status window is active (text cannot be broadcast from here) |

## Mesh Synchronization & Virtual Nodes

`fmesh-cli` can link multiple instances together over TCP/TLS to create an integrated synchronization network:

- **Stream Server (`/server`)**: Listens on a TLS port (default 28400) using ephemeral self-signed X.509 certificates. Credentials are automatically generated and stored in `config.txt`.
- **Stream Client (`/connect mesh:<host>:<port>`)**: Authenticates against a remote `fmesh-cli` server over TLS.
- **Virtual Nodes**: Physical LoRa radios attached to a server or client are automatically advertised to connected peers (`devices` payload). Peer instances expose them as `virtual:<stream>:<radio_id>` (e.g. `virtual:mesh:127.0.0.1:28400:serial:/dev/ttyUSB0`), allowing users to select them via `/device` and transmit over LoRa through remote hardware.
- **Zstandard Compression**: JSON synchronization payloads (database sync, node lists, telemetry) are adaptively compressed using Zstandard (`0xD1` framing marker), providing >60% reduction in bandwidth consumption with fallback to uncompressed JSON (`0xD0`) and memory-bomb protection.
- **Topologies Supported**: Tested and verified across both multi-client star networks (hub-and-spoke) and multi-hop daisy chains.

## Architecture & Wire Protocol

```
src/
  app/          CLI args, configuration, startup wiring
  ble/          BlueZ D-Bus: BLE scan/connect/GATT + pairing agent
  mesh/         Protocol codec (protobuf), node DB, mesh service, events, sync
  store/        SQLite persistence (messages, nodes, channels, positions)
  stream/       TCP, serial, and TLS StreamServer / StreamClient transports
  tui/          ncurses/PDCurses TUI: windows, input, status bar, commands, wizard
  util/         Compression (zstd), logging, eventfd, concurrent queues
  tests/        Minitest test framework (unit & integration tests)
```

### Inter-CLI Stream Framing

Stream messages between CLI instances use a 4-byte header:
`0x94 <marker> <len_hi> <len_lo>` followed by the payload (up to 65,000 bytes).

| Marker | Payload Type | Description |
|--------|--------------|-------------|
| `0xC3` | Meshtastic Protobuf | Raw ToRadio / FromRadio packet |
| `0xD0` | DbSync JSON | Uncompressed database synchronization payload |
| `0xD1` | DbSync Zstd | Zstandard-compressed database synchronization payload |

### BLE GATT Characteristics

When connecting directly to Meshtastic hardware via BLE:

| UUID | Purpose |
|------|---------|
| `6ba1b218-...` | Service |
| `f75c76d2-...` | TORADIO (write) |
| `2c55e69e-...` | FROMRADIO (read/poll) |
| `ed9da18c-...` | FROMNUM (notify) |
| `5a3d6e49-...` | LOGRADIO (notify, optional) |

## Data storage

- Database: `~/.local/share/fmesh-cli/mesh.db`
- Configuration: `~/.local/share/fmesh-cli/config.txt`
- History: `~/.local/share/fmesh-cli/history`
- Log: `~/.local/share/fmesh-cli/fmesh-cli.log`

*(On Windows, files reside in `%USERPROFILE%\.local\share\fmesh-cli\`)*

## Testing

```sh
# Run the complete test suite (204 unit & integration tests)
./build/fmesh-cli-tests
# (On Windows: .\build\Release\fmesh-cli-tests.exe)

# Run live integration test (requires a paired device in range)
./build/fmesh-cli-live
```

The test suite validates protocol encoding, packet deduplication, SQLite persistence, TUI command dispatch, Zstandard round-trip & bomb protection, 5-client star mesh sharing, and multi-hop chain propagation.

## Fault tolerance

- **Write retries**: stream `send_to_radio()` retries on partial writes and transient errors (EAGAIN, EINTR) with backoff, up to 10 attempts.
- **Buffer protection**: stream read buffer is capped at 64 KB to prevent unbounded growth on corrupt data.
- **Decompression bomb guard**: Zstd decompression caps payload size at 10 MB and limits allocation sizes based on frame headers.
- **Thread safety**: BLE/stream proxy access between the background worker threads and the UI thread is guarded by mutexes.
- **Null-safe DB**: all SQLite column reads check for NULL values before conversion; operations on a closed database no-op safely.
- **Input bounds**: input line buffer is capped at 8 KB; history at 10,000 entries.
- **Scroll clamp**: window scroll offset is clamped both at zero and at the line count (no overscroll).
- **DB checkpoint**: WAL checkpoint runs periodically (every 100 writes) and on close to keep the WAL file bounded.
