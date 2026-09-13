// Fronius Solar Power Display - v2
// Ported from Fronius_Solar_Display (ESP32 WROOM DA) to the Waveshare ESP32-S3-ETH.
// Network scanner: https://github.com/agentzex/ESP_network_ip_scanner

#include <Arduino.h>

// WiFi & Network
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_wifi.h>     // ps / bandwidth / protocol / tx-power knobs Arduino doesn't expose
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/tcpip.h>   // LOCK_TCPIP_CORE / UNLOCK_TCPIP_CORE
#include <netscanner.h>

// JSON, HTTP GET
#include <ArduinoJson.h>
#include <HTTPClient.h>

// Over-the-air firmware update
#include <ArduinoOTA.h>

// Local Library (Ensure folder structure is src/LiquidCrystal-master/src/)
#include "LiquidCrystal-master/src/LiquidCrystal.h"

/* ---- DEBUG SECTION ---- */
#define DEBUG //Comment out for production use
#ifdef DEBUG
#define DEBUG_PRINT(x)       Serial.print(x)
#define DEBUG_PRINTLN(x)     Serial.println(x)
#define DEBUG_PRINTF(...)    Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)
#endif
/* ---- END DEBUG SECTION ---- */

#define TRIGGER_PIN 0   // BOOT button on the ESP32-S3

const char DEVICE_VERSION[]   = "1A";
const char SOFTWARE_VERSION[] = "002_ALPHA";
const char DEVICE_ID[]        = "4280d4f3-204c-4bd8";
const char DEVICE_NAME[]      = "SOLAR POWER DISPLAY";
const char WIFI_APN[]         = "SOLAR_POWER_DISPLAY";

// OTA. The hostname is what appears in PlatformIO's `--upload-port` / mDNS.
// OTA_PASSWORD gates who may flash the device: anyone on the LAN can reach
// port 3232, so leaving this empty means anyone on your network can overwrite
// the firmware. Change it.
const char OTA_HOSTNAME[] = "solar-display";
const char OTA_PASSWORD[] = "changeme";

// Fronius Solar API v1 endpoints
const char HTTPSTRING[]      = "http://";
const char APIDATA[]         = "/solar_api/GetAPIVersion.cgi";
const char INVERTERDATA[]    = "/solar_api/v1/GetPowerFlowRealtimeData.fcgi";
const char INVERTERVOLTAGE[] = "/solar_api/v1/GetMeterRealtimeData.cgi?Scope=System";

// "xxx.xxx.xxx.xxx" = 15 chars + NUL
static const size_t IP_LEN  = 16;
// "xx:xx:xx:xx:xx:xx" = 17 chars + NUL
static const size_t MAC_LEN = 18;
// Longest URL is HTTPSTRING + a full IP + INVERTERVOLTAGE. v1 sized this by eye and
// overflowed: 7 + 15 + 43 + NUL = 66 did not fit its char[64].
static const size_t URL_LEN = 96;

static const uint32_t POLL_INTERVAL_MS  = 5000;
static const uint32_t ERROR_RESET_MS    = 300000;  // forget errors after 5 min
static const int      MAX_CONNECT_ERRORS = 5;      // reboot past this

// ARP is asynchronous: etharp_request() only queues the request, so the first
// etharp_find_addr() after it almost always misses. Poll on a timer rather than
// blocking with delay() the way v1 did.
static const uint32_t ARP_RETRY_FAST_MS = 1000;
static const uint32_t ARP_RETRY_SLOW_MS = 30000;
static const uint8_t  ARP_FAST_ATTEMPTS = 15;

// WiFi supervision. Nothing else re-establishes the STA link: WiFiManager's ESP32
// disconnect handler only reconnects under #ifdef esp32autoreconnect (not defined),
// and startConfigPortal() actively turns the station interface OFF. So we drive it.
static const uint32_t WIFI_RETRY_MS     = 20000;   // between reconnect attempts
static const uint32_t WIFI_GRACE_MS     = 120000;  // offline this long -> raise the AP
static const uint32_t AP_PORTAL_MAX_MS  = 180000;  // ...then drop it and retry the STA

// Boot connect: classic blocking begin-and-wait, retried, BEFORE WiFiManager runs.
// wm.autoConnect() makes exactly one attempt with no timeout and no retries
// (_connectTimeout=0, _connectRetries=1): a transient radio/netif bring-up failure
// fails in ~0 ms - silently, the core only log_e()s it - and drops the device
// straight into the AP portal without ever having tried the router.
static const uint8_t  WIFI_BOOT_ATTEMPTS   = 2;      // 2 x 10 s = max 20 s before letting go
static const uint32_t WIFI_BOOT_ATTEMPT_MS = 10000;  // per-attempt connect window

char IP_char[IP_LEN]   = "";
char MAC_char[MAC_LEN] = "";

struct PowerData {
  int pvProduction;
  int gridConsumption;
  int homeConsumption;
};

struct GridVoltage {
  float L1;
  float L2;
  float L3;
  int   devPercent;
};

int  connectErrors        = 0;
bool wm_nonblocking       = true;
bool inverterIPSet        = false;
bool inverterMACSet       = false;
bool inverterAsSmartMeter = true;
bool initSuccess          = false;
bool otaReady             = false;
uint32_t pollTimeout      = 0;
uint32_t errorTimeout     = 0;

bool     wifiOnline       = false;  // last observed link state, for edge detection
bool     haveSavedWiFi    = false;  // are there credentials to fall back on?
uint32_t offlineSince     = 0;      // millis() of the drop, 0 while online
uint32_t lastWifiRetry    = 0;
uint32_t apPortalStarted  = 0;      // millis() the soft-AP portal was raised

