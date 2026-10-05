# BandiC5

**C5 Bandit** is a pocket dual-band RF scout for the Waveshare ESP32-C5-LCD-1.47.

The project is intentionally standalone. It is not part of the Dragon family and does not depend on dragon-core.

## Hardware target

- Waveshare ESP32-C5-LCD-1.47
- ESP32-C5 with 4 MB embedded flash
- 1.47-inch 172 x 320 ST7789 SPI LCD
- microSD/TF slot on the shared LCD SPI bus
- onboard WS2812B RGB LED
- 2.4 GHz + 5 GHz Wi-Fi 6 radio

BandiC5 uses Waveshare's ESP-IDF BSP through the Espressif Component Manager instead of copying the vendor board-support source into this repository.

## v0.1 RF dashboard

The first milestone established the physically validated live RF dashboard:

- portrait C5 BANDIT dashboard
- recurring Wi-Fi scans
- 2.4 GHz and 5 GHz AP counts
- strongest observed AP, RSSI, channel, and security mode
- total, open, and hidden AP counts
- scan generation counter
- serial summary for each completed scan

The scanner uses normal Wi-Fi active scanning for fast discovery. Active scanning can transmit probe requests. BandiC5 is not doing packet capture, deauthentication, injection, or traffic interception.

## v0.2 TF session logging

When a compatible FAT-formatted TF/microSD card is present at boot, BandiC5:

- mounts the card through the Waveshare BSP
- creates `/bandic5` on the card if needed
- selects the first unused numbered session file from `session-0001.csv` through `session-9999.csv`
- logs one CSV row for every AP observed in every completed scan
- flushes and syncs the file every three scans
- shows a compact storage badge on the dashboard:
  - `SD` in green: logging active
  - `--` in gray: no usable card at boot
  - `!!` in red: logging failed after mount/startup
- continues scanning normally when storage is unavailable

CSV columns:

```text
uptime_ms,scan,bssid,ssid,rssi,channel,band,auth,hidden
```

Example:

```text
8421,3,"AA:BB:CC:DD:EE:FF","MyWifi",-47,149,5GHz,WPA3,0
```

The logger uses monotonic milliseconds since boot because this board has no configured real-time clock or network time source. Future GNSS support can add absolute time and position without replacing the basic per-AP record format.

Hot insertion/removal is not supported yet. Card state is established at boot, and runtime write/flush failures move the storage indicator to the error state.

## Build

The project targets **ESP-IDF v5.5.5** and **esp32c5**.

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```

The checked-in `sdkconfig.defaults` follows Waveshare's current ESP-IDF example configuration for this board.

## Board resources

| Resource | Assignment |
| --- | --- |
| LCD SPI clock | GPIO7 |
| LCD SPI MOSI | GPIO6 |
| LCD CS | GPIO23 |
| LCD D/C | GPIO24 |
| LCD reset | GPIO26 |
| LCD backlight | GPIO10 |
| microSD MISO | GPIO5 |
| microSD CS | GPIO4 |
| WS2812B | GPIO8 |

The LCD and microSD share GPIO6/GPIO7 and use separate chip-select lines.

## Project identity

- Repository/project name: **BandiC5**
- Device/UI name: **C5 Bandit**
- Role: pocket dual-band RF scout
