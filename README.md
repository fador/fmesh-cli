# mesh-cli

An irssi-style terminal chat client for [Meshtastic](https://meshtastic.org) devices, written in C++17 with ncurses.

## Features

- **BLE connectivity** via BlueZ D-Bus (sdbus-c++)
- **irssi-style TUI**: multiple windows, status bar, activity marks, Alt+N switching, colored prompts
- **Channels + DMs**: broadcast channel windows + direct-message query windows
- **Message history**: past sessions' messages reload from SQLite on startup
- **Device config viewer**: `/config` shows LoRa, power, position, network, Bluetooth settings
- **Raw packet view**: `/raw` displays hex dumps of received FromRadio packets
- **Node inspection**: `/whois` shows detailed node info (ID, HW, battery, position, SNR, flags)
- **Auto-pairing**: built-in `org.bluez.Agent1` that supplies the PIN automatically
- **SQLite persistence**: messages, nodes, channels, and ACK state survive restarts
- **Multi-device ready**: `MeshService` supports multiple concurrent BLE connections
- **PKI DM support**: public-key encryption for direct messages on recent firmware

## Build

### Prerequisites (Ubuntu 24.04)

```sh
sudo apt install -y build-essential cmake git libncursesw5-dev libsqlite3-dev \
  libprotobuf-dev protobuf-compiler libsdbus-c++-dev libgtest-dev
sudo usermod -aG bluetooth $USER   # then re-login
```

### Compile

```sh
git clone --recurse-submodules <repo-url> mesh-cli
cd mesh-cli
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run tests

```sh
./build/mesh-cli-tests
```

## Usage

```sh
# Connect to the default test device (Fad3_0330, PIN 123456)
./build/mesh-cli

# Pair first, then connect
./build/mesh-cli pair

# Specify a different device
./build/mesh-cli --name MyNode --pin 000000

# Skip scanning by providing the MAC directly
./build/mesh-cli --addr 1C:DB:D4:A7:03:31

# Scan for BLE devices and exit
./build/mesh-cli --scan

# Headless mode (for testing — logs events, no TUI)
./build/mesh-cli --headless --debug

# Verbose logging
./build/mesh-cli --debug
```

## Key bindings

| Key | Action |
|-----|--------|
| Alt+1..0 | Switch to window N |
| Alt+a | Next active window |
| Alt+n / Alt+p | Next / previous window |
| PgUp / PgDn | Scroll scrollback |
| Ctrl+L | Redraw screen |
| Ctrl+C or /quit | Exit |

## Commands

| Command | Description |
|---------|-------------|
| `/help` | Show help |
| `/list` | List windows |
| `/nodes` | List known nodes |
| `/query <node\|nick>` | Open a DM window |
| `/msg <node\|nick> <text>` | Send a DM without switching |
| `/channel <n>` | Switch to channel N |
| `/window <N>` | Switch to window N |
| `/close` | Switch away from current window |
| `/clear` | Clear scrollback |
| `/info` | Show connection info |
| `/me <text>` | Send an action |
| `/reconnect` | Reconnect device |
| `/config` | Show device configuration |
| `/whois <node\|nick>` | Show detailed node information |
| `/raw [N]` | Show last N raw packets (hex dump) |
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
  mesh/         Protocol codec (protobuf), node DB, mesh service
  store/        SQLite persistence
  tui/          ncurses TUI: windows, input, status bar, commands
  util/         Logging, thread-safe queue, eventfd
  tests/        GoogleTest unit tests
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

- Database: `~/.local/share/mesh-cli/mesh.db`
- Log: `~/.local/share/mesh-cli/mesh-cli.log`

## Testing

```sh
# Unit tests (no device required)
./build/mesh-cli-tests

# Live integration test (requires a paired device in range)
./build/mesh-cli-live
```

The live test connects to the configured device, sends a channel broadcast
and a DM, and verifies the outgoing messages are persisted to SQLite.

## Known issues

- BLE connections can be unreliable if the device isn't paired. Use `mesh-cli pair` to establish a bond first.
- Meshtastic firmware holds a single bond; if the device is paired to a phone, pairing from mesh-cli will fail with `AuthenticationFailed`. Remove the bond on the phone first, then run `mesh-cli pair`.
- The `le-connection-abort-by-local` error on first connect is common; the client retries automatically.
- If GATT operations time out, the device may have dropped the connection. Use `/reconnect` or restart.
