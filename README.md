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


### TF hot-plug recovery

Starting with **v0.2.5 SD HOTPLUG**, storage can recover after card removal and reinsertion:

- a write or flush failure tears down the stale FAT mount instead of leaving the logger permanently stuck in `ERROR`
- BandiC5 keeps RF scanning while storage is unavailable
- the storage service retries mounting every few seconds
- a successful reinsertion creates a **new numbered session CSV**
- the dashboard badge returns to green `SD` automatically
- a missing/unmountable card shows gray `--`
- a mounted card with a filesystem/open failure remains red `!!`

Because the board exposes no card-detect GPIO through the current Waveshare BSP, hot-plug detection is software-driven from I/O failure plus periodic remount attempts.

## Phone OTA updates

This build is designed to be the last routine USB flash.

The 4 MB flash uses two OTA application slots plus OTA metadata. Future updates write the inactive slot, verify the image, switch the boot target, and reboot. Application rollback is enabled so a newly OTA-installed image is not accepted until the core display/radio/update-trigger path initializes successfully.

To enter update mode from a normally running C5 Bandit:

1. Hold the physical **BOOT** button for about 2 seconds. Do **not** hold BOOT while resetting, because that enters the ESP32-C5 ROM download bootloader instead of BandiC5 update mode.
2. C5 Bandit closes/syncs the TF session, stops normal RF scanning, and changes its screen to **UPDATE MODE**.
3. On the phone, join:
   - Wi-Fi: `BandiC5-Update`
   - Password: `bandic5ota`
4. Open Safari to `http://192.168.4.1`.
5. Choose the new `bandic5.bin` and upload it.
6. Keep power connected until the device reports **REBOOTING**.

The upload page streams the firmware directly into the inactive OTA slot, so it does not need enough RAM to hold the entire binary.

OTA mode is intentionally local and physically initiated. It is not exposed during ordinary scanning.

### HTTP task hardening

A captured serial panic from the original bootstrap showed the OTA server crashing before any firmware upload began:

- task: `httpd`
- panic: stack protection fault
- observed stack size: about 6 KiB
- crash occurred on the first phone HTTP traffic after joining the updater AP

The hardened updater build **`v0.2.3 OTA HTTPFIX`**:

- moves the 4096-byte OTA receive buffer to heap
- increases the HTTP server task stack to 12288 bytes
- adds a wildcard GET probe handler that returns HTTP 204 for captive-portal/internet-check paths
- logs the HTTP task stack high-water mark on root, probe, and upload entry
- retains the exact ELF and map in CI artifacts for future address decoding

### OTA stack-overflow fix

A physical OTA attempt exposed a stack-protection panic in the HTTP upload path. The original updater placed a 4096-byte receive buffer on a 6144-byte HTTP server task stack. The fix:

- moves the OTA receive buffer to heap storage
- increases the HTTP server task stack to 10240 bytes
- keeps the visible verification identity as **`v0.2.2 OTA STACKFIX`**
- retains `bandic5.elf` and `bandic5.map` in CI artifacts for future panic decoding

### OTA verification build

The first phone-OTA verification payload identifies itself as **`v0.2.1 OTA TEST`** on the normal dashboard, updater page, and serial boot log. That visible identity exists specifically to prove that a phone-uploaded application image actually booted after the slot switch.

The updater web UI uses browser upload progress and treats a connection loss after 100% of the file has been transmitted as an expected reboot condition rather than automatically reporting a failed update. The firmware also waits four seconds after returning the success response before rebooting.

### First OTA-capable installation

The first installation of this partition layout must still be flashed over USB because it changes the bootloader/partition-table environment.

Flash the complete CI artifact once using its generated `flash_args` / `flasher_args.json`. After that, routine application updates only need `bandic5.bin` through the phone update page.

## v0.3 optional uplink monitoring

BandiC5 can optionally join one saved Wi-Fi access point during normal scouting and expose a read-only live status page on that LAN.

The uplink is deliberately separate from the maintenance/update AP:

- normal operation remains Wi-Fi STA + RF scouting
- the saved uplink is optional; missing credentials leave BandiC5 in scout-only mode
- if the uplink disappears, RF scanning continues and reconnect attempts are throttled
- the existing physical BOOT gesture still switches the radio into the dedicated `BandiC5-Update` AP for OTA/recovery/configuration
- BandiC5 does **not** run its maintenance SoftAP and infrastructure STA simultaneously

### Configure an uplink

1. Run the desired v0.3 firmware.
2. Hold **BOOT** for about 2 seconds during normal operation.
3. Join `BandiC5-Update` with password `bandic5ota`.
4. Open `http://192.168.4.1`.
5. In **Uplink Wi-Fi**, enter the hotspot/router SSID and password.
6. Tap **Save Wi-Fi & reboot**.
7. After reboot BandiC5 performs its first RF scan, then attempts the saved uplink during the inter-scan idle window.