WiFiManager wm;
WiFiManagerParameter inverter_input;
WiFiManagerParameter inverter_input_2;
WiFiManagerParameter inverter_text;
Preferences preferences;
NetScanner scanner;

// LCD Wiring: RS, EN, D4, D5, D6, D7
// Pinout for Waveshare ESP32-S3-ETH.
// D6/D7 are on 47/48, NOT 19/20: those are the ESP32-S3 native USB pins
// (USB_D-/USB_D+). Driving them from the LCD stops the board enumerating over USB.
LiquidCrystal lcd(15, 16, 17, 18, 47, 48);

/* ---- Custom LCD glyphs ---- */
// The HD44780 has 8 CGRAM slots (0-7). Slots 0-3 hold the house and are set once
// in setup(); slots 4-7 hold the power pole.
byte bottomLeft[8]  = {0b10000, 0b10000, 0b10000, 0b10110, 0b10110, 0b10110, 0b10110, 0b11111};
byte bottomRight[8] = {0b00001, 0b00001, 0b00001, 0b01101, 0b01101, 0b00001, 0b00001, 0b11111};
byte topLeft[8]     = {0b00000, 0b01101, 0b01110, 0b01100, 0b01000, 0b10000, 0b10000, 0b10000};
byte topRight[8]    = {0b00000, 0b10000, 0b01000, 0b00100, 0b00010, 0b00001, 0b01101, 0b01101};

byte PoleTLeft[8]   = {0b00000, 0b00000, 0b00000, 0b00000, 0b00001, 0b00110, 0b01011, 0b10010};
byte PoleTRight[8]  = {0b00000, 0b00000, 0b00000, 0b00000, 0b10000, 0b01100, 0b11010, 0b01001};
byte PoleMLeft[8]   = {0b11111, 0b00001, 0b00001, 0b00001, 0b00010, 0b00010, 0b00010, 0b00100};
byte PoleMRight[8]  = {0b11111, 0b10000, 0b10000, 0b10000, 0b01000, 0b01000, 0b01000, 0b00100};

// Forward declarations. The Arduino IDE generates these automatically for .ino
// sketches; PlatformIO compiles this as a plain .cpp, so they must be explicit.
void checkButton();
void saveParamCallback();
void resolveMAC();
bool portalActive();
void openPortal();
void openAPPortal();
void manageWiFi();
void tuneRadio();
void setupOTA();
void runInverterHandshake();
void drawPowerScreen(const PowerData &prm, const GridVoltage &gv);
bool isValidIP(const char *s);
bool buildURL(char *out, size_t outLen, const char *endpoint);
eth_addr *get_sta_mac(const ip4_addr_t &ip);
String httpGETRequest(const char *serverPath);
PowerData getInverterData();
GridVoltage getGridVoltage();
void lcdMessage(const char *l0, const char *l1 = nullptr,
                const char *l2 = nullptr, const char *l3 = nullptr);

