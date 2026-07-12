# Fronius Solar Power Display — v2

A 20×4 character LCD that shows live PV production, grid import/export and grid-voltage
deviation, read from a Fronius inverter's **Solar API v1** over the local network.

v2 is a port of the original `Fronius_Solar_Display` (ESP32 WROOM DA / DevKit V1) to the
**Waveshare ESP32-S3-ETH**. The application logic is the same; the board change forced a
new pinout, and a set of latent memory bugs were fixed along the way (see
[Bugs fixed in the port](#bugs-fixed-in-the-port)).

---

## Hardware

| | |
|---|---|
| **Board** | Waveshare ESP32-S3-ETH (ESP32-S3, 16 MB flash, 8 MB **octal** PSRAM) |
| **Display** | HD44780-compatible 20×4 character LCD, 4-bit parallel mode |
| **Network** | WiFi (STA). The board's W5500 Ethernet is **not used** — see [Ethernet](#ethernet-not-used) |
| **Input** | The on-board **BOOT** button (GPIO0) |

### Pinout

```
LCD (LiquidCrystal, 4-bit)      RS=15  EN=16  D4=17  D5=18  D6=47  D7=48
Button                          GPIO0 (BOOT, INPUT_PULLUP)
```

**Pins you must not use on this board:**

| GPIO | Why |
|---|---|
| **19, 20** | ESP32-S3 native USB (`USB_D−` / `USB_D+`). Driving these from the LCD **stops the board enumerating over USB** — no COM port, no flashing, no serial. This bit us: the original DevKit pinout put LCD D6/D7 here. |
| **26–37** | Consumed by flash + octal PSRAM (`board_build.arduino.memory_type = qio_opi`). |
| **9–14** | Wired to the on-board W5500 Ethernet controller. Free in practice since we don't use Ethernet, but leave them alone unless you enable it. |
| **0, 3, 45, 46** | Strapping pins. GPIO0 is the BOOT button (fine as an input); avoid the others for driven outputs. |
| **48** | Often the on-board WS2812 RGB LED. Used here for LCD D7 — worst case the LED flickers on LCD writes. |

Free and safe: `4–8, 15–18, 21, 38–44, 47`.

### Ethernet (not used)

The board is an *ETH* variant, but the firmware is WiFi-only. The `Ethernet_Generic`
dependency and the `W5500_SPI_*` build flags were removed: the library never read those
macros (they configure nothing), two of the five pins were wrong anyway, and no code
called `Ethernet` at all.

To enable it later, the W5500 sits on **SCK 13 / MISO 12 / MOSI 11 / CS 14 / INT 10 / RST 9**,
and must be brought up explicitly:

```cpp
SPI.begin(13, 12, 11, 14);
Ethernet.init(14);
Ethernet.begin(mac);
```

Note `Ethernet_Generic`'s header is `Ethernet_Generic.h`, **not** `Ethernet.h`, and its
`Dns_Impl.h` does `#define DNS_PORT 53`, which macro-clobbers WiFiManager's
`const byte DNS_PORT` unless `WiFiManager.h` is included first.

---

## Layout

```
src/main.cpp                      everything: state machine, Fronius client, LCD, OTA
src/LiquidCrystal-master/         vendored HD44780 driver
lib/netscanner-lib/               ARP-table scanner (find a host by IP or by MAC)
platformio.ini                    two envs: USB flash + OTA flash
```

## Build & flash

```bash
# USB is the default env — this is the normal way to flash
pio run -t upload
pio device monitor -b 115200

# Opt in to a network flash (board must already be running OTA firmware)
pio run -e waveshare-esp32-s3-eth-ota -t upload
```

**Don't hardcode a COM port.** The ESP32-S3's native USB re-enumerates on every reset, and
Windows can hand it a *different* COM number each time — a pinned `--upload-port COM4` will
go stale on its own. `upload_port` is intentionally unset so PlatformIO auto-detects.

`default_envs` pins the default to the USB env: a bare `pio run -t upload` will **not**
try to flash over OTA. Without that, PlatformIO runs every env and the OTA upload fails
with `Host solar-display.local Not Found`.

No Windows driver is needed. The ESP32-S3's native USB enumerates as USB CDC
(`usbser.sys`, inbox on Win10/11); in download mode it appears as a "USB JTAG/serial debug
unit", also inbox. To force download mode: hold **BOOT**, tap **RESET**, release RESET,
release BOOT.

> If the board does *not* enumerate, suspect the LCD pinout before suspecting drivers.

---

## Runtime state machine

`loop()` is a cascade of guards. Each stage runs only once the previous one is satisfied,
and most stages `return` so a single iteration does one thing.

```
                    ┌──────────────┐
  boot ───────────► │ splash (LCD) │
                    └──────┬───────┘
                           ▼
                  wm.autoConnect()          saved WiFi creds
                           │
              ┌────────────┴────────────┐
         connected                  not connected
              │                          │
              │                          ▼
              │                 ┌──────────────────┐
              │                 │ CONFIG PORTAL    │  AP: SOLAR_POWER_DISPLAY
              │                 │ (WiFiManager)    │  user enters WiFi + inverter IP
              │                 └────────┬─────────┘
              ▼                          │
       ┌─────────────┐ ◄─────────────────┘
       │ OTA::begin  │  (lazy: also fires if WiFi arrives later)
       └──────┬──────┘
              ▼
      inverterIPSet?  ── no ──► open config portal, wait
              │ yes
              ▼
      inverterMACSet? ── no ──► resolveMAC()      ARP, non-blocking, retry 1s ×15 → 30s
              │ yes
              ▼
      initSuccess?    ── no ──► runInverterHandshake()
              │ yes                 ├─ netscanner: is IP_char still our MAC?
              │                     │    ├─ no  → re-find by MAC → save → reboot
              │                     │    └─ gone → clear IP → reboot
              │                     └─ GET /solar_api/GetAPIVersion.cgi → must be 1
              ▼
    ┌────────────────────────┐
    │ POLL every 5s          │  getInverterData()  + getGridVoltage()
    │  → drawPowerScreen()   │  >5 consecutive errors → reboot
    └────────────────────────┘  errors forgotten after 5 min
```

### Persisted state (`Preferences`, namespace `inverter_config`)

| Key | Type | Meaning |
|---|---|---|
| `inverter_IP` | string | Inverter's IPv4, entered by the user in the portal |
| `inverter_MAC` | string | Resolved via ARP. The **stable identity** — IPs move, MACs don't |
| `inverterIPSet` | bool | Gate for the portal |
| `inverterMACSet` | bool | Gate for `resolveMAC()` |

WiFi credentials are stored separately by WiFiManager.

### Why the MAC matters

The inverter is found by **IP**, but identified by **MAC**. On every boot
`runInverterHandshake()` ARPs `inverter_IP` and compares the answer to the stored MAC:

- **Match** → proceed.
- **Different MAC** → some *other* host now holds that IP. Clear `inverterIPSet`, reboot.
- **No answer** → the inverter's DHCP lease moved. `netscanner.findIPbyMAC()` sweeps the
  /24 with ARP requests, finds the new IP, saves it, reboots.

This is what makes the display survive a DHCP reshuffle without the user touching it.
The sweep is slow by design (254 addresses × 500 ms ≈ **2 minutes**) — hence the
"please be patient" screen.

---

## Fronius Solar API

Base: `http://<inverter_ip>`

| Endpoint | Used for |
|---|---|
| `/solar_api/GetAPIVersion.cgi` | Handshake. `APIVersion` must be `1`, else reboot. |
| `/solar_api/v1/GetPowerFlowRealtimeData.fcgi` | PV production, grid flow |
| `/solar_api/v1/GetMeterRealtimeData.cgi?Scope=System` | Per-phase grid voltage |

**PV production** sums `Body.Data.Inverters[1..9].P`, and — when `inverterAsSmartMeter` is
true — also adds `SecondaryMeters[1..9].P` for meters whose `Category` is
`METER_CAT_WR`, `METER_CAT_BAT`, or `METER_CAT_PV_BAT` (battery / secondary PV).

**Grid flow** is `Body.Data.Site.P_Grid`:

- `P_Grid > 0` → **importing** from the grid. Rendered `<  <  <  1.2kW`.
- `P_Grid ≤ 0` → **exporting**. Rendered `1.2kW  >  >  >`.

**Home consumption** = PV + grid. (Computed, but not currently shown.)

**Voltage deviation** takes the phase furthest from nominal 230 V and reports it as a
percentage — `100%` is nominal, `106%` means a phase is sitting ~244 V.

## The display

```
  ┌────────────────────┐
  │  ☼☼ 4.2kW        /\│   row 0: PV production, pole top
  │/\                | |│   row 1: house roof, pole
  │|_|  <  <  <  820W| |│   row 2: house base, grid flow + arrows
  │  101%gV 0E -54dRSSI │   row 3: voltage deviation, error count, WiFi RSSI
  └────────────────────┘
```

The HD44780 has **8 CGRAM slots**. Slots 0–3 hold the house (set once in `setup()`);
slots 4–7 hold the power pole (re-uploaded on each redraw in `drawPowerScreen()`).

---

## OTA

`ArduinoOTA` on the default port 3232, advertised over mDNS as `solar-display.local`.

- Configured in `setupOTA()`, lazily — it starts whenever WiFi first connects, whether at
  boot or later via the portal.
- `initSuccess` is cleared on OTA start so the 5-second poll loop stops writing to the LCD
  mid-flash.
- Progress is drawn as a percentage plus a 20-cell bar on the bottom row.

> ⚠️ **`OTA_PASSWORD` is `"changeme"`.** Anyone on the LAN can reach port 3232. Change it in
> [src/main.cpp](src/main.cpp), and keep `upload_flags = --auth=...` in
> [platformio.ini](platformio.ini) in sync.

---

## Bugs fixed in the port

v1's shipped firmware had three memory bugs. They are fixed here; if you ever go back to
the v1 tree, they are still there.

| Bug | Detail |
|---|---|
| **Stack overflow on every redraw** | The grid-flow string was built with `strcat()` into `char buffer[7]` / `char gridArrows[12]`. The export branch appended 8 bytes to an already-full 7-byte buffer — ~15 bytes into 7. It fired *whenever the system was exporting*, i.e. most of a sunny day. Now one `snprintf` into a correctly-sized buffer. |
| **URL buffer overflow** | `char serverPathData[64]` for `"http://"` (7) + IP (≤15) + endpoint (43) + NUL = **66**. Now `URL_LEN = 96` with a checked `snprintf`. |
| **`putString(key, 0)`** | The literal `0` binds to `const char*` → **nullptr**. Three call sites. Replaced with clearing the corresponding bool flag. |
| **Infinite loops in netscanner** | `for (char i = 1; i < 255; i++)` — a signed `char` overflows at 127 and never reaches 255. Three occurrences. Now `int`. |
| **`splitIp()` self-destruction** | It ran `strtok()` directly on `interface_ip`, overwriting the `.` separators with NULs, so the member was destroyed by the first call. Now works on a copy via `strtok_r`. |
| **Null-deref in `resolveMAC()`** | `etharp_find_addr()` returns nothing on the first call (ARP is async), and v1 dereferenced the null result. v1 papered over it with `delay(100)`; v2 returns early and retries on a timer (1 s × 15, then 30 s). |
| **`tcpip_adapter` removed in IDF 5** | netscanner used `tcpip_adapter_get_ip_info()` / `TCPIP_ADAPTER_IF_STA`, gone since ESP-IDF 5. Replaced with `WiFi.localIP()` and `netif_default`. |

### Also changed

- **`.ino` → `.cpp`**: the Arduino IDE auto-generates function prototypes; PlatformIO does
  not. Explicit forward declarations are now required and present.
- **`autoConnect()` instead of unconditional `startConfigPortal()`** — v1 forced the portal
  open on every boot and never tried saved credentials.
- **Portal restart loop** — v1's `loop()` called `startConfigPortal()` on *every iteration*
  while the IP was unset, continuously restarting it. Now guarded by `portalRunning`.
- **Wrong callback** — v1 wired `setSaveConfigCallback(saveParamCallback)`, so saving *WiFi
  credentials* set `inverterIPSet = true` even if no inverter IP was ever entered. Only
  `setSaveParamsCallback` is wired now.
- **IP validation** (`isValidIP`) before persisting; garbage no longer gets saved and fed to ARP.
- **MAC parameter field** was 15 chars for a 17-char MAC — it truncated. Now 18.
- `strcpy` → `strlcpy` throughout; `connectErrors` now resets on a successful poll.

---

## Known gaps

- `homeConsumption` is computed but never displayed.
- The netscanner MAC sweep blocks for ~2 minutes; it only runs on the recovery path, but it
  does freeze the UI while it runs.
- No TLS — the Solar API is plain HTTP on the LAN.
- `DEVICE_ID` is a compile-time constant shared by every unit built from this tree.
