# fmesh-cli

An irssi-style terminal chat client for [Meshtastic](https://meshtastic.org) devices, written in C++17 with ncurses.

## Features

- **BLE connectivity** via BlueZ D-Bus (sdbus-c++)
- **TCP + serial** transport via `--tcp` and `--serial` CLI options
- **irssi-style TUI**: multiple windows, status bar, activity marks, Alt+N switching, colored prompts
- **Channels + DMs**: broadcast channel windows + direct-message query windows
- **Interactive nodelist**: `/nodes` opens a scrollable, sortable node list window with selection
- **Connection wizard**: `/scan` opens an interactive wizard for BLE scanning, TCP, or serial connection
- **Multi-device support**: connect to multiple radios simultaneously via `--device` flag or `/connect`
- **Active device cycling**: `Ctrl+X` or `/device` switches the active device for context-sensitive commands
- **Auto-reconnect**: per-device automatic reconnection on disconnect (up to 6 attempts, 5s intervals)
- **Mesh Connectivity (Server/Client)**: multi-device secure mesh sharing with TLS authentication
- **Message history**: past sessions' messages reload from SQLite on startup
- **Device config viewer**: `/config` shows LoRa, power, position, network, Bluetooth settings
- **Raw packet view**: `/raw` displays hex dumps of received FromRadio packets, live raw window
- **Node inspection**: `/whois` shows detailed node info (ID, HW, battery, position, SNR, flags)
- **Auto-pairing**: built-in `org.bluez.Agent1` that supplies the PIN automatically
- **SQLite persistence**: messages, nodes, channels, and ACK state survive restarts
- **PKI DM support**: public-key encryption for direct messages on recent firmware
- **Terminal resize**: responsive layout with minimum-size guards and SIGWINCH handling
- **Outgoing echo**: sent messages appear immediately in the window (no delay for mesh echo)
- **Log rotation**: log file automatically rotates when it exceeds 5 MB
- **WAL checkpoint**: periodic SQLite WAL checkpoint prevents unbounded file growth
- **Fault tolerance**: write retries, buffer protection, thread safety, input bounds, scroll clamping
- **Color themes**: configurable color schemes with several built-in presets

## Build

### Prerequisites (Ubuntu 24.04)

```sh
sudo apt install -y build-essential cmake git libncursesw5-dev libsqlite3-dev \
  libprotobuf-dev protobuf-compiler libsdbus-c++-dev libgtest-dev libssl-dev
sudo usermod -aG bluetooth $USER   # then re-login
```

### Prerequisites (Windows 10/11)

- Visual Studio 2022 (with "Desktop development with C++" workload)
- CMake
- Git
- OpenSSL-Win64 (optional, required for Mesh Server/Client TLS functionality)

*Note: Windows dependencies (SQLite, PDCurses, Protobuf) are automatically fetched by CMake.*

### Compile

```sh
git clone --recurse-submodules <repo-url> fmesh-cli
cd fmesh-cli
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run tests

```sh
./build/fmesh-cli-tests
# (On Windows, run .\build\Release\fmesh-cli-tests.exe)
```

## Usage

```sh
# Connect to the default test device (Fad3_0330, PIN 123456)
./build/fmesh-cli
# (On Windows, use .\build\Release\fmesh-cli.exe instead)

# Pair first, then connect
./build/fmesh-cli pair

# Specify a different device
./build/fmesh-cli --name MyNode --pin 000000

# Skip scanning by providing the MAC directly
./build/fmesh-cli --addr 1C:DB:D4:A7:03:31

# Scan for BLE devices and exit
./build/fmesh-cli --scan

# Headless mode (for testing — logs events, no TUI)
./build/fmesh-cli --headless --debug

# Verbose logging
./build/fmesh-cli --debug

# Connect via TCP (e.g. Meshtastic device on WiFi, port 4403)
./build/fmesh-cli --tcp 192.168.1.50:4403

# Connect via serial port
./build/fmesh-cli --serial /dev/ttyUSB0
./build/fmesh-cli --serial /dev/ttyACM0 --serial-baud 921600

# Multi-device: connect to several devices at once
./build/fmesh-cli --device ble:NodeA:123456 --device tcp:192.168.1.50 --device serial:/dev/ttyUSB0

# Device spec formats:
#   ble:<name>[:<pin>]      addr:<mac>[:<pin>]
#   tcp:<host>[:<port>]     serial:<path>[:<baud>]
```

## Key bindings

| Key | Action |
|-----|--------|
| Alt+1..0 | Switch to window N |
| Alt+a | Next active window |
| Alt+n / Alt+p | Next / previous window |
| PgUp / PgDn | Scroll scrollback (navigate pages in nodelist) |
| Ctrl+X | Cycle active device (for multi-device) |
| Ctrl+L | Redraw screen |
| Ctrl+C or /quit | Exit |

**Nodelist window keys (when `/nodes` is active):**

| Key | Action |
|-----|--------|
| ↑ / ↓ or j / k | Move selection |
| PgUp / PgDn | Page up/down |
| Enter | Show node info + open DM window |
| s | Cycle sort: Name → Last heard → Node ID → Battery → Hops |

**Connection wizard keys (`/scan`):**

| Key | Action |
|-----|--------|
| ← / → or Tab | Cycle BLE / TCP / Serial |
| ↑ / ↓ or j / k | Navigate BLE device list |
| Enter | Connect / confirm |
| Tab | Switch field (PIN, host, port, etc.) |
| Esc | Go back / cancel |

## Commands

| Command | Description |
|---------|-------------|
| `/help` | Show help |
| `/list` | List windows |
| `/nodes` | Open interactive node list window (arrows=select, enter=info, s=sort) |
| `/query <node\|nick>` | Open a DM window (prefers active device) |
| `/msg <node\|nick> <text>` | Send a DM without switching (prefers active device) |
| `/channel <n>` | Switch to channel N (uses active device; Ctrl+X to cycle) |
| `/window <N>` | Switch to window N |
| `/close` | Switch away from current window |
| `/clear` | Clear scrollback |
| `/info` | Show connection info (all devices) |
| `/me <text>` | Send an action |
| `/reconnect [id]` | Reconnect all devices, or a specific device by ID |
| `/config [section]` | Show device configuration (all devices, optional filter) |
| `/whois <node\|nick>` | Show detailed node information (prefers active device) |
| `/raw [N]` | Show last N raw packets (all devices, default 5) |
| `/stats` | Show packet type counts (all devices) |
| `/topic` | Show channel/DM details |
| `/lastlog <pattern>` | Search scrollback |
| `/connect <spec>` | Connect a new device at runtime |
| `/disconnect [id]` | Disconnect a device (no arg: list devices) |
| `/device [id]` | Show or switch active device |
| `/scan` | Open the interactive connection wizard (BLE scan / TCP / serial) |
| `/quit` | Exit |

Plain text (without leading `/`) sends to the current window's target: channel broadcast or DM.

### Input prompt

The input line shows a context-sensitive prompt so it's always clear what your
text will do:

| Prompt | Meaning |
|--------|---------|
| `cmd> ` | You typed `/` — input is a command, not a message |
| `#name> ` | Input goes to the `name` channel as a broadcast |
| `nick> ` | Input goes to `nick` as a direct message |
| `status> ` | Status window is active (text can't be sent from here) |

## Architecture

```
src/
  app/          CLI args, config, top-level wiring
  ble/          BlueZ D-Bus: BLE scan/connect/GATT + pairing agent
  mesh/         Protocol codec (protobuf), node DB, mesh service, events
  store/        SQLite persistence
  stream/       TCP and serial transport
  tui/          ncurses TUI: windows, input, status bar, commands, wizard
  util/         Logging, thread-safe queue, eventfd
  tests/        Custom minitest unit tests (header-only framework)
```

The BLE layer talks the Meshtastic wire protocol directly (protobuf over GATT), with no Python dependency. GATT characteristics used:

| UUID | Purpose |
|------|---------|
| `6ba1b218-...` | Service |
| `f75c76d2-...` | TORADIO (write) |
| `2c55e69e-...` | FROMRADIO (read/poll) |
| `ed9da18c-...` | FROMNUM (notify) |
| `5a3d6e49-...` | LOGRADIO (notify, optional) |

## Data storage

- Database: `~/.local/share/fmesh-cli/mesh.db`
- Log: `~/.local/share/fmesh-cli/fmesh-cli.log`

## Testing

```sh
# Unit tests (no device required)
./build/fmesh-cli-tests

# Live integration test (requires a paired device in range)
./build/fmesh-cli-live
```

The live test connects to the configured device, sends a channel broadcast
and a DM, and verifies the outgoing messages are persisted to SQLite.

## Known issues

- BLE connections can be unreliable if the device isn't paired. Use `fmesh-cli pair` to establish a bond first.
- Meshtastic firmware holds a single bond; if the device is paired to a phone, pairing from fmesh-cli will fail with `AuthenticationFailed`. Remove the bond on the phone first, then run `fmesh-cli pair`.
- The `le-connection-abort-by-local` error on first connect is common; the client retries automatically.
- If GATT operations time out, the device may have dropped the connection. Use `/reconnect` or restart.

## Fault tolerance

- **Write retries**: stream `send_to_radio()` retries on partial writes and transient errors (EAGAIN, EINTR) with backoff, up to 10 attempts.
- **Buffer protection**: stream read buffer is capped at 64 KB to prevent unbounded growth on corrupt data.
- **Thread safety**: BLE proxy/connection access between the event-loop thread and the UI thread is protected by a mutex.
- **Null-safe DB**: all SQLite column reads check for NULL values before conversion; operations on a closed database no-op safely.
- **Input bounds**: input line buffer is capped at 8 KB; history at 10,000 entries.
- **Scroll clamp**: window scroll offset is clamped both at zero and at the line count (no overscroll).
- **Ncurses guard**: ncurses initialization failure is detected and reported before entering the TUI loop.
- **DB checkpoint**: WAL checkpoint runs periodically (every 100 writes) and on DB close to keep the WAL file bounded.