void setup() {
#ifdef DEBUG
  Serial.begin(115200);
#endif

  delay(2000);
  lcd.begin(20, 4);
  delay(100);
  lcd.clear();
  delay(100);

  lcd.createChar(0, bottomLeft);
  lcd.createChar(1, bottomRight);
  lcd.createChar(2, topLeft);
  lcd.createChar(3, topRight);
  delay(500);

  lcd.setCursor(0, 0);
  lcd.print(DEVICE_NAME);
  lcd.setCursor(0, 1);
  lcd.print(F("VERSION: "));  lcd.print(DEVICE_VERSION);
  lcd.setCursor(0, 2);
  lcd.print(F("SOFTWARE: ")); lcd.print(SOFTWARE_VERSION);
  lcd.setCursor(0, 3);
  lcd.print(DEVICE_ID);

  WiFi.mode(WIFI_STA);
  delay(100);
  tuneRadio();

  // The SSID is served by FOUR routers in and around the house, so always take the
  // loudest. Two places must agree on that:
  //
  // 1) These setters - but they only apply to connects that pass an explicit SSID
  //    (the config portal's save path). The no-arg WiFi.begin() used everywhere else
  //    (boot loop, manageWiFi retries) reuses the wifi_config_t stored in NVS verbatim.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // 2) The stored config itself, which may still carry the core default from whenever
  //    the credentials were last saved: WIFI_FAST_SCAN = first BSSID that answers wins,
  //    i.e. a 1-in-4 lottery. Rewrite it in place; esp_wifi_set_config persists to NVS,
  //    so every later no-arg begin() inherits all-channel + by-signal.
  {
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
      conf.sta.scan_method     = WIFI_ALL_CHANNEL_SCAN;
      conf.sta.sort_method     = WIFI_CONNECT_AP_BY_SIGNAL;
      conf.sta.threshold.rssi  = -127;  // never skip a candidate for being weak
      esp_wifi_set_config(WIFI_IF_STA, &conf);
    }
  }

  // Ask exactly once, here, while the station interface is definitely up: getWiFiIsSaved()
  // ends in esp_wifi_get_config(WIFI_IF_STA, &conf) with the return value ignored, so with
  // the station disabled - which is precisely the state the AP portal leaves us in - it can
  // hand back uninitialized stack garbage. manageWiFi() reads this latch instead of asking.
  haveSavedWiFi = wm.getWiFiIsSaved();
  DEBUG_PRINTF("WIFI: saved credentials: %s\n", haveSavedWiFi ? "yes" : "no");

  // Load persisted config
  preferences.begin("inverter_config", false);
  strlcpy(IP_char,  preferences.getString("inverter_IP",  "").c_str(), sizeof(IP_char));
  strlcpy(MAC_char, preferences.getString("inverter_MAC", "").c_str(), sizeof(MAC_char));
  inverterIPSet  = preferences.getBool("inverterIPSet",  false) && isValidIP(IP_char);
  inverterMACSet = preferences.getBool("inverterMACSet", false) && MAC_char[0] != '\0';

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  if (wm_nonblocking) wm.setConfigPortalBlocking(false);

  new (&inverter_input) WiFiManagerParameter(
      "InverterInputID", "Inverter IPv4", IP_char, IP_LEN - 1,
      "placeholder=\"xxx.xxx.xxx.xxx\"");
  new (&inverter_input_2) WiFiManagerParameter(
      "InverterInputID2", "Inverter MAC", MAC_char, MAC_LEN - 1,
      "placeholder=\"xx:xx:xx:xx:xx:xx\" disabled");
  new (&inverter_text) WiFiManagerParameter(
      "<p>The MAC address will be determined automatically after you connect to your local LAN.</p>");

  wm.addParameter(&inverter_input);
  wm.addParameter(&inverter_input_2);
  wm.addParameter(&inverter_text);

  // Only the params callback is wired: setSaveConfigCallback() fires when WiFi
  // credentials are saved, which says nothing about whether an inverter IP was entered.
  wm.setSaveParamsCallback(saveParamCallback);

  std::vector<const char *> menu = {"wifi", "info", "param", "sep", "restart", "exit"};
  wm.setMenu(menu);
  wm.setClass("invert");
  // Portal lifetime is ours, not WiFiManager's: its timeout would shut the AP down
  // and leave the device with neither a portal nor a station link. manageWiFi()
  // cycles AP <-> STA instead, so the device always finds the router again.
  wm.setConfigPortalTimeout(0);
  wm.setCaptivePortalEnable(false);
  wm.setAPClientCheck(true);

  // Classic blocking connect, the old way: begin(), wait, retry - and only after
  // every attempt has failed is WiFiManager allowed to raise the AP portal. Nothing
  // else runs until this window has passed. Re-issuing begin() also retries the
  // whole STA bring-up, which is what fails on the bad boots.
  if (haveSavedWiFi) {
    for (uint8_t attempt = 1;
         attempt <= WIFI_BOOT_ATTEMPTS && WiFi.status() != WL_CONNECTED; attempt++) {
      DEBUG_PRINTF("WIFI: boot connect, attempt %u/%u\n", attempt, WIFI_BOOT_ATTEMPTS);
      char line[21];
      snprintf(line, sizeof(line), "attempt %u of %u", attempt, WIFI_BOOT_ATTEMPTS);
      lcdMessage("WIFI: connecting...", line);

      WiFi.begin();  // no args: the credentials saved in NVS
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_BOOT_ATTEMPT_MS) {
        delay(250);
      }
    }
    DEBUG_PRINTF("WIFI: boot connect %s\n",
                 WiFi.status() == WL_CONNECTED ? "succeeded" : "failed - falling back to portal");
  }

  // Already connected by the loop above -> autoConnect() sees WL_CONNECTED and
  // returns true immediately. Only if all attempts failed does it get one try of
  // its own and then raise the AP portal, same as before.
  if (wm.autoConnect(WIFI_APN)) {
    DEBUG_PRINTLN(F("WIFI: connected"));
    lcdMessage("WIFI: connected...");
    wifiOnline = true;
    setupOTA();
    otaReady = true;
    delay(1000);
  } else {
    // autoConnect() has already brought up the soft-AP portal itself. Don't mirror
    // that in a local flag: WiFiManager closes this portal on its own once WiFi is
    // saved (_disableConfigPortal), and would leave the mirror stuck true forever.
    // Do record when it went up - manageWiFi() takes it down again to retry the STA.
    DEBUG_PRINTLN(F("WIFI: not connected - AP config portal open"));
    lcdMessage("WIFI: failed to", "connect, join AP:", WIFI_APN, "192.168.4.1");
    wifiOnline      = false;
    offlineSince    = millis();
    apPortalStarted = millis();
  }

  DEBUG_PRINTF("Saved inverter IP:  %s\n", IP_char[0]  ? IP_char  : "(none)");
  DEBUG_PRINTF("Saved inverter MAC: %s\n", MAC_char[0] ? MAC_char : "(none)");
}

void loop() {
  if (wm_nonblocking) wm.process();

  // Owns every WiFi state transition: reconnects a dropped link, raises the AP portal
  // when the outage is long, and tears that portal back down so the STA can retry.
  manageWiFi();

  if (otaReady) ArduinoOTA.handle();

  checkButton();

  // Keep the LAN web portal up for as long as we have WiFi: it is the only way to
  // change the inverter IP later, and it costs nothing but the web server.
  // Guarded by portalActive(): v1 called startConfigPortal() on every loop
  // iteration, restarting the portal continuously.
  if (WiFi.status() == WL_CONNECTED && !portalActive()) {
    openPortal();
  }

  // Everything below needs the network. Without this guard a WiFi outage looked
  // exactly like a dead inverter: the poll kept firing, every GET failed, and six
  // failures rebooted the device straight into the AP portal.
  if (WiFi.status() != WL_CONNECTED) return;

  if (inverterIPSet && !inverterMACSet) {
    resolveMAC();
    return;
  }

  // One-time handshake: confirm the box at IP_char is still our inverter, and
  // that it speaks Solar API v1.
  if (inverterIPSet && inverterMACSet && !initSuccess) {
    runInverterHandshake();
    return;
  }

  // Reboot if the inverter has been unreachable too many times in a row.
  if (connectErrors > MAX_CONNECT_ERRORS) {
    DEBUG_PRINTLN(F("Too many connection errors, rebooting..."));
    lcdMessage("Too many errors,", "rebooting...");
    delay(2000);
    ESP.restart();
  }
  if (errorTimeout != 0 && (millis() - errorTimeout) > ERROR_RESET_MS) {
    connectErrors = 0;
    errorTimeout  = 0;
  }

  // Main screen
  if (initSuccess && (millis() - pollTimeout) > POLL_INTERVAL_MS) {
    pollTimeout = millis();
    PowerData   prm = getInverterData();
    GridVoltage gv  = getGridVoltage();
    drawPowerScreen(prm, gv);
  }
}

