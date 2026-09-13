# Fronius Solar Power Display — v2

A 20×4 character LCD that shows live PV production, grid import/export and grid-voltage
deviation, read from a Fronius inverter's **Solar API v1** over the local network.

v2 began as a port of the original `Fronius_Solar_Display` (ESP32 WROOM DA / DevKit V1) to
the **Waveshare ESP32-S3-ETH**. The application logic is the same; the board change forced a
new pinout, and a set of latent memory bugs were fixed along the way (see
[Bugs fixed in the port](#bugs-fixed-in-the-port)).

**That S3 board has since died and been replaced by a uPesy ESP-WROOM-32 DevKit** — back to
v1's board family, and with it v1's pinout (one wire excepted). Everything below describes
the WROOM-32; sections that only ever applied to the S3 are marked *retired*. All of the
v2 work above the hardware layer — the memory fixes, the WiFi supervision, the ARP scanner —
carried over untouched.

**Pre-port source (v1):** `X:\Code\PlatformIO\Projects\Fronius_Solar_Display` — a separate
git repo whose whole application lives in one file, `src/Display.cpp`. It is the reference
for anything in this document that says "v1 did X", and its `README.md` carries the wiring
table and Fritzing sketch the current pinout is derived from.

---

## Hardware

| | |
|---|---|
| **Board** | uPesy ESP-WROOM-32 DevKit (ESP32-D0WD, 4 MB flash, no PSRAM) |
| **Display** | HD44780-compatible 20×4 character LCD, 4-bit parallel mode, run at 5 V |
| **Contrast** | B10K potentiometer: outer legs to 5 V / GND, wiper to LCD pin 3 (V0) |
| **Network** | WiFi (STA) |
| **Input** | The on-board **BOOT** button (GPIO0) |

### Pinout

```
LCD (LiquidCrystal, 4-bit)      RS=13  EN=33  D4=14  D5=27  D6=26  D7=25
LCD power                       VDD + backlight anode -> 5V ; VSS + RW + cathode -> GND
Button                          GPIO0 (BOOT, INPUT_PULLUP, on-board — no wire)
```

All eight connections land on the **left header**, which runs top to bottom:

```
3V3 EN 36 39 34 35 32  33  25  26  27  14  12  GND  13  9 10 11  5V
                        E  D7  D6  D5  D4  ✗  rail  RS  ·  ·  ·  rail
```

So D7–D4 sit on four consecutive pads in descending order and no wire crosses the board.

**`RW` (LCD pin 5) must be tied to GND.** The 6-argument `LiquidCrystal` constructor never
drives RW, and grounding it pins the HD44780 in write-only mode — which is also what makes
the 3.3 V / 5 V mix safe. The LCD's data pins stay inputs and never drive 5 V back into a
non-5 V-tolerant GPIO. Leave RW floating and the display can drive the bus.

**Pins you must not use on this board:**

| GPIO | Why |
|---|---|
| **12** | **MTDI strapping pin — leave bare.** Sampled at reset to select the flash core voltage; held high it picks 1.8 V and the module will not boot or flash. v1 ran LCD EN here and survived (the HD44780's E input is high-impedance and never pulled it up), but the pad sits *inside* the run we use — between D4 and GND — so a miscount of one lands on it. EN was moved to 33. |
| **6–11** | SPI flash. Never usable. |
| **34–39** | Input only — cannot drive an LCD line. |
| **0, 2, 15** | Strapping. GPIO0 is the BOOT button (fine as an input); avoid the others for driven outputs. |
| **1, 3** | UART0 — the USB serial console. |

Free and safe for driven outputs: `4, 5, 13, 14, 16–19, 21–23, 25–27, 32, 33`.

### Power — the display needs an external 5 V supply

**USB power is not enough to run this build, and the way it fails looks like a wiring
fault.** Confirmed empirically during the WROOM-32 rewire: identical firmware, identical
soldering, blank or garbled LCD on USB, correct display the moment it ran from an external
5 V supply.

The load is the problem. A 20×4 module with its backlight is roughly 100–200 mA on its own,
and the ESP32 adds ~300 mA bursts every time the radio transmits. Between the host port's
current limit and the drop across the DevKit's protection diode, the 5 V rail sags under
those bursts. Two consequences, and neither announces itself:

- **Contrast is ratiometric.** V0 comes off a pot strung between 5 V and GND, so when VDD
  droops the contrast bias droops with it and the panel goes unreadable — looking exactly
  like a pot that needs adjusting.
- **A brownout mid-write desynchronises the 4-bit bus.** The HD44780 takes commands as two
  nibbles; interrupt it between them and every subsequent byte is shifted. The display then
  shows stable, confident garbage until it is power-cycled.

So: **USB for flashing and serial, external 5 V for running.** If the display misbehaves,
rule out the supply before touching the wiring.

> The same nibble-desync explains a symptom that is *not* a hardware fault: the HD44780 has
> no reset line, so it does not restart when the ESP32 does. `LiquidCrystal::begin()` assumes
> a controller that powered up in 8-bit mode, so re-running it against one already in 4-bit
> mode can leave the bus out of sync. After any reset loop, **power-cycle the LCD** before
> concluding anything about the wiring.

### The antenna switch (0 R resistor) — *retired with the S3 board*

Kept because the lesson generalises and [Radio tuning](#radio-tuning-tuneradio) refers back
to it. The WROOM-32 has a fixed module antenna and no such switch — one less
thing to get wrong.

**The S3 board shipped wired to its on-board PCB antenna, and plugging an antenna into the
IPEX connector did nothing until you moved a 0 Ω resistor.** From the
[Waveshare wiki](docs/ESP32-S3-ETH%20-%20Waveshare%20Wiki.pdf) FAQ, *"How to switch to the
external antenna for IPEX 1 generation"*:

> the default weld is a **vertical** 0R resistor. If you want to switch to an external antenna,
> you need to re-solder the 0R resistor to a **horizontal** position.

| Bridge | Selects |
|---|---|
| **Vertical** (factory default) | On-board PCB antenna |
| **Horizontal** | External antenna on the IPEX / u.FL connector |

This cost a lot of debugging time. The display sat at **-80 to -90 dBm from an AP 8 m away in
the same room, line of sight** — where free-space loss says it should read in the **-40s**. A
30-45 dB deficit is not distance, clutter or a bad router; it is a broken RF path. The
symptoms it produced looked exactly like flaky WiFi: dropped associations, inverter polls
timing out, and the AP periodically needing a restart before the display could rejoin (an
access point will give up on a station it can barely hear).

Two traps, both of which we walked into:

- **The naming is a coin-flip and the default is "internal".** Bridging *vertical* — the
  intuitive "I soldered the thing" move — selects the on-board antenna, i.e. the one you were
  trying to replace.
- **Soldering the antenna directly to the bridge pads "works", and is still wrong.** Any
  conductor on the RF feed radiates, so RSSI *improves* and the change looks correct. But the
  feed is a 50 Ω impedance-controlled node expecting exactly one load. Leave the vertical
  bridge in place and solder an antenna on top, and both radiators hang off it in parallel:
  the match is destroyed and much of the power reflects back into the radio. **Relative
  improvement proves the path was starved. It does not prove the new path is good** — only the
  absolute number does, and -85 dBm at 8 m is not good.

Correct configuration: horizontal bridge **only** (remove every trace of the vertical one),
antenna clicked onto the **u.FL connector**, never soldered to the pads.

### Ethernet — *retired with the S3 board*

The S3 board was an *ETH* variant with an on-board W5500; the firmware never used it, and
the `Ethernet_Generic` dependency and `W5500_SPI_*` build flags were removed during the v2
port (the library never read those macros, and two of the five pins were wrong anyway).
The WROOM-32 has no Ethernet at all, so this is now moot.

One landmine worth keeping if Ethernet ever comes back on any board: `Ethernet_Generic`'s
header is `Ethernet_Generic.h`, **not** `Ethernet.h`, and its `Dns_Impl.h` does
`#define DNS_PORT 53`, which macro-clobbers WiFiManager's `const byte DNS_PORT` unless
`WiFiManager.h` is included first.

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
pio run -e esp32-wroom-32-ota -t upload
```

`default_envs` pins the default to the USB env: a bare `pio run -t upload` will **not**
try to flash over OTA. Without that, PlatformIO runs every env and the OTA upload fails
with `Host solar-display.local Not Found`.

**The partition table is not the default, and must not go back to it.** `min_spiffs.csv`
gives each OTA slot 1'966'080 B. The stock 4 MB layout gives 1'310'720 B, and this firmware
is ~1'238'000 B — 94% full, with the overflow surfacing as a linker error that reads like a
code problem. Nothing here uses SPIFFS/LittleFS, so the smaller data partition costs nothing.

**COM port.** Unlike the S3's native USB, the DevKit's CP2102/CH340 bridge keeps a stable
COM number across resets, so pinning `upload_port` is safe — it is just unnecessary, and
`upload_port` is left unset so PlatformIO auto-detects. This board currently comes up on
**COM5**.

Unlike the S3, this board needs a **USB-serial driver** (CP210x or CH340, depending on which
bridge the board carries) — it does not enumerate as inbox USB CDC.

**Auto-reset into download mode does not work on this board.** `esptool` pulses DTR/RTS to
drive EN and GPIO0 through the usual two-transistor circuit; here the EN half works and the
GPIO0 half does not, so the chip resets and boots normally instead:

```
A fatal error occurred: Failed to connect to ESP32: Wrong boot mode detected (0x13)!
```

`0x13` is `SPI_FAST_FLASH_BOOT` — a normal boot, i.e. GPIO0 was never pulled low. **Hold the
BOOT button down through the `Connecting......` dots** and release it once writing starts.
Setting download mode by hand beforehand does *not* help on its own: esptool's own reset
pulse knocks the chip straight back out of it before the sync.

### Do not let a serial terminal assert DTR

**On this board, opening the serial port with DTR asserted factory-resets the device.**

DTR drives GPIO0 through the auto-reset circuit, so asserting it holds GPIO0 low.
`checkButton()` reads that as the BOOT button, and after `LOW` for three seconds it calls
`wm.resetSettings()` and `preferences.clear()` and reboots — wiping the WiFi credentials and
the inverter IP/MAC. With DTR held, this repeats every ~8 s:

```
WIFI: Button Pressed
WIFI: Button Held - erasing config, restarting
*wm:SETTINGS ERASED
rst:0xc (SW_CPU_RESET)
```

This bit us during the rewire and, because it also reset the ESP32 without resetting the
LCD, it produced garbled output that read convincingly as a soldering fault.

| Lines | Effect |
|---|---|
| DTR high, RTS low | GPIO0 **low** — the config-wipe trap |
| DTR low, RTS high | EN low — clean reset, GPIO0 stays high |
| Both low (or both high) | Idle — safe for passive monitoring |

`pio device monitor` is safe. Raw `System.IO.Ports.SerialPort` is **not**: it defaults to
`DtrEnable = false`, but any code that sets it true will wipe the device. To reset the board
cleanly from a terminal, pulse RTS alone.

This is worth guarding in firmware — ignore `TRIGGER_PIN` for the first few seconds after
boot, or trigger on a press edge rather than a level — but it is not fixed yet.

> The S3-era advice "if the board does not enumerate, suspect the LCD pinout" no longer
> applies — that was specific to GPIO 19/20 being the S3's native USB lines. On the
> WROOM-32 the USB bridge is independent of every pin the LCD touches.

### Building from the `X:` share

This tree lives on a VPN-mounted SMB share, and that breaks two things.

**SCons cannot write its signature database over SMB.** A build dies at the end with
`OSError: [Errno 22] Invalid argument: ...\.pio\build\...\.sconsign311.tmp`, *after* the
compile has already succeeded — so it looks like a code failure but isn't. Put the build
directory on a local disk:

```bash
PLATFORMIO_BUILD_DIR=C:/pio-build/fronius-display-v2 pio run
```

**The VPN and the display are mutually exclusive.** `X:` *is* the VPN, but the display is
on the local LAN and OTA can't reach it while the VPN is up. So the order is: build with the
VPN up (source must be reachable), drop the VPN, then flash. Once the VPN is down the source
tree is gone, so PlatformIO can't run — push the already-built binary with `espota.py`
directly, which needs no project directory:

```bash
python "$HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py" \
  -i solar-display.local -p 3232 -a changeme \
  -f C:/pio-build/fronius-display-v2/esp32-wroom-32/firmware.bin -r -d
```

---

## Runtime state machine

`loop()` is a cascade of guards. Each stage runs only once the previous one is satisfied,
and most stages `return` so a single iteration does one thing.

```
                    ┌──────────────┐
  boot ───────────► │ splash (LCD) │
                    └──────┬───────┘
                           ▼
              boot connect loop (blocking)  2 × 10 s on saved creds; see below
                           ▼
                  wm.autoConnect()          already connected → returns true
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
        manageWiFi()  ◄─── every loop; see WiFi supervision below
              │
       WL_CONNECTED? ── no ──► reconnect / AP portal, and do nothing else
              │ yes
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

### Boot connect loop (before WiFiManager runs)

`wm.autoConnect()` cannot be trusted to actually *try*. It makes exactly **one** attempt
with **no timeout and no retries** (`_connectTimeout = 0`, `_connectRetries = 1`), and the
Arduino 3.x core fails that attempt in ~0 ms on any transient bring-up hiccup —
`enableSTA()` failing, `ESP_NETIF_STARTED_BIT` not arriving within 1 s, or
`esp_wifi_connect()` erroring. All of those are `log_e()` only, invisible at the default
`CORE_DEBUG_LEVEL=0`. The observed symptom: the device jumps into the AP portal
*immediately* after reset, with none of the usual 3–5 s connect lag, even though the router
is up and the credentials are good.

So `setup()` now does it the old, classic way first: if credentials are saved, a blocking
`WiFi.begin()` + wait loop, retried (`WIFI_BOOT_ATTEMPTS` × `WIFI_BOOT_ATTEMPT_MS`,
2 × 10 s), with the attempt counter on the LCD. **No other code runs until this window has
passed.** Re-issuing `begin()` re-runs the whole STA bring-up, which is precisely the part
that fails on the bad boots. Only after every attempt has failed is `wm.autoConnect()`
allowed to fall through to the AP portal; when the loop has already connected,
`autoConnect()` sees `WL_CONNECTED` and returns true without touching anything.

### WiFi supervision (`manageWiFi()`)

**Nothing else re-establishes the station link.** Not WiFiManager: its ESP32 disconnect
handler only calls `WiFi.reconnect()` under `#ifdef esp32autoreconnect`, which is not
defined. And not the Arduino core's silent auto-reconnect, because of this, in
`WiFiManager::startConfigPortal()`:

```cpp
if(_disableSTA || (!WiFi.isConnected() && _disableSTAConn)){   // _disableSTAConn defaults true
  WiFi_Disconnect();
  WiFi_enableSTA(false);            // <-- the station interface is switched OFF
}
```

Raising the soft-AP portal turns the station **off**. A device parked in that portal cannot
see the router come back — there is nothing left to see it with. Neither v1 nor early v2 had
any code to get out again, so a router outage was a one-way trip: poll errors → reboot →
`autoConnect()` fails while the router is still down → AP portal → stranded until someone
power-cycled it.

`manageWiFi()` runs once per `loop()` and owns every transition:

| State | Action |
|---|---|
| **Rising edge** (link came back) | Tear down the AP portal, clear `connectErrors`, start OTA if it was never started, repaint the LCD, force an immediate poll |
| **Falling edge** (link dropped) | Note the time, tell the user, stop polling |
| **Offline, no portal** | `WiFi.begin()` (saved NVS creds) every **20 s** |
| **Offline > 2 min** | Raise the AP portal so the user *can* re-provision — without giving up on the saved network |
| **AP portal idle > 3 min** | Take it down, switch the station back on, retry the saved credentials. If the router is still gone, the 2-minute timer raises the AP again |
| **AP portal, client connected** | Leave it up. `WiFi.softAPgetStationNum() > 0` keeps pushing the deadline out, so it never dies under someone who is mid-configuration |
| **AP portal, no saved SSID** | Leave it up forever. A fresh device has nothing to fall back to |

So the device alternates AP ⇄ STA instead of committing to either, and always finds the
router again on its own.

Two supporting changes make this work:

- **`wm.setConfigPortalTimeout(0)`.** WiFiManager's own portal timeout would shut the AP down
  and leave the device with *neither* a portal nor a station link — LCD frozen on the "join
  AP" screen, permanently. Portal lifetime is `manageWiFi()`'s job now.
- **The poll loop returns early unless `WL_CONNECTED`.** Without that guard a WiFi outage
  looked exactly like a dead inverter: the 5-second poll kept firing, every HTTP GET failed,
  and six failures rebooted the box straight into the AP portal. The inverter is not at fault
  for the router being down, and `connectErrors` no longer says it is.

### Radio tuning (`tuneRadio()`)

Written while the display was reading **-80 to -90 dBm** and the link was dropping constantly.
That turned out to be a hardware fault, not a site problem — see
[The antenna switch](#the-antenna-switch-0-r-resistor--retired-with-the-s3-board). Do not read this section as evidence
that the installation is inherently marginal; **8 m of clear line-of-sight should read in the
-40s**, and if it doesn't, fix the antenna before touching anything here.

The tuning is still worth having — it is what you want on any WiFi link you cannot make short
— but it buys single-digit dB. It cannot rescue a broken RF path, and it was never the reason
the display kept falling off the network.

`tuneRadio()` trades away everything we don't need for link margin:

| Setting | Why |
|---|---|
| `WiFi.setSleep(false)` | **The important one.** The default `WIFI_PS_MIN_MODEM` parks the radio between beacons. At -85 dBm missed beacons are common, and a run of them drops the association. The display is mains-powered — there is nothing to save |
| `setTxPower(WIFI_POWER_19_5dBm)` | Full TX. See the caveat below |
| `esp_wifi_set_bandwidth(HT20)` | A 40 MHz channel spreads the same power over twice the bandwidth: ~3 dB of receive sensitivity traded for throughput we have no use for (the payload is a few kB every 5 s) |
| `esp_wifi_set_protocol(11B\|11G\|11N)` | Keep 802.11b. Dropping it as "legacy" is the obvious move and it is wrong: the b rates go down to 1 Mbps and have the best receiver sensitivity in the set — they are the rates that still carry a frame at -90 dBm |
| `setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL)` | If the SSID is served by more than one radio, take the loudest rather than the first one found |

> ⚠️ **TX power cannot improve the RSSI on the LCD.** That number is how well the *display
> hears the router*; TX power governs how well the *router hears the display*. No transmitter
> can talk itself louder into its own receiver. Expect the on-screen figure not to move — what
> should improve is the AP no longer losing us mid-conversation.

**`tuneRadio()` is re-applied on every reconnect, not just at boot.** A `WIFI_STA` →
`WIFI_AP` mode change — precisely what a trip through the config portal is — resets
power-save, bandwidth and TX power to the IDF defaults. Setting them once in `setup()` would
silently lose them the first time the display fell back to the AP, i.e. exactly when they
matter most.

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

Reference documents, both in [docs/](docs/):

- [solar_api.pdf](docs/solar_api.pdf) — Fronius Solar API v1 specification. Section **4.8**
  covers `GetMeterRealtimeData`; **4.8.5** defines the channels used here.
- [solarApiv1.json](docs/solarApiv1.json) — JSON schema for the same responses.

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

The meter to read it from is **not** at a fixed key. `Body.Data` is an object keyed by meter
*device ID*, and those IDs are assigned by the Datamanager: this system, with a grid meter
and a second one on the Carport, reports them as `"2"` and `"3"` — there is no `"0"` at all.
Both v1 and early v2 hardcoded `Body.Data.0`, so the lookup silently missed and the display
sat at `0%gV` forever.

`getGridVoltage()` now selects by `Meter_Location_Current` (spec 4.8.5):

| Value | Meaning | Used? |
|---|---|---|
| `0` | Grid interconnection point (primary meter) | **Yes — this is the one we want** |
| `1` | Load path (primary meter) | Fallback only |
| `3` | External generator (secondary, e.g. the Carport meter) | Never |
| `256`–`511` | Subloads (secondary) | Never |

A secondary meter reports the voltage at *its own* connection point, not at the grid feed­-in
point, so it must never be picked for gV.

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
