# ESP32 WAV Downloader & Player

A small **ESP-IDF** firmware for the **ESP32** that, on boot, connects to WiFi, downloads a
`.wav` file over HTTPS, stores it in on-board flash, and plays it out the ESP32's built-in
DAC to a speaker.

```
 ┌──────────┐   ┌──────────┐   ┌────────────┐   ┌───────────┐
 │  WiFi    │──▶│ HTTPS    │──▶│ LittleFS   │──▶│  DAC out  │
 │  connect │   │ download │   │ (flash)    │   │  (GPIO25) │
 └──────────┘   └──────────┘   └────────────┘   └───────────┘
```

---

## How it works

On startup ([`main/main.c`](main/main.c)):

1. **Mount storage** — LittleFS is mounted on the `storage` flash partition.
2. **Connect WiFi** — station mode, blocks until an IP is acquired.
3. **Download** — fetches `hello.wav` from a GitHub raw URL into `/storage/hello.wav`.
4. **Play** — streams the WAV's 8-bit samples to DAC channel 0 (GPIO25).

## Project structure

The logic is split into four self-contained ESP-IDF components:

| Component | Source | Responsibility |
|-----------|--------|----------------|
| `filesystem`   | [`components/filesystem/filesystem.c`](components/filesystem/filesystem.c)     | Mount LittleFS on the `storage` partition; read/write self-test |
| `wifi_manager` | [`components/wifi_manager/wifi_manager.c`](components/wifi_manager/wifi_manager.c) | Connect to WiFi in STA mode with auto-reconnect |
| `downloader`   | [`components/downloader/downloader.c`](components/downloader/downloader.c)     | Download a file over HTTPS (`esp_http_client` + cert bundle) |
| `aud_player`   | [`components/aud_player/aud_player.c`](components/aud_player/aud_player.c)     | Read a WAV file and push samples to the DAC |

```
esp_audio/
├── CMakeLists.txt          # top-level ESP-IDF project
├── partitions.csv          # custom partition table (4M app + 10M storage)
├── main/
│   ├── main.c              # app entry point + orchestration
│   └── idf_component.yml   # component-manager deps (littlefs)
└── components/
    ├── filesystem/
    ├── wifi_manager/
    ├── downloader/
    └── aud_player/
```

## Requirements

- **ESP-IDF v5.0 or newer** (developed against v6.0.2 — see [`dependencies.lock`](dependencies.lock))
- A **classic ESP32** board — the DAC (GPIO25/26) exists only on the original ESP32,
  not on the S2 / S3 / C3 variants.
- A speaker or small amplifier wired to **GPIO25**.

Dependencies are pulled automatically by the IDF component manager:
- [`joltwallet/littlefs`](https://components.espressif.com/components/joltwallet/littlefs) — flash filesystem

## Configuration

Before building, set your WiFi credentials in [`main/main.c`](main/main.c):

```c
#define WIFI_SSID      "your-ssid"
#define WIFI_PASSWORD  "your-password"
```

To play a different file, change the URL and destination path in the `download_task`
function of the same file.

## Build & flash

```bash
# 1. Set up the ESP-IDF environment (adjust path to your install)
. $HOME/esp/esp-idf/export.sh

# 2. Select the target and build
idf.py set-target esp32
idf.py build

# 3. Flash and open the serial monitor (replace with your port)
idf.py -p /dev/tty.usbserial-0001 flash monitor
```

Expected serial output:

```
=====================================
 ESP32 WAV Downloader
=====================================
I (xxx) FILESYSTEM: LittleFS mounted
I (xxx) WIFI: Connected. IP: 192.168.x.x
I (xxx) DOWNLOADER: Download Complete
I (xxx) AUD_PLAYER: Playing /storage/hello.wav
I (xxx) AUD_PLAYER: Playback completed
```

### Dev container

A [`.devcontainer`](.devcontainer) with the ESP-IDF toolchain and the ESP-IDF /
ESP-IDF-Web VS Code extensions is included for a ready-to-go build environment.

## Hardware wiring

| Signal      | ESP32 pin |
|-------------|-----------|
| Audio out   | GPIO25 (DAC channel 0) |
| GND         | GND       |

Connect GPIO25 to the input of a small amplifier or a speaker (through a suitable
driver), and share ground with the board.

## Known limitations / TODO

- **Audio playback is approximate.** `aud_player` skips a fixed 44-byte WAV header and
  assumes 8-bit / ~16 kHz / mono. It paces samples with a busy-wait
  (`esp_rom_delay_us(62)`), which blocks the CPU and drifts. Moving to **I2S** or a
  hardware timer would give clean, correct-rate playback and support standard WAV formats.
- **WiFi credentials are hardcoded** in `main.c`. Consider moving them to `menuconfig`
  (Kconfig) or NVS.
- **Partition mismatch:** [`partitions.csv`](partitions.csv) labels the `storage`
  partition subtype as `spiffs`, but the firmware mounts it as **LittleFS**. It works,
  but the label is misleading.
- No download retry / resume, and the played file is not deleted or cached between boots.
