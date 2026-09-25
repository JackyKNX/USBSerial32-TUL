# USBSerial32-TUL

ESP-IDF based USB-to-KNX serial bridge firmware for **BUSWARE TUL ESP32-C3** hardware.

The firmware provides a transparent USB ↔ KNX TPUART transport while adding web-based management, MQTT status integration, OTA updates and diagnostics.

## About this project

This project is a TUL ESP32-C3 specific development based on the open-source **USBSerial32** project.

**Original USBSerial32 project:**  
https://github.com/tostmann/USBSerial32

**USBSerial32-TUL development and enhancements:**  
https://github.com/JackyKNX/USBSerial32-TUL

The TUL version adds hardware-specific support and further development for the BUSWARE TUL ESP32-C3 platform, including the WebManager, MQTT integration, OTA firmware updates and serial monitoring.

## Acknowledgements

Many thanks to **Thomas Ostmann** for the original [USBSerial32](https://github.com/tostmann/USBSerial32) project and for making the project available as open source under the Apache-2.0 license.

This project builds on that work and keeps the original project attribution.

## License

The upstream USBSerial32 project is licensed under the **Apache-2.0** license.

See the original project for the upstream license and copyright information.

---

## Design principle

The primary design goal is simple:

> **The KNX serial path has priority.**

The firmware is designed so that WebManager, Wi-Fi, MQTT and diagnostic functions are management features and must not become part of the KNX protocol path or block USB ↔ TPUART forwarding.

The ESP32 itself does not implement the KNX protocol. It provides a low-level byte-transparent transport between the NCN5130 TPUART interface and USB.

## Hardware

- BUSWARE TUL
- ESP32-C3
- NCN5130 KNX TPUART
- 4 MB flash
- USB Serial/JTAG

## Architecture

```text
KNX TP bus
    │
    ▼
NCN5130 TPUART
    │
    │ 38400 8E1
    ▼
ESP32-C3
    │
    │ transparent byte bridge
    ▼
USB Serial/JTAG
    │
    ▼
/dev/ttyACM0
    │
    ▼
knxd
    │
    ▼
KNX / openHAB
```

Management and diagnostics are provided separately:

```text
ESP32-C3
    │
    └── Wi-Fi
         └── HTTP WebManager
              ├── Status
              ├── Wi-Fi configuration
              ├── MQTT configuration
              ├── OTA firmware update
              └── Read-only serial monitor
```

Wi-Fi and HTTP are therefore management and diagnostic functions, not the KNX transport itself.

## Main features

- ESP32-C3 / BUSWARE TUL support
- NCN5130 TPUART interface
- transparent KNX TPUART ↔ USB bridge
- 38400 8E1 TPUART communication
- USB Serial/JTAG transport
- bidirectional traffic counters
- activity and system status
- Wi-Fi station mode
- fallback configuration access point
- persistent Wi-Fi configuration in NVS
- MQTT status integration
- WebManager
- web-based OTA firmware update
- browser-based read-only serial monitor
- system status API
- coredump support

## Serial monitor

The WebManager provides a read-only diagnostic monitor at:

```text
/serial
```

The monitor displays traffic observed by the bridge in both directions.

The monitor is intended for diagnostics only. It does not provide a path for sending data back into the KNX/USB transport.

The KNX bridge remains the primary function; diagnostic data is secondary and may be limited by the available diagnostic buffer.

## Current firmware version

The current TUL firmware version is **1.0.8**.

## Build environment

The currently validated development environment uses:

```text
ESP-IDF v6.1-dev-6443-g8bf9c476cfa
Python 3.13
Target: ESP32-C3
```

Load the ESP-IDF environment:

```bash
cd ~/Projects/USBSerial32-TUL
source ./set_env.sh
```

Verify:

```bash
idf.py --version
```

## Build TUL ESP32-C3

For a direct build:

```bash
cd ~/Projects/USBSerial32-TUL

idf.py \
  -DBW_PRODUCT=TUL \
  -DBW_VERSION=1.0.8 \
  build
```

The application binary is generated as:

```text
build/usbserial32.bin
```

The current application fits into the 1 MB OTA partition.

## Build script

`build.sh` supports four product/target combinations:

```text
tul_c6
eul_c6
tul_c3
eul_c3
```

Build all targets:

```bash
./build.sh
```

Build only TUL ESP32-C3:

```bash
./build.sh tul_c3
```

The script builds in a local temporary build directory and copies completed factory images to `dist/`.

## Flashing

### Normal development update — OTA

For an already running TUL device, use the WebManager OTA update.

1. Build the firmware.
2. Open the BUSWARE TUL WebManager.
3. Open **Firmware / OTA update**.
4. Select:

```text
build/usbserial32.bin
```

5. Upload the firmware.
6. Allow the ESP32 to restart.

**Do not use the merged factory image for a normal Web OTA update.**

### USB / factory flashing

For initial installation or a full image flash, generate the factory image:

```bash
idf.py \
  -DBW_PRODUCT=TUL \
  -DBW_VERSION=1.0.8 \
  merge-bin -o tul-c3.factory.bin
```

The factory image contains the bootloader, partition table and application image.

The generated file can then be flashed to the ESP32-C3 with `esptool`.

For example:

```bash
python -m esptool \
  --chip esp32c3 \
  --port /dev/ttyACM0 \
  write-flash 0x0 tul-c3.factory.bin
```

For a direct IDF-managed flash:

```bash
idf.py -p /dev/ttyACM0 flash
```

## Partition layout

The current 4 MB partition layout uses two 1 MB OTA application slots:

```text
nvs       0x009000    20 KB
otadata   0x00e000     8 KB
ota_0     0x010000     1 MB
ota_1     0x110000     1 MB
coredump   0x210000    64 KB
```

## Configuration and dependencies

The repository keeps the following project configuration files in source control:

```text
sdkconfig.defaults
dependencies.lock
```

Generated/local files are intentionally excluded by `.gitignore`, including:

```text
build/
managed_components/
dist/
sdkconfig
sdkconfig.old
```

## KNX / knxd

The ESP32 exposes the transparent USB serial transport to Linux.

A typical setup is:

```text
BUSWARE TUL
    │ USB
    ▼
Linux /dev/knx2
    │
    ▼
knxd
    │
    ▼
openHAB / KNX clients
```

Example `knxd` configuration:

```yaml
version: "3.8"
services:
  knxd:
    image: welteki/knxd:latest
    container_name: knxd-newesp32
    network_mode: host
    devices:
      - "/dev/knx2:/dev/knx2"
    command:
      - knxd
      - --eibaddr=1.1.128
      - --client-addrs=1.1.32:8
      - -D
      - -T
      - -R
      - -S
      - -i
      - -b
      - tpuarts:/dev/knx2
    restart: unless-stopped
```

The exact Linux device name depends on the host's udev configuration.

## Development status

**Status: Early development / active development**

The firmware has been built and tested on real BUSWARE TUL ESP32-C3 hardware with an NCN5130 TPUART interface and real KNX traffic.

The current development focus is to maintain reliable, transparent KNX serial communication while providing useful management, MQTT and diagnostic functionality.

## Repository

**USBSerial32-TUL:**  
https://github.com/JackyKNX/USBSerial32-TUL

**Upstream USBSerial32:**  
https://github.com/tostmann/USBSerial32
