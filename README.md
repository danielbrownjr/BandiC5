# BandiC5

**C5 Bandit** is a pocket dual-band RF scout for the Waveshare ESP32-C5-LCD-1.47.

The project is intentionally standalone. It is not part of the Dragon family and does not depend on dragon-core.

## Hardware target

- Waveshare ESP32-C5-LCD-1.47
- ESP32-C5 with 4 MB embedded flash
- 1.47-inch 172 x 320 ST7789 SPI LCD
- microSD slot on the shared LCD SPI bus
- onboard WS2812B RGB LED
- 2.4 GHz + 5 GHz Wi-Fi 6 radio

BandiC5 uses Waveshare's ESP-IDF BSP through the Espressif Component Manager instead of copying the vendor board-support source into this repository.

## v0.1 bring-up target

The first milestone is a real RF dashboard rather than a demo-only scaffold:

- portrait C5 BANDIT dashboard
- recurring Wi-Fi scans
- 2.4 GHz and 5 GHz AP counts
- strongest observed AP, RSSI, channel, and security mode
- total, open, and hidden AP counts
- scan generation counter
- serial summary for each completed scan

The current scanner uses normal Wi-Fi active scanning for fast discovery. Active scanning can transmit probe requests. BandiC5 is not doing packet capture, deauthentication, injection, or traffic interception.

SD logging, RGB status behavior, channel-density views, and optional GNSS are intentionally deferred until display + radio behavior is physically validated on the board.

## Build

The project targets **ESP-IDF v5.5.5** and **esp32c5**.

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```

The checked-in `sdkconfig.defaults` follows Waveshare's current ESP-IDF example configuration for this board.

## Board resources used by v0.1

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

The LCD and microSD share GPIO6/GPIO7. v0.1 does not mount the SD card yet.

## Project identity

- Repository/project name: **BandiC5**
- Device/UI name: **C5 Bandit**
- Role: pocket dual-band RF scout
