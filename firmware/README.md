# DXMon Firmware

ESP32-S3 / LVGL firmware for the DXMon device.

## Hardware

- **Waveshare ESP32-S3-Touch-LCD-4.3B** -- 4.3", 800x480, 5-point capacitive
  touch, IPS. This specific board's own named panel-timing config is used
  (see Toolchain below) -- a different board in this family (e.g. the 5" or
  5B variants) will need different timing and is not a drop-in swap.
- A USB-C-to-C cable, connected to a direct rear/motherboard USB port rather
  than a hub or a front-panel header. This board's native USB-CDC has been
  observed to fail large uploads intermittently on marginal cables/ports --
  a proper cable on a direct port has been the reliable combination.

## Toolchain

This uses the **pioarduino** community fork of the `espressif32` platform,
not official PlatformIO's own `espressif32` package -- official PlatformIO
doesn't support the Arduino core version this board needs.

Pinned versions (`platformio.ini`):

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/51.03.07/platform-espressif32.zip
board = esp32-s3-devkitc-1
board_build.partitions = default_16MB.csv
board_upload.flash_size = 16MB
board_build.flash_size = 16MB
board_build.arduino.memory_type = qio_opi

lib_deps =
    https://github.com/esp-arduino-libs/ESP32_Display_Panel.git#v1.0.4
    https://github.com/esp-arduino-libs/ESP32_IO_Expander.git#v1.1.1
    https://github.com/esp-arduino-libs/esp-lib-utils.git#v0.3.0
    https://github.com/lvgl/lvgl.git#v8.4.0
```

ArduinoJson is pinned to **7.4.3** (the v7 API -- a single `JsonDocument`,
not the v6-style `StaticJsonDocument<N>`/`DynamicJsonDocument(N)`).

`ESP32_Display_Panel`, `ESP32_IO_Expander`, and `esp-lib-utils` aren't on
PlatformIO's own library registry by name -- they're pulled directly by
GitHub URL, pinned to exact tags. Don't unpin these without testing; the
named-board panel timing this project uses comes specifically from
`ESP32_Display_Panel` v1.0.4's own bundled board support for the 4.3B.

**Do not open the repo root in your IDE** -- open `firmware/` itself as the
project root, or PlatformIO's own toolchain integration (build/upload
buttons, IntelliSense) won't activate correctly.

## Before building

Two files need real values filled in before this will build/run correctly
against your own setup:

1. **`include/config.h`** -- set `DXMON_SERVER_HOST` and `DXMON_SERVER_PORT`
   to match wherever you deployed the [server](../server/README.md) (default
   port `8083`).
2. **`include/wifi_credentials.h`** -- copy `wifi_credentials.h.example` to
   `wifi_credentials.h` and fill in your real Wi-Fi SSID/password.
   `wifi_credentials.h` is gitignored -- never commit your real credentials.
   (There's no on-device Wi-Fi setup flow yet -- the Config screen's "Wi-Fi
   Setup" button is a visible, deliberately disabled placeholder for now.)

## Building and flashing

Using the PlatformIO CLI directly (adjust the path to your own PlatformIO
install if it's not on your system PATH):

```
platformio run -d <path-to-firmware-folder> -t upload
```

Or use PlatformIO's own Build/Upload buttons if you have it integrated into
your IDE, with `firmware/` open as the project root.

Watch the Serial Monitor on first boot to confirm Wi-Fi connects and the
first data fetch succeeds:

```
platformio device monitor -d <path-to-firmware-folder> -b 115200
```

## Known limitations

- **A display-rendering glitch** (content can drift a few pixels vertically
  over time, most noticeable as the tab bar) has a real, external root cause
  that wasn't found despite a thorough investigation. It's mitigated with a
  scheduled reboot every 15 minutes (`SCHEDULED_REBOOT_INTERVAL_MS` in
  `config.h`) -- a brief blank screen every interval, which fully clears it.
  This is expected, permanent behavior, not a bug to report.
- **No on-device Wi-Fi setup flow** -- Wi-Fi credentials are compiled in via
  `wifi_credentials.h` (see above). Changing networks means re-flashing.
- **Beam heading depends on the server-side lookup succeeding** -- see the
  [server README](../server/README.md)'s own note on this. If every heading
  is blank at once, that's a server-side issue, not a firmware one.

## Project structure

```
firmware/
├── platformio.ini
├── include/
│   ├── config.h                    -- server address, timing constants
│   ├── wifi_credentials.h.example  -- copy to wifi_credentials.h and fill in
│   └── dxmon_client.h              -- data structs + fetch function declarations
└── src/
    ├── main.cpp                    -- UI, navigation, screen logic
    └── dxmon_client.cpp            -- HTTP fetch + JSON parsing
```