/* ---- Display ---- */

void lcdMessage(const char *l0, const char *l1, const char *l2, const char *l3) {
  lcd.clear();
  const char *lines[4] = {l0, l1, l2, l3};
  for (int i = 0; i < 4; i++) {
    if (lines[i] == nullptr) continue;
    lcd.setCursor(0, i);
    lcd.print(lines[i]);
  }
}

// Formats watts as "1234W" or "12.3kW" into buf.
static void formatPower(int watts, char *buf, size_t len) {
  int w = abs(watts);
  if (w < 1000) {
    snprintf(buf, len, "%dW", w);
  } else {
    snprintf(buf, len, "%.1fkW", w / 1000.0f);
  }
}

void drawPowerScreen(const PowerData &prm, const GridVoltage &gv) {
  lcd.clear();

  // PV production (row 0)
  char pv[12];
  formatPower(prm.pvProduction, pv, sizeof(pv));
  lcd.setCursor(2, 0);
  lcd.write(0b10110010);
  lcd.write(0b10110010);
  lcd.print(" ");
  lcd.print(pv);

  // House (rows 1-2, cols 0-1)
  lcd.setCursor(0, 2); lcd.write(byte(0));
  lcd.setCursor(1, 2); lcd.write(byte(1));
  lcd.setCursor(0, 1); lcd.write(byte(2));
  lcd.setCursor(1, 1); lcd.write(byte(3));

  // Pole (rows 0-2, cols 18-19). CGRAM slots 4-7 are only needed here.
  lcd.createChar(4, PoleTLeft);
  lcd.createChar(5, PoleTRight);
  lcd.createChar(6, PoleMLeft);
  lcd.createChar(7, PoleMRight);

  lcd.setCursor(18, 0); lcd.write(byte(4));
  lcd.setCursor(18, 1); lcd.write(byte(6));
  lcd.setCursor(18, 2); lcd.print(F("|"));
  lcd.setCursor(19, 0); lcd.write(byte(5));
  lcd.setCursor(19, 1); lcd.write(byte(7));
  lcd.setCursor(19, 2); lcd.print(F("|"));

  // Grid flow (row 2). v1 built this with strcat() into char[7]/char[12] buffers
  // and overran both - the export branch wrote ~15 bytes into a 7-byte stack
  // buffer on every refresh. snprintf into one correctly-sized buffer instead.
  char power[12];
  formatPower(prm.gridConsumption, power, sizeof(power));

  char gridLine[20];
  if (prm.gridConsumption > 0) {
    // Importing: grid -> house
    snprintf(gridLine, sizeof(gridLine), "<  <  < %s", power);
  } else {
    // Exporting: house -> grid
    snprintf(gridLine, sizeof(gridLine), "%s >  >  >", power);
  }
  lcd.setCursor(3, 2);
  lcd.print(gridLine);

  // Status (row 3)
  char status[20];
  snprintf(status, sizeof(status), "%d%%gV %dE %ddRSSI",
           gv.devPercent, connectErrors, (int)WiFi.RSSI());
  lcd.setCursor(2, 3);
  lcd.print(status);
}

/* ---- Fronius Solar API ---- */

// Builds "http://<ip><endpoint>". Returns false if it would not fit.
bool buildURL(char *out, size_t outLen, const char *endpoint) {
  int n = snprintf(out, outLen, "%s%s%s", HTTPSTRING, IP_char, endpoint);
  if (n < 0 || (size_t)n >= outLen) {
    DEBUG_PRINTF("buildURL: URL too long for %u-byte buffer\n", (unsigned)outLen);
    return false;
  }
  return true;
}

String httpGETRequest(const char *serverPath) {
  HTTPClient http;
  DEBUG_PRINTF("Requesting URL: %s\n", serverPath);

  http.begin(serverPath);
  int httpResponseCode = http.GET();
  String payload = "{}";

  if (httpResponseCode > 0) {
    DEBUG_PRINTF("HTTP Response code: %d\n", httpResponseCode);
    payload = http.getString();
  } else {
    DEBUG_PRINTF("HTTP error code: %d\n", httpResponseCode);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("HTTP Error: "));
    lcd.print(httpResponseCode);
    lcd.setCursor(0, 1);
    lcd.print(F("Inverter unavailable"));
  }

  http.end();
  return payload;
}

// Flags a failed poll and starts the 5-minute forget-errors window.
static void noteConnectError() {
  connectErrors++;
  errorTimeout = millis();
}