Leaving the SSID blank and saving clears the stored uplink configuration.

Credentials are stored in the ESP32-C5 NVS partition and are not written to the TF session CSV files or repository.

### Monitor BandiC5

When the STA receives an IP address:

- the dashboard shows a compact globe indicator: green while connected, gray while configured/disconnected, hidden when uplink is disabled
- the dashboard subtitle changes to the assigned numeric IP while connected
- serial logs report the SSID and assigned IP
- BandiC5 advertises the mDNS hostname `bandic5`
- the read-only status page is available at `http://bandic5.local/` when the LAN supports mDNS
- the numeric DHCP address from the serial log/router remains the fallback

The browser dashboard refreshes live status every two seconds and reports:

- firmware version
- uplink connection/IP
- 2.4 GHz and 5 GHz AP counts
- strongest observed AP, RSSI, and channel
- total/open/hidden counts and scan generation
- TF logging state
- uptime

A JSON representation is also available at `/status.json`.

Some hotspot implementations isolate connected clients from the hotspot host or from one another. In that case BandiC5 can still join the hotspot for connectivity, but direct browser monitoring from the isolated device may not work. A normal LAN/travel router without client isolation is the most predictable monitoring setup.

### Single-radio behavior

The ESP32-C5 has one Wi-Fi radio. Active dual-band scouting temporarily visits channels away from the associated AP. BandiC5 explicitly uses Espressif's home-channel dwell between scan channels to help preserve the infrastructure connection, but brief latency during scans is expected. Uplink monitoring is intentionally low-bandwidth and read-only.

## v0.4 web observability

v0.4 turns the existing uplink page into a practical read-only RF and logging console.

### Latest-scan AP table

The browser shows a table from the most recently **completed** RF scan:

- SSID
- BSSID
- 2.4 GHz / 5 GHz band
- channel
- RSSI
- authentication mode

Rows are sorted strongest-first.

The device keeps a bounded cache of the strongest 64 APs. If a scan sees more than 64 APs, the JSON response marks the table as truncated rather than consuming unbounded RAM. The browser never reads a half-built scan; the working set is published only when a complete scan succeeds.

The raw endpoint is:

`/aps.json`

SSID values are inserted into the browser through DOM `textContent`, not interpreted as HTML.

### TF session browser and downloads

When a TF card is mounted, the web page lists the newest 32 numbered BandiC5 sessions and provides a download link for each.

Downloads use a numeric session parameter:

`/download?session=17`

The firmware reconstructs the expected `session-0017.csv` path internally. Arbitrary filesystem paths are never accepted from the browser.

The current active session is marked separately and is intentionally not downloadable while it is still open for writing. Completed session files are stable and downloadable.

Session metadata is available from:

`/logs.json`

CSV files are streamed in small chunks rather than loaded into RAM.


### Mobile CSV download framing

v0.4.1 changes completed-session downloads from HTTP chunked transfer to a fixed
`Content-Length` response. The file is still streamed from TF in 2 KiB pieces,
but mobile browsers/download managers can now display the real attachment size
instead of `-1 byte` / unknown length.


### Single-radio CSV transfer guard

v0.4.2 serializes active RF scans and completed-session downloads with a shared
radio mutex. A download waits for any current channel sweep to finish, then
temporarily keeps the single ESP32-C5 Wi-Fi radio on the associated AP's home
channel while streaming the CSV. RF scanning resumes automatically as soon as
the transfer ends.

This addresses mobile transfers that received the correct `Content-Length` but
stalled at zero bytes when an active dual-band scan moved the STA off-channel.
The web UI also retains the last known status/table during brief server-busy
periods instead of replacing good data with a transient unavailable message.


### Browser-managed CSV downloads

v0.4.3 removes the hand-framed fixed-length HTTP response used in v0.4.1/v0.4.2.
Completed-session CSVs are again sent with ESP-IDF's supported chunked response
API, but the status page now fetches the file itself, tracks received bytes
against the known session size, assembles a browser Blob, and triggers the local
download only after the transfer completes.

While a file transfer is active:
- normal status/AP/session polling is paused
- the shared radio mutex prevents new RF sweeps
- the HTTP send timeout is extended to 20 seconds
- polling and scanning resume automatically after success or failure

This avoids mobile download-manager quirks around local chunked attachments and
also avoids leaving the ESP-IDF HTTP server in an inconsistent state after a
manually framed response.

### v0.3.1 recovery landmark

The branch `release/v0.3.1` points at the exact physically validated v0.3.1 mainline commit. The connected GitHub tooling used for this project can mutate branches/PRs but does not expose Git tag/release-asset creation, so this branch is the durable recovery pointer until a GitHub Release/tag is created separately.

## v0.4.5 static RC

This pass addresses the three merge blockers found by the adversarial re-review
of v0.4.4, plus several small safety fixes that were cheap to close before
hardware testing.

### Uplink connection state

- association/discovery now has a 15-second window instead of a 3-second total
  connect deadline