PowerData getInverterData() {
  PowerData prm = {0, 0, 0};

  char url[URL_LEN];
  if (!buildURL(url, sizeof(url), INVERTERDATA)) return prm;

  String payload = httpGETRequest(url);
  if (payload == "{}") {
    DEBUG_PRINTLN(F("Inverter data not available"));
    noteConnectError();
    return prm;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    DEBUG_PRINTF("deserializeJson() failed: %s\n", error.f_str());
    noteConnectError();
    return prm;
  }
  if (doc["Body"]["Data"]["Inverters"]["1"]["P"].isNull()) {
    DEBUG_PRINTLN(F("Inverter data not available"));
    noteConnectError();
    return prm;
  }

  float pvProduction = 0;
  for (int i = 1; i <= 9; i++) {  // up to 10 inverters
    pvProduction += doc["Body"]["Data"]["Inverters"][String(i)]["P"].as<float>();
  }

  if (inverterAsSmartMeter) {
    for (int i = 1; i <= 9; i++) {  // up to 10 secondary meters
      const char *category =
          doc["Body"]["Data"]["SecondaryMeters"][String(i)]["Category"].as<const char *>();
      if (category != nullptr && (strcmp(category, "METER_CAT_WR") == 0 ||
                                  strcmp(category, "METER_CAT_BAT") == 0 ||
                                  strcmp(category, "METER_CAT_PV_BAT") == 0)) {
        pvProduction +=
            doc["Body"]["Data"]["SecondaryMeters"][String(i)]["P"].as<float>();
      }
    }
  }

  float gridConsumption = doc["Body"]["Data"]["Site"]["P_Grid"].as<float>();

  prm.pvProduction    = round(pvProduction);
  prm.gridConsumption = round(gridConsumption);
  prm.homeConsumption = round(pvProduction + gridConsumption);

  connectErrors = 0;  // a good poll clears the streak
  return prm;
}

GridVoltage getGridVoltage() {
  GridVoltage gV = {0, 0, 0, 0};

  char url[URL_LEN];
  if (!buildURL(url, sizeof(url), INVERTERVOLTAGE)) return gV;

  String payload = httpGETRequest(url);
  if (payload == "{}") {
    DEBUG_PRINTLN(F("Meter data not available"));
    noteConnectError();
    return gV;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    DEBUG_PRINTF("deserializeJson() failed: %s\n", error.f_str());
    noteConnectError();
    return gV;
  }
  // Body.Data is keyed by meter device ID, and that ID is NOT necessarily "0":
  // the IDs come from the Datamanager, and a system with two Smart Meters can report
  // them as "2" and "3" with no "0" at all. Select by Meter_Location_Current instead
  // (Solar API v1 spec 4.8.5): 0 = grid interconnection point (primary meter),
  // 1 = load (primary), 3 = external generator, 256-511 = subloads (both secondary).
  // We want the grid meter; only fall back to a load-path primary, never a secondary -
  // the Carport generator meter would report its own voltages, not the grid's.
  JsonObject meters = doc["Body"]["Data"].as<JsonObject>();
  JsonObject meter;
  for (JsonPair entry : meters) {
    JsonObject m = entry.value().as<JsonObject>();
    if (m["Voltage_AC_Phase_1"].isNull()) continue;

    int location = m["Meter_Location_Current"].as<int>();
    if (location == 0) {  // grid interconnection point - what we're after
      meter = m;
      break;
    }
    if (location == 1 && meter.isNull()) meter = m;  // load-path primary, second choice
  }
  if (meter.isNull()) {
    DEBUG_PRINTLN(F("Meter data not available"));
    noteConnectError();
    return gV;
  }

  gV.L1 = meter["Voltage_AC_Phase_1"].as<float>();
  gV.L2 = meter["Voltage_AC_Phase_2"].as<float>();
  gV.L3 = meter["Voltage_AC_Phase_3"].as<float>();

  // Report whichever phase deviates furthest from nominal 230 V.
  float UACmax = max(gV.L1, max(gV.L2, gV.L3));
  float UACmin = min(gV.L1, min(gV.L2, gV.L3));
  int devMinPerc = round(UACmin / 230.0f * 100);
  int devMaxPerc = round(UACmax / 230.0f * 100);
  gV.devPercent = (abs(100 - devMinPerc) > abs(100 - devMaxPerc)) ? devMinPerc : devMaxPerc;

  return gV;
}

/* ---- Inverter discovery / handshake ---- */

// Confirms the box at IP_char is still the inverter we recorded, and that it
// speaks Solar API v1. If its DHCP lease moved, re-find it by MAC.
void runInverterHandshake() {
  lcdMessage("Scanning network...", "Please wait, this", "might take a bit...");

  scanner.begin();
  const char *eth_ret = scanner.findIP(IP_char);

  if (eth_ret == nullptr) {
    // IP no longer answers - the inverter probably got a new DHCP lease.
    scanner.end();
    scanner.begin();
    DEBUG_PRINTLN(F("IP not found in ARP table"));
    lcdMessage("Inverter IP changed", "searching by MAC.", "please be patient...");

    char *IPResult = scanner.findIPbyMAC(MAC_char);
    if (IPResult != nullptr && isValidIP(IPResult)) {
      DEBUG_PRINTF("Resolved new IP: %s\n", IPResult);
      strlcpy(IP_char, IPResult, sizeof(IP_char));
      preferences.putString("inverter_IP", IP_char);
      lcdMessage("Inverter IP changed", "New IP found:", IP_char, "rebooting...");
    } else {
      DEBUG_PRINTLN(F("Could not find inverter by MAC"));
      // v1 passed a literal 0 to putString() here - that is a const char* nullptr.
      // Clear the flag instead so the portal reopens after the reboot.
      preferences.putBool("inverterIPSet", false);
      lcdMessage("Inverter not found", "on the network.", "rebooting...");
    }
    scanner.end();
    delay(2000);
    ESP.restart();
  }

  DEBUG_PRINTLN(F("IP found in ARP table"));
  DEBUG_PRINTF("Comparing current MAC %s with stored MAC %s\n", eth_ret, MAC_char);
  bool macMatches = (strcmp(MAC_char, eth_ret) == 0);
  scanner.end();
  delay(500);  // give the destructor time to clean up

  if (!macMatches) {
    // Something else now holds that IP.
    DEBUG_PRINTLN(F("MAC address does not match"));
    preferences.putBool("inverterIPSet", false);
    lcdMessage("MAC address does", "not match,", "rebooting...");
    delay(2000);
    ESP.restart();
  }

  char url[URL_LEN];
  if (!buildURL(url, sizeof(url), APIDATA)) return;

  delay(500);
  String payload = httpGETRequest(url);
  delay(500);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    DEBUG_PRINTF("deserializeJson() failed: %s\n", error.f_str());
    noteConnectError();
    return;  // retried next loop
  }

  int APIVersion = doc["APIVersion"].as<int>();
  DEBUG_PRINTF("API Version: %d\n", APIVersion);

  if (APIVersion != 1) {
    DEBUG_PRINTLN(F("API Version not supported"));
    lcdMessage("API Version not", "supported,", "rebooting...");
    delay(2000);
    ESP.restart();
  }

  lcdMessage("API Version: 1", "...loading data....");
  initSuccess = true;
  pollTimeout = millis() - POLL_INTERVAL_MS;  // poll immediately
}

/* ---- OTA ---- */

// Push new firmware with:
//   pio run -t upload --upload-port solar-display.local
// (or use the device IP). The LCD shows progress; the display loop is paused for
// the duration, because writing flash while polling the inverter is asking for a
// half-written image.
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (OTA_PASSWORD[0] != '\0') ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    // U_SPIFFS would mean a filesystem image; we only ever push U_FLASH.
    const char *what = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    DEBUG_PRINTF("OTA: start updating %s\n", what);
    initSuccess = false;  // stop the poll loop from touching the LCD mid-flash
    lcdMessage("OTA UPDATE", "Starting...", "Do not power off!");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lastPct = -1;
    int pct = (total > 0) ? (progress * 100) / total : 0;
    if (pct == lastPct) return;  // the LCD is slow; only redraw on change
    lastPct = pct;

    char line[21];
    snprintf(line, sizeof(line), "Progress: %3d%%", pct);
    lcd.setCursor(0, 1);
    lcd.print("                    ");
    lcd.setCursor(0, 1);
    lcd.print(line);

    // 20-cell progress bar on the bottom row
    int filled = (pct * 20) / 100;
    lcd.setCursor(0, 3);
    for (int i = 0; i < 20; i++) lcd.write(i < filled ? byte(0xFF) : ' ');

    DEBUG_PRINTF("OTA: %d%%\r", pct);
  });

  ArduinoOTA.onEnd([]() {
    DEBUG_PRINTLN(F("\nOTA: done, rebooting"));
    lcdMessage("OTA UPDATE", "Complete!", "Rebooting...");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    const char *msg = "Unknown error";
    switch (error) {
      case OTA_AUTH_ERROR:    msg = "Auth failed";    break;
      case OTA_BEGIN_ERROR:   msg = "Begin failed";   break;
      case OTA_CONNECT_ERROR: msg = "Connect failed"; break;
      case OTA_RECEIVE_ERROR: msg = "Receive failed"; break;
      case OTA_END_ERROR:     msg = "End failed";     break;
    }
    DEBUG_PRINTF("OTA error [%u]: %s\n", error, msg);
    lcdMessage("OTA UPDATE FAILED", msg, "Rebooting...");
    delay(3000);
    ESP.restart();
  });

  ArduinoOTA.begin();
  DEBUG_PRINTF("OTA ready at %s.local (%s)\n", OTA_HOSTNAME,
               WiFi.localIP().toString().c_str());
}

/* ---- Radio tuning ---- */

// Everything here is aimed at one problem: the AP is only -80..-90 dBm away, which is
// the edge of the ESP32's usable range. None of it is free performance - it is all
// trading things we do not need (power, throughput, 40 MHz channels) for link margin,
// which is the only thing we are short of.
//
// Re-applied on every reconnect, not just at boot: a WIFI_STA -> WIFI_AP mode change,
// which is exactly what a trip through the config portal does, resets these to the IDF
// defaults. Setting them once in setup() would silently lose them the first time the
// display fell back to the AP.
void tuneRadio() {
  // The big one. The default power-save mode (WIFI_PS_MIN_MODEM) parks the radio
  // between the AP's beacons. At -85 dBm a missed beacon is not a rare event, and a
  // run of them is a dropped association - which is very likely what has been knocking
  // this display off the air. The display is mains-powered; there is nothing to save.
  WiFi.setSleep(false);

  // Full transmit power.
  //
  // Be clear about what this does and does not buy: it improves the UPLINK, i.e. how
  // well the Zyxel hears the display. It cannot improve the RSSI on the LCD, which is
  // how well the display hears the Zyxel - no transmitter can talk itself louder into
  // its own receiver. Expect the number on screen to stay where it is; what should
  // improve is the AP no longer losing us mid-conversation.
  //
  // Must come after esp_wifi_start() - i.e. after WiFi.mode() - or it is silently
  // dropped. 19.5 dBm is the Arduino enum's ceiling and the module's practical max.
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Force 20 MHz. A 40 MHz channel spreads the same transmit power across twice the
  // bandwidth and costs roughly 3 dB of receive sensitivity - a straight trade of range
  // for throughput, and we have no use for throughput. The payload is a few kB every
  // five seconds.
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

  // Keep 802.11b in the protocol set. Dropping it as "legacy" is the obvious-looking
  // move and it is wrong here: the b rates go down to 1 Mbps and have far and away the
  // best receiver sensitivity in the set. They are the rates that still carry a frame
  // at -90 dBm. This is a range problem, so we want every slow robust rate available.
  esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  int8_t txq = 0;
  esp_wifi_get_max_tx_power(&txq);  // reported in 0.25 dBm units
  DEBUG_PRINTF("WIFI: radio tuned - sleep off, HT20, B/G/N, TX %.2f dBm\n", txq / 4.0f);
}