- `WIFI_EVENT_STA_CONNECTED` starts a separate 10-second DHCP window
- the CONNECTING state is published before `esp_wifi_connect()`, so an
  unusually fast GOT_IP event cannot be overwritten by the caller
- connect attempts now own the same radio mutex as scans and downloads for the
  asynchronous attempt lifetime
- LOST_IP is treated as a DHCP-recovery state instead of immediately issuing a
  second connect while still associated
- a deferred scout scan gets priority before another connect or download starts

### Completed-session downloads

- the 30-second total transfer cap is removed
- ESP-IDF's configured socket send timeout remains the server-side stalled-client
  bound
- the browser's 30-second timer is now inactivity-based and is re-armed whenever
  a stream chunk arrives
- completed sessions are validated/opened before attempting to reserve the radio
- downloads back off when a scout scan is already waiting
- the radio wait for a download is capped at one second
- an in-flight download exits at the next chunk boundary when uplink shutdown
  begins, so BOOT/OTA entry is not forced to wait for a healthy long transfer

### OTA rollback interaction

A pending OTA image is confirmed after the update AP and updater HTTP server
successfully start. That recovery path is sufficient evidence to make update
mode usable: a second firmware upload and a Wi-Fi-settings reboot can no longer
be rejected or silently roll the new image back during its normal health window.
Both POST handlers also confirm defensively, closing the tiny race between the
HTTP server accepting its first request and the control task's post-start check.

Normal scout-mode confirmation is still delayed until a successful scan/publish,
the healthy-runtime window, and first uplink-attempt resolution.

### Session creation safety

- directory enumeration errors are distinguished from normal end-of-directory
- new session files are created with exclusive-create semantics
- a case-variant or otherwise previously missed filename cannot be silently
  truncated by opening it with create-always behavior

## v0.4.4 hardening RC

This release-candidate pass follows an adversarial whole-repository review and
focuses on concurrency, recovery, and long-run reliability rather than adding
new user-facing features.

### TF/FAT lifetime safety

The storage module now owns a mutex and explicit completed-session reader leases.

- storage state becomes unavailable before unmount begins
- FAT/VFS unmount is deferred while any web reader is still open
- no new completed-session reader can open after teardown starts
- web downloads use storage-owned open/read/close wrappers instead of touching
  FATFS directly
- session listing is serialized with teardown and stats only the newest retained
  entries
- session allocation scans the directory once for the highest canonical session
  number instead of probing every number with repeated `stat()`
- session names must exactly match `session-NNNN.csv`
- failed initial session creation removes the incomplete file
- CSV cells beginning with spreadsheet formula trigger characters are prefixed
  to prevent formula execution when logs are opened in spreadsheet software

### Scout/uplink arbitration

Radio ownership is now nonblocking from the scout control loop.

- scans use a try-lock instead of waiting forever behind a browser transfer
- the control loop continues servicing BOOT/OTA, storage recovery, and uplink
  state while the radio is busy
- uplink connection attempts have an explicit connecting state and a 3-second
  bounded opportunity before the radio returns to scouting
- scan-start `ESP_ERR_WIFI_STATE` is treated as a deferral rather than a hard
  scan error
- DHCP lost-IP events are handled
- Wi-Fi configuration uses RAM storage; the BandiC5 NVS namespace remains the
  credential source of truth

### HTTP reliability

- status server enables LRU socket purge and TCP keepalive
- completed CSV transfers are bounded to 30 seconds server-side
- browser-side fetches are also aborted after 30 seconds
- normal page polling pauses during a CSV transfer
- downloads retain the radio guard but can no longer park the main control loop

### OTA and event-task hardening

- HTTP and mDNS startup moved out of the ESP-IDF system event task
- Wi-Fi/IP event callbacks now only update small state flags
- update and Wi-Fi-config POST receives abort after six consecutive socket
  timeouts instead of wedging update mode indefinitely
- pending OTA images are no longer confirmed immediately after task creation
- confirmation waits for a successful scan/publish path, a 30-second healthy
  runtime window, and resolution of the first uplink attempt when configured
- failed confirmation is retried rather than latched as successful

### Scan failure cleanup

Wi-Fi driver AP lists are explicitly cleared on count/allocation/retrieval
failure paths so a dense-environment allocation failure does not retain the
driver's scan result memory.

## Build

The project targets **ESP-IDF v5.5.5** and **esp32c5**.

```bash
idf.py set-target esp32c5
idf.py build
idf.py -p <PORT> flash monitor
```

The checked-in `sdkconfig.defaults` follows the Waveshare board baseline plus BandiC5's custom dual-slot OTA partition table and bootloader rollback support.

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
| BOOT / OTA request | GPIO28 |

The LCD and microSD share GPIO6/GPIO7 and use separate chip-select lines.

## Project identity

- Repository/project name: **BandiC5**
- Device/UI name: **C5 Bandit**
- Role: pocket dual-band RF scout