/* ---- WiFi / config portal ---- */

// Two different portals, and picking the wrong one is why the device was unreachable
// on its LAN address:
//
//   startConfigPortal() - brings up a soft-AP (SSID SOLAR_POWER_DISPLAY) and serves on
//                         192.168.4.1. Needed when we have no WiFi. You must join that
//                         AP to see it; it is NOT reachable from the LAN.
//   startWebPortal()    - serves the same menu on the existing STA connection, i.e. at
//                         http://<device-lan-ip>/. Only works when already connected.
//
// So: connected -> web portal on the LAN. Not connected -> AP.
//
// Portal state is read back from WiFiManager rather than mirrored in our own bool.
// WiFiManager tears the AP portal down by itself the moment WiFi credentials are
// saved (_disableConfigPortal defaults true), without any callback - a local
// "portalRunning" flag set at boot would stay true forever and wedge the state machine.
bool portalActive() {
  return wm.getConfigPortalActive() || wm.getWebPortalActive();
}

void openPortal() {
  if (portalActive()) return;

  if (WiFi.status() == WL_CONNECTED) {
    wm.startWebPortal();
    DEBUG_PRINTF("Web portal: http://%s/\n", WiFi.localIP().toString().c_str());
    // Only claim the LCD while the inverter IP is still missing - once it is set
    // the portal is just sitting there for later edits, and the power screen owns
    // the display.
    if (!inverterIPSet) {
      lcdMessage("Set inverter IP at", WiFi.localIP().toString().c_str());
    }
  } else {
    openAPPortal();
  }
}

// Raises the soft-AP portal (SSID SOLAR_POWER_DISPLAY, 192.168.4.1).
//
// This is a one-way door as far as WiFi is concerned: startConfigPortal() calls
// WiFi_Disconnect() + WiFi_enableSTA(false) whenever the station is not connected,
// so while the AP is up the device cannot see - let alone rejoin - the router. That
// is exactly how the display got stranded: a router outage bounced it in here and
// nothing ever brought the station back. manageWiFi() therefore times this portal
// out and retries the saved credentials.
void openAPPortal() {
  if (wm.getConfigPortalActive()) return;

  // startConfigPortal() only guards against configPortalActive, not webPortalActive:
  // starting it on top of a live web portal would re-create the HTTP server under the
  // old one. Take the web portal down first.
  if (wm.getWebPortalActive()) wm.stopWebPortal();

  DEBUG_PRINTLN(F("WIFI: starting AP config portal"));
  lcdMessage("WIFI: Starting", "config portal:", WIFI_APN, "192.168.4.1");
  apPortalStarted = millis();
  wm.startConfigPortal(WIFI_APN);
}

// The whole WiFi lifecycle, edge-triggered. Called once per loop().
void manageWiFi() {
  const bool online = (WiFi.status() == WL_CONNECTED);

  if (online) {
    if (!wifiOnline) {  // rising edge: we just (re)joined the router
      wifiOnline    = true;
      haveSavedWiFi = true;  // we are associated, so credentials exist by definition
      offlineSince  = 0;

      // The AP portal is meaningless now, and it is what was covering the LCD.
      // loop() reopens the LAN web portal on the next iteration.
      if (wm.getConfigPortalActive()) wm.stopConfigPortal();

      // A mode change resets power-save, bandwidth and TX power to the IDF defaults,
      // and getting here from the AP portal means we just did one.
      tuneRadio();

      connectErrors = 0;  // the outage was ours, not the inverter's
      errorTimeout  = 0;

      DEBUG_PRINTF("WIFI: connected, IP %s\n", WiFi.localIP().toString().c_str());
      lcdMessage("WIFI: connected", WiFi.localIP().toString().c_str());

      if (!otaReady) {  // WiFi may only arrive here, via the portal
        setupOTA();
        otaReady = true;
      }
      pollTimeout = millis() - POLL_INTERVAL_MS;  // repaint the power screen at once
    }
    return;
  }

  /* ---- offline ---- */

  if (wifiOnline) {  // falling edge
    wifiOnline    = false;
    offlineSince  = millis();
    lastWifiRetry = 0;
    DEBUG_PRINTLN(F("WIFI: connection lost"));
    lcdMessage("WIFI: connection", "lost, retrying...");
  }
  if (offlineSince == 0) offlineSince = millis();

  if (wm.getConfigPortalActive()) {
    // Someone is on the AP configuring - keep it up, and keep pushing the deadline.
    if (WiFi.softAPgetStationNum() > 0) {
      apPortalStarted = millis();
      return;
    }
    // Nothing saved to fall back to (fresh device): the portal is the only way in.
    if (!haveSavedWiFi) return;

    if ((millis() - apPortalStarted) < AP_PORTAL_MAX_MS) return;

    // Nobody came. Drop the AP so the station can be switched back on and look for
    // the router again; if it is still gone, the grace timer raises the AP anew.
    DEBUG_PRINTLN(F("WIFI: AP portal idle, retrying saved credentials"));
    lcdMessage("WIFI: retrying", "saved network...");
    wm.stopConfigPortal();
    offlineSince  = millis();
    lastWifiRetry = 0;
    return;
  }

  // No portal in the way: bang on the saved credentials.
  if (lastWifiRetry == 0 || (millis() - lastWifiRetry) > WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    DEBUG_PRINTLN(F("WIFI: reconnecting..."));
    WiFi.mode(WIFI_STA);  // may have been turned off by a previous AP portal
    WiFi.begin();         // no args: reuse the credentials in NVS
  }

  // Long outage, and the router is not coming back on its own terms. Offer the AP so
  // the user can point the display at a different network - without giving up on the
  // saved one, which the AP timeout above keeps retrying.
  if ((millis() - offlineSince) > WIFI_GRACE_MS) {
    openAPPortal();
  }
}

void checkButton() {
  if (digitalRead(TRIGGER_PIN) != LOW) return;

  delay(50);  // debounce
  if (digitalRead(TRIGGER_PIN) != LOW) return;

  DEBUG_PRINTLN(F("WIFI: Button Pressed"));
  lcdMessage("WiFi button pressed,", "hold for config...");

  delay(3000);  // hold-to-erase window
  if (digitalRead(TRIGGER_PIN) == LOW) {
    lcdMessage("WiFi button held", "Erasing Config...");
    DEBUG_PRINTLN(F("WIFI: Button Held - erasing config, restarting"));
    wm.resetSettings();
    preferences.clear();
    delay(1000);
    ESP.restart();
  }

  DEBUG_PRINTLN(F("WIFI: Starting config portal"));
  openPortal();
}

// Rejects "", "0", and anything else that is not dotted-quad IPv4.
bool isValidIP(const char *s) {
  if (s == nullptr || s[0] == '\0') return false;
  ip4_addr_t addr;
  return ip4addr_aton(s, &addr) != 0 && addr.addr != IPADDR_NONE && addr.addr != IPADDR_ANY;
}

void saveParamCallback() {
  DEBUG_PRINTLN(F("[CALLBACK] saveParamCallback fired"));

  char entered[IP_LEN];
  strlcpy(entered, inverter_input.getValue(), sizeof(entered));

  if (!isValidIP(entered)) {
    DEBUG_PRINTF("Rejected invalid inverter IP: '%s'\n", entered);
    return;  // leave inverterIPSet false; loop() keeps the portal open
  }

  strlcpy(IP_char, entered, sizeof(IP_char));
  preferences.putString("inverter_IP", IP_char);
  inverterIPSet = true;
  preferences.putBool("inverterIPSet", inverterIPSet);
  DEBUG_PRINTF("Saved inverter IP: %s\n", IP_char);

  // A new IP invalidates any previously resolved MAC.
  MAC_char[0] = '\0';
  preferences.putString("inverter_MAC", "");
  inverterMACSet = false;
  preferences.putBool("inverterMACSet", false);
  initSuccess = false;

  // Leave the portal up. It used to be torn down here, which - on the soft-AP
  // portal - killed it before WiFi credentials had been saved, stranding the
  // device. loop() reopens the web portal anyway, so closing it achieved nothing.
  // Don't resolve the MAC here either: ARP is async and would need to block.
  // loop() polls it.
}

/* ---- ARP / MAC resolution ---- */

// Returns nullptr when the ARP entry is not (yet) in the cache.
//
// etharp_* are raw lwIP core APIs. lwIP runs in its own TCPIP task, and ESP-IDF 5
// asserts ("Required to lock TCPIP core functionality!") if they are called from
// another thread - which the Arduino loop task is - without holding the core lock.
// v1 called them unlocked and got away with it on IDF 4, where the check was absent.
eth_addr *get_sta_mac(const ip4_addr_t &ip) {
  ip4_addr_t requestIP = ip;
  eth_addr *ret_eth_addr = nullptr;
  ip4_addr const *ret_ip_addr = nullptr;

  if (netif_default == nullptr) return nullptr;

  LOCK_TCPIP_CORE();
  etharp_request(netif_default, &requestIP);
  s8_t found = etharp_find_addr(netif_default, &requestIP, &ret_eth_addr, &ret_ip_addr);
  UNLOCK_TCPIP_CORE();

  return (found < 0) ? nullptr : ret_eth_addr;
}

void resolveMAC() {
  static uint32_t lastAttempt = 0;
  static uint8_t  attempts    = 0;

  const uint32_t interval =
      (attempts < ARP_FAST_ATTEMPTS) ? ARP_RETRY_FAST_MS : ARP_RETRY_SLOW_MS;

  if (lastAttempt != 0 && (millis() - lastAttempt) < interval) return;
  lastAttempt = millis();

  lcdMessage("Resolving MAC for:", IP_char);

  ip4_addr_t target;
  if (!ip4addr_aton(IP_char, &target)) {
    DEBUG_PRINTF("resolveMAC: '%s' is not a valid IP\n", IP_char);
    inverterIPSet = false;  // reopens the portal
    preferences.putBool("inverterIPSet", false);
    return;
  }

  eth_addr *inverter_MAC = get_sta_mac(target);
  if (inverter_MAC == nullptr) {
    // Normal on the first pass - etharp_request() has only just been queued.
    if (attempts < 255) attempts++;
    if (attempts == ARP_FAST_ATTEMPTS) {
      DEBUG_PRINTF("resolveMAC: no ARP reply from %s after %u tries, backing off\n",
                   IP_char, (unsigned)ARP_FAST_ATTEMPTS);
    }
    return;
  }

  snprintf(MAC_char, sizeof(MAC_char), "%02x:%02x:%02x:%02x:%02x:%02x",
           inverter_MAC->addr[0], inverter_MAC->addr[1], inverter_MAC->addr[2],
           inverter_MAC->addr[3], inverter_MAC->addr[4], inverter_MAC->addr[5]);

  DEBUG_PRINTF("Resolved MAC for %s: %s\n", IP_char, MAC_char);
  preferences.putString("inverter_MAC", MAC_char);
  inverterMACSet = true;
  preferences.putBool("inverterMACSet", inverterMACSet);
  attempts = 0;
}
