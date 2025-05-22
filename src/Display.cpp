#define DEBUG //Enable for debugging output over Serial

//https://github.com/agentzex/ESP_network_ip_scanner/tree/master/network_scanner/main
//WORKS with board manager ESP_IDF 2.0.17
//ESP32 WROOM DA MODULE
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#ifdef ESP32
  #include <SPIFFS.h>
#endif
#include <Arduino.h>
#include <LiquidCrystal.h>
// https://github.com/tzapu/WiFiManager
#include <WiFiManager.h> 
#include <Preferences.h>
#include <lwip/etharp.h>
#include <lwip/sockets.h>
#include <netscanner.h>
//JSON, HTTP GET
#include <ArduinoJson.h>
#include <HTTPClient.h>

// define the number of bytes you want to access
#define EEPROM_SIZE 14


/* ---- DEBUG SECTION ---- */

#ifdef DEBUG
  #define DEBUG_PRINT(x)  Serial.print (x)
  #define DEBUG_PRINTLN(x)  Serial.println (x)
  #define DEBUG_PRINTF(x, y)  Serial.printf (x, y)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(x)
#endif

/* ---- END DEBUG SECTION ---- */

#define TRIGGER_PIN 0

const char DEVICE_VERSION[] PROGMEM = "1A";
const char SOFTWARE_VERSION[] PROGMEM = "001_ALPHA";
const char DEVICE_ID[] PROGMEM = "4280d4f3-204c-4bd8";
const char DEVICE_NAME[] PROGMEM = "SOLAR POWER DISPLAY";
const char WIFI_APN[] PROGMEM = "SOLAR_POWER_DISPLAY";
const char WIFI_PASSWORD[] PROGMEM = "password";

const char HTTPSTRING[] PROGMEM = "http://";
const char APIDATA[] PROGMEM = "/solar_api/GetAPIVersion.cgi";
const char INVERTERDATA[] PROGMEM = "/solar_api/v1/GetPowerFlowRealtimeData.fcgi";
const char INVERTERVOLTAGE[] PROGMEM = "/solar_api/v1/GetMeterRealtimeData.cgi?Scope=System";

char IP_char[16] = "0";
char MAC_char[32] = "0";
String inverterReadings;
struct PowerData
{
  int pvProduction;
  int gridConsumption;
  int homeConsumption;
};
struct gridVoltage
{
  float L1;
  float L2;
  float L3;
  int devPercent;
};

int connectErrors = 0;

// wifimanager can run in a blocking mode or a non blocking mode
// Be sure to know how to process loops with no delay() if using non blocking
bool wm_nonblocking = true; // change to true to use non blockingManager
bool shouldSaveConfig = false;
bool inverterIPSet = false;
bool inverterMACSet = false;
bool configPortal = false;
bool inverterAsSmartMeter = true;
bool initSuccess = false; 
long timeout = 0;
long errorTimeout = 0;

WiFiManager wm; // global wm instance
WiFiManagerParameter inverter_input; // global param ( for non blocking w params )
WiFiManagerParameter inverter_input_2;
WiFiManagerParameter inverter_text;
Preferences preferences;
NetScanner scanner;
 
// initialize the library with the numbers of the interface pins
//Wiring: https://deepbluembedded.com/esp32-lcd-display-16x2-without-i2c-arduino/
LiquidCrystal lcd(13, 12, 14, 27, 26, 25);
// VIN GND // RS, EN, D4, D5, D6, D7

/*Liquid Crystal symbols */
// Custom characters for the house
byte bottomLeft[8] = {
  B10000,
  B10000,
  B10000,
  B10110,
  B10110,
  B10110,
  B10110,
  B11111
};

byte bottomRight[8] = {
  B00001,
  B00001,
  B00001,
  B01101,
  B01101,
  B00001,
  B00001,
  B11111
};

byte topLeft[8] = {
  B00000,
  B01101,
  B01110,
  B01100,
  B01000,
  B10000,
  B10000,
  B10000
};
byte topRight[8] = {
  B00000,
  B10000,
  B01000,
  B00100,
  B00010,
  B00001,
  B01101,
  B01101
};


//Custom characters for the pole
byte PoleBLeft[8] = {
  B00100,
  B00100,
  B01000,
  B01000,
  B01000,
  B10000,
  B10000,
  B10000
};

byte PoleMLeft[8] = {
  B11111,
  B00001,
  B00001,
  B00001,
  B00010,
  B00010,
  B00010,
  B00100
};

byte PoleTLeft[8] = {
  B00000,
  B00000,
  B00000,
  B00000,
  B00001,
  B00110,
  B01011,
  B10010
};

byte PoleTRight[8] = {
  B00000,
  B00000,
  B00000,
  B00000,
  B10000,
  B01100,
  B11010,
  B01001
};

byte PoleMRight[8] = {
  B11111,
  B10000,
  B10000,
  B10000,
  B01000,
  B01000,
  B01000,
  B00100
};

byte PoleBRight[8] = {
  B00100,
  B00100,
  B00010,
  B00010,
  B00010,
  B00001,
  B00001,
  B00001
};

void checkButton(){
  // check for button press
  if ( digitalRead(TRIGGER_PIN) == LOW ) {
    // poor mans debounce/press-hold, code not ideal for production
    delay(50);
    if( digitalRead(TRIGGER_PIN) == LOW ){
      DEBUG_PRINTLN(F("WIFI: Button Pressed"));
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print(F("WiFi button pressed,"));
      lcd.setCursor(0,1);
      lcd.print(F("hold for config..."));
      DEBUG_PRINTLN(F("WIFI: Starting config portal"));
      if (!wm.startConfigPortal(WIFI_APN)) {
        wm.startConfigPortal(WIFI_APN);
        configPortal = true;
      }
      // still holding button for 3000 ms, reset settings, code not ideaa for production
      delay(3000); // reset delay hold, not ideal with nonblocking code
      if( digitalRead(TRIGGER_PIN) == LOW ){
        lcd.clear();
        lcd.setCursor(0,0);
        lcd.print(F("WiFi button held"));
        lcd.setCursor(0,1);
        lcd.print(F("Erasing Config..."));
        DEBUG_PRINTLN(F("WIFI: Button Held"));
        DEBUG_PRINTLN(F("WIFI: Erasing Config, restarting"));
        wm.resetSettings();
        ESP.restart();
      }
      
    }
  }
}

eth_addr *get_sta_mac(const u32_t &ip)
{
    ip4_addr requestIP{ip};
    eth_addr *ret_eth_addr = nullptr;
    ip4_addr const *ret_ip_addr = nullptr;
    err_t result = etharp_request(netif_default, &requestIP);
    DEBUG_PRINTF("Request result = %d\n", result);
    delay (100); // Time to respond ARP request. Could be less
    result = etharp_find_addr(netif_default, &requestIP, &ret_eth_addr, &ret_ip_addr);
    DEBUG_PRINTF("Find result = %d\n", result);
    return ret_eth_addr;
}

void resolveMAC() {
  eth_addr* inverter_MAC;
  u32_t entered_IP;
  DEBUG_PRINT(F("PARAM inverter_IP = "));
  DEBUG_PRINTLN(IP_char);
  entered_IP = inet_addr(IP_char);
  inverter_MAC = get_sta_mac(entered_IP);
  
  if (inverter_MAC == nullptr) {
    DEBUG_PRINTLN(F("MAC address not found"));
    return;
  }
  
  #ifdef DEBUG
    Serial.print(F("MAC address: "));
    Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x\n", inverter_MAC->addr[0], inverter_MAC->addr[1], inverter_MAC->addr[2], inverter_MAC->addr[3], inverter_MAC->addr[4], inverter_MAC->addr[5]);
  #endif

    snprintf(MAC_char, sizeof(MAC_char), "%02x:%02x:%02x:%02x:%02x:%02x", inverter_MAC->addr[0], inverter_MAC->addr[1], inverter_MAC->addr[2], inverter_MAC->addr[3], inverter_MAC->addr[4], inverter_MAC->addr[5]);
    DEBUG_PRINT(F("Resolved MAC: "));
    DEBUG_PRINTLN(MAC_char);
    preferences.putString("inverter_MAC", MAC_char);
    inverterMACSet = true;
    preferences.putBool("inverterMACSet", inverterMACSet);

}

void saveParamCallback(){
  DEBUG_PRINTLN(F("[CALLBACK] saveParamCallback fired"));
  String IPInput = inverter_input.getValue();
  IPInput.toCharArray(IP_char,16);
  preferences.putString("inverter_IP", IP_char);
  inverterIPSet = true;
  preferences.putBool("inverterIPSet", inverterIPSet);
  preferences.putString("inverter_MAC", 0);
  inverterMACSet = false;
  preferences.putBool("inverterMACSet", inverterMACSet);
  if (WiFi.status() == WL_CONNECTED) { //If WIFI is already connected get (unique) MAC from IP
    resolveMAC();
  }
}


void saveConfigCallback() {
  shouldSaveConfig = true;
}

String httpGETRequest(const char* serverPath) {
  HTTPClient http;
  // Ensure the URL starts with "http://"
  String url = String(serverPath);
  DEBUG_PRINT(F("Requesting URL: "));
  DEBUG_PRINTLN(url);
  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    DEBUG_PRINTLN(F("Invalid URL: URL must start with 'http://' or 'https://'"));
    return "{}";
  }
  // Begin HTTP request
  http.begin(url.c_str());
  // Send HTTP GET request
  int httpResponseCode = http.GET();
  String payload = "{}"; 

  if (httpResponseCode>0) {
    DEBUG_PRINT(F("HTTP Response code: "));
    DEBUG_PRINTLN(httpResponseCode);
    payload = http.getString();
    //DEBUG_PRINTLN(payload);
  }
  else {
    DEBUG_PRINT(F("Error code: "));
    DEBUG_PRINTLN(httpResponseCode);
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("HTTP Error: "));
    lcd.print(httpResponseCode);
    lcd.setCursor(0,1);
    lcd.print(F("Inverter unavailable"));
  }
  // Free resources
  http.end();
  return payload;
}

PowerData getInverterData() {
  char serverPathData[64] = "";
  strcat_P(serverPathData, HTTPSTRING); 
  strcat(serverPathData, IP_char);
  strcat_P(serverPathData, INVERTERDATA);
  String payload = httpGETRequest(serverPathData);
  PowerData prm = {0,0,0};
  if (strcmp(payload.c_str(), "{}") == 0) {
    DEBUG_PRINTLN(F("Inverter data not available"));
    connectErrors++;
    errorTimeout = millis();
    return prm;
  }
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    DEBUG_PRINT(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());
    return prm;
  }
  if (doc["Body"]["Data"]["Inverters"]["1"]["P"] == nullptr) {
    DEBUG_PRINTLN(F("Inverter data not available"));
    connectErrors++;
    return prm;
  }
  // Extract values
  float pvProduction = 0;
  float gridConsumption = 0;
  float homeConsumption = 0;
  for (int i = 1; i <= 9; i++) { //Maximum of 10 inverters
    float inverterPower = doc["Body"]["Data"]["Inverters"][String(i)]["P"].as<float>();
    pvProduction = pvProduction + inverterPower;
    // DEBUG_PRINTLN(i);
    // DEBUG_PRINTLN(pvProduction);
  }
  if (inverterAsSmartMeter){
    for (int i = 1; i <= 9; i++) { //Maximum of 10 smart meters
      const char* category = doc["Body"]["Data"]["SecondaryMeters"][String(i)]["Category"].as<const char*>();
      if (category != nullptr && (strcmp(category, "METER_CAT_WR") == 0 || strcmp(category, "METER_CAT_BAT") == 0 || strcmp(category, "METER_CAT_PV_BAT") == 0)) {
        pvProduction = pvProduction + doc["Body"]["Data"]["SecondaryMeters"][String(i)]["P"].as<float>();
        // DEBUG_PRINTLN(i);
        // DEBUG_PRINTLN(pvProduction);
      }
    }
  }
  gridConsumption = doc["Body"]["Data"]["Site"]["P_Grid"].as<float>();
  homeConsumption = pvProduction + gridConsumption;

  prm.pvProduction = round(pvProduction);
  prm.gridConsumption = round(gridConsumption);
  prm.homeConsumption = round(homeConsumption);

  return prm;
} 

gridVoltage getGridVoltage() {
  char serverPathData[128] = "";
  strcat_P(serverPathData, HTTPSTRING); 
  strcat(serverPathData, IP_char);
  strcat_P(serverPathData, INVERTERVOLTAGE);
  String payload = httpGETRequest(serverPathData);
  //DEBUG_PRINTLN(payload);
  gridVoltage gV = {0,0,0};
  if (strcmp(payload.c_str(), "{}") == 0) {
    DEBUG_PRINTLN(F("Inverter data not available"));
    connectErrors++;
    errorTimeout = millis();
    return gV;
  }
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    DEBUG_PRINT(F("deserializeJson() failed: "));
    DEBUG_PRINTLN(error.f_str());
    return gV;
  }
  // Find the primary meter key
  JsonObject data = doc["Body"]["Data"].as<JsonObject>();
  JsonObject primaryMeter;
  for (JsonPair kv : data) {
      JsonObject meter = kv.value().as<JsonObject>();
      float location = meter["Meter_Location_Current"].as<float>();
      if (location == 0.0) {
        primaryMeter = meter;
        break;
      }
  }
  // If not found, fallback to first meter
  if (primaryMeter.isNull() && data.size() > 0) {
    primaryMeter = data.begin()->value().as<JsonObject>();
  }

  if (primaryMeter.isNull() || primaryMeter["Voltage_AC_Phase_1"] == nullptr) {
    DEBUG_PRINTLN(F("Meter data not available"));
    connectErrors++;
    return gV;
  }
  // Extract values
  float UAC_L1 = primaryMeter["Voltage_AC_Phase_1"].as<float>();
  float UAC_L2 = primaryMeter["Voltage_AC_Phase_2"].as<float>();
  float UAC_L3 = primaryMeter["Voltage_AC_Phase_3"].as<float>();
  gV.L1 = UAC_L1;
  gV.L2 = UAC_L2;
  gV.L3 = UAC_L3;

  //Calculate deviation
  float UACmax = max(UAC_L1, max(UAC_L2, UAC_L3));
  float UACmin = min(UAC_L1, min(UAC_L2, UAC_L3));
  int devMinPerc = round(UACmin / 230 * 100);
  int devMaxPerc = round(UACmax / 230 * 100);

  int minDistance = abs(100 - devMinPerc);
  int maxDistance = abs(100 - devMaxPerc);

  if (minDistance > maxDistance) {
    gV.devPercent = devMinPerc;
  } else {
    gV.devPercent = devMaxPerc;
  }

  return gV;
} 
 
void setup() {
  #ifdef DEBUG
    Serial.begin(115200);
  #endif
    // set up the LCD's number of columns and rows:
  delay(2000);
  lcd.begin(20, 4);
  delay(100);
  lcd.clear();
  delay(100);
  // Create custom characters
  lcd.createChar(0, bottomLeft);
  lcd.createChar(1, bottomRight);
  lcd.createChar(2, topLeft);
  lcd.createChar(3, topRight);

  delay(500);

  // Print a message to the LCD.
  lcd.setCursor(0,0);
  lcd.print(DEVICE_NAME);
  lcd.setCursor(0,1);
  lcd.print(F("VERSION: ")); lcd.print(DEVICE_VERSION);
  lcd.setCursor(0,2);
  lcd.print(F("SOFTWARE: ")); lcd.print(SOFTWARE_VERSION);
  lcd.setCursor(0,3);
  lcd.print(DEVICE_ID);

  // WIFI MANAGER CODE //
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP  
  WiFi.begin();
  // Wait for 10 seconds to connect to WiFi
  long timeout = millis() + 10000;
  while (WiFi.status() != WL_CONNECTED && millis() < timeout) {
    delay(500);
    DEBUG_PRINT(".");
  }
  delay(100);
  DEBUG_PRINTLN(F("\n Starting Wifi Manager"));

  //Load custom parameters from preferences
  preferences.begin("inverter_config", false);
  strcpy(IP_char, preferences.getString("inverter_IP", "0").c_str());
  strcpy(MAC_char, preferences.getString("inverter_MAC", "0").c_str());
  inverterMACSet = preferences.getBool("inverterMACSet", false);
  inverterIPSet = preferences.getBool("inverterIPSet", false);
  pinMode(TRIGGER_PIN, INPUT);

  if(wm_nonblocking) wm.setConfigPortalBlocking(false);
  wm.setSaveConfigCallback(saveParamCallback);
  // add a custom input field WiFiManagerParameter("customfieldid", "Custom Field Label", "Custom Field Value", customFieldLength,"placeholder=\"Custom Field Placeholder\"");
  // test custom html input type(checkbox)
  // new (&custom_field) WiFiManagerParameter("customfieldid", "Custom Field Label", "Custom Field Value", customFieldLength,"placeholder=\"Custom Field Placeholder\" type=\"checkbox\""); // custom html type
  
  //  custom html(radio)
  new (&inverter_input) WiFiManagerParameter("InverterInputID", "Inverter IPv4", IP_char, 15,"placeholder=\"xxx.xxx.xxx.xxx\"");
  new (&inverter_input_2) WiFiManagerParameter("InverterInputID2", "Inverter MAC", MAC_char, 15,"placeholder=\"xx:xx:xx:xx:xx:xx\", disabled=true");
  wm.addParameter(&inverter_input);
  wm.addParameter(&inverter_input_2);
  new (&inverter_text) WiFiManagerParameter("<p>The MAC address will be determined automatically after you connect to your local LAN.</p>");
  wm.addParameter(&inverter_text);
  wm.setSaveParamsCallback(saveParamCallback);

  // custom menu via array or vector
  // 
  // menu tokens, "wifi","wifinoscan","info","param","close","sep","erase","restart","exit" (sep is seperator) (if param is in menu, params will not show up in wifi page!)
  std::vector<const char *> menu = {"wifi","info","param","sep","restart","exit"};
  wm.setMenu(menu);

  // set dark theme
  wm.setClass("invert");

  //wm.setConnectTimeout(6000); // how long to try to connect for before continuing
  wm.setConfigPortalTimeout(600); // auto close configportal after n seconds
  wm.setCaptivePortalEnable(false); // disable captive portal redirection
  wm.setAPClientCheck(true); // avoid timeout if client connected to softap

  // wifi scan settings
  // wm.setRemoveDuplicateAPs(false); // do not remove duplicate ap names (true)
  // wm.setMinimumSignalQuality(20);  // set min RSSI (percentage) to show in scans, null = 8%
  // wm.setShowInfoErase(false);      // do not show erase button on info page
  // wm.setScanDispPerc(true);       // show RSSI as percentage not graph icons
  
  // wm.setBreakAfterConfig(true);   // always exit configportal even if wifi save fails

  // Connect to last known WiFi, if it won't work start config portal
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN(F("WIFI: Failed to connect or hit timeout, starting config portal..."));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("WIFI: Failed to"));
    lcd.setCursor(0,1);
    lcd.print(F("connect, starting "));
    lcd.setCursor(0,2);
    lcd.print(F("config portal..."));
    wm.startConfigPortal(WIFI_APN);
    configPortal = true;
  } else {
    // If you get here, you have connected to the WiFi    
    DEBUG_PRINTLN(F("WIFI: connected...yeey :)"));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("WIFI: connected..."));
    delay(1000);
  }
  DEBUG_PRINT(F("Last inverter IP address saved: "));
  DEBUG_PRINTLN(IP_char);
  DEBUG_PRINT(F("Last inverter MAC address saved: "));
  DEBUG_PRINTLN(MAC_char);
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(F("Saved inverter IP:"));
  lcd.setCursor(0,1);
  lcd.print(IP_char);
  lcd.setCursor(0,2);
  lcd.print(F("Saved inverter MAC:"));
  lcd.setCursor(0,3);
  lcd.print(MAC_char);
  delay(1000);
  // END WIFI MANAGER CODE //

}
 
void loop() {
  // WIFI MANAGER CODE //
  if (wm_nonblocking) wm.process(); // Avoid delays() in loop when non-blocking and other long-running code  
  checkButton();

  // If inverter IP has not been set, start config portal for setup
  if (!inverterIPSet && WiFi.status() == WL_CONNECTED && !configPortal) {
    DEBUG_PRINTLN(F("WIFI: Starting config portal"));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("WIFI: Starting"));
    lcd.setCursor(0,1);
    lcd.print(F("config portal:"));
    lcd.setCursor(0,2);
    lcd.print(WIFI_APN);
    wm.startConfigPortal(WIFI_APN);
    configPortal = true;
  }

  // If config portal timed out and WiFi is not connected, try reconnecting or restart
if (configPortal && !wm.getConfigPortalActive() && WiFi.status() != WL_CONNECTED) {
  DEBUG_PRINTLN(F("Config portal timed out, retrying WiFi connection..."));
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(F("Config portal"));
  lcd.setCursor(0,1);
  lcd.print(F("timed out..."));
  lcd.setCursor(0,2);
  lcd.print(F("Retrying WiFi..."));
  WiFi.begin();
  long retryTimeout = millis() + 10000;
  while (WiFi.status() != WL_CONNECTED && millis() < retryTimeout) {
    delay(500);
    DEBUG_PRINT(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINTLN(F("WiFi reconnected!"));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("WiFi reconnected!"));
    delay(1000);
    configPortal = false;
  } else {
    DEBUG_PRINTLN(F("WiFi reconnect failed, restarting..."));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("WiFi failed,"));
    lcd.setCursor(0,1);
    lcd.print(F("restarting..."));
    delay(2000);
    ESP.restart();
  }
}

  // If inverter IP is set but MAC is not, and WiFi is connected, resolve MAC
  if (inverterIPSet && !inverterMACSet && WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("Resolving MAC for:"));
    lcd.setCursor(0,1);
    lcd.print(IP_char);
    resolveMAC();
  }
  // END WIFI MANAGER CODE //
  if (WiFi.status() == WL_CONNECTED && initSuccess == false) {
      // BEGIN NETWORK SCANNER //
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print(F("Scanning network..."));
      lcd.setCursor(0,1);
      lcd.print(F("Please wait, this"));
      lcd.setCursor(0,2);
      lcd.print(F("might take a bit..."));
      scanner.begin();
      const char *eth_ret = scanner.findIP(IP_char);
      if (eth_ret != nullptr) {
        DEBUG_PRINTLN(F("IP found in ARP table"));
        //Need to implement check for MAC address here (if MAC = MAC_char)
        DEBUG_PRINT(F("Comparing current MAC with stored MAC: "));
        int z = strcmp(MAC_char, eth_ret);
        DEBUG_PRINTLN((z==0) ? F("MAC address matches") : F("MAC address does not match"));
        scanner.end();
        delay(500); //Give the destructor some time to clean up
        if (z!=0) {
          preferences.putString("inverter_IP", 0);
          DEBUG_PRINTLN(F("MAC address does not match"));
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print(F("MAC address does"));
          lcd.setCursor(0,1);
          lcd.print(F("not match, rebooting..."));
          delay(2000);
          ESP.restart();
        }

        char serverPath[56] = "";
        strcat_P(serverPath, HTTPSTRING); 
        strcat(serverPath, IP_char);
        strcat_P(serverPath, APIDATA);
        delay(500);
        inverterReadings = httpGETRequest(serverPath);
        delay(500);
        JsonDocument doc;
          // Deserialize the JSON document
        DeserializationError error = deserializeJson(doc, inverterReadings);
        // Test if parsing succeeds.
        if (error) {
          DEBUG_PRINT(F("deserializeJson() failed: "));
          DEBUG_PRINTLN(error.f_str());
          return;
        }
        int APIVersion = doc["APIVersion"].as<int>();
        DEBUG_PRINT(F("API Version: "));
        DEBUG_PRINTLN(APIVersion);
        if (APIVersion == 1) {
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print(F("API Version: 1"));
          lcd.setCursor(0,1);
          lcd.print(F("...loading data...."));
          initSuccess = true;
        } else {
          DEBUG_PRINTLN(F("API Version not supported"));
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print(F("API Version not"));
          lcd.setCursor(0,1);
          lcd.print(F("supported, rebooting..."));
          delay(2000);
          ESP.restart();
        }
          
      } else {
        //Clear network scanner memory first!
        scanner.end();
        scanner.begin();
        DEBUG_PRINTLN(F("IP not found in ARP table"));
        //Clear preferences so if IP is not found, it will return to full re-initialization after reboot
        preferences.putString("inverter_IP", 0);
        lcd.clear();
        lcd.setCursor(0,0);
        lcd.print(F("Inverter IP changed"));
        lcd.setCursor(0,1);
        lcd.print(F("searching by MAC."));
        lcd.setCursor(0,2);
        lcd.print(F("please be patient..."));
        //Resolve IP by MAC
        char* IPResult = scanner.findIPbyMAC(MAC_char);
        if (IPResult != nullptr) {
          DEBUG_PRINT(F("Resolved IP: "));
          DEBUG_PRINTLN(IPResult);
          strcpy(IP_char, IPResult);
          preferences.putString("inverter_IP", IP_char);
          lcd.setCursor(0,3);
          lcd.print(F("IP found, reboot now"));
          delay(2000);
        }
        ESP.restart();
      }
  }

  //Reboot routine in case of multiple connection errors
  if (connectErrors > 5) {
    DEBUG_PRINTLN(F("Too many connection errors, rebooting..."));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(F("Too many errors,"));
    lcd.setCursor(0,1);
    lcd.print(F("rebooting..."));
    delay(2000);
    ESP.restart();
  }
  //Reset connect errors to 0 after 5 mins timeout
  if (millis() - errorTimeout > 300000) {
    connectErrors = 0;
  }
  //Main function that displays PV data
  if (initSuccess && (millis() - timeout) > 5000) {
    PowerData prm = getInverterData();

    lcd.clear();
    //PV Production
    lcd.setCursor(2,0);
    if (prm.pvProduction < 1000) {
      lcd.write(B10110010);lcd.write(B10110010); lcd.print(" "); lcd.print(prm.pvProduction); lcd.print(F("W"));
    } else {
      float pvProductionOneDec = (float)prm.pvProduction / 1000;
      char buffer[6]; // Buffer to hold the formatted string
      dtostrf(pvProductionOneDec, 4, 1, buffer); // Convert float to string with 1 decimal place
      lcd.write(B10110010);lcd.write(B10110010); lcd.print(" "); lcd.print(buffer); lcd.print(F("kW"));
    }

    // Drawings
    // Draw the house
    lcd.setCursor(0, 2);
    lcd.write(byte(0)); // BAsement
    lcd.setCursor(1, 2);
    lcd.write(byte(1)); // BAsement
    lcd.setCursor(0, 1);
    lcd.write(byte(2)); // Roof
    lcd.setCursor(1, 1);
    lcd.write(byte(3)); // Roof

    //REdefine the custom characters (max 7 supported!)
    lcd.createChar(4, PoleTLeft);
    lcd.createChar(5, PoleTRight);
    lcd.createChar(6, PoleMLeft);
    lcd.createChar(7, PoleMRight);

    lcd.setCursor(18, 0);
    lcd.write(byte(4)); // Pole top
    lcd.setCursor(18, 1);
    lcd.write(byte(6)); // Pole middle
    lcd.setCursor(18, 2);
    lcd.print(F("|")); // Pole bottom
    lcd.setCursor(19, 0);
    lcd.write(byte(5)); // Pole top
    lcd.setCursor(19, 1);
    lcd.write(byte(7)); // Pole middle
    lcd.setCursor(19, 2);
    lcd.print(F("|")); // Pole bottom

    //Grid consumption
    char gridArrows[12];
    char buffer[7]; // Buffer to hold the formatted string
    if (prm.gridConsumption > 0) {
      strcpy(gridArrows, "<  <  < ");
    } else {
      strcpy(gridArrows, " >  >  >");
    }
    if (abs(prm.gridConsumption) < 1000) {
      dtostrf(abs(prm.gridConsumption), 4, 0, buffer); // Convert float to string with 1 decimal place
      strcat(buffer, "W");
    } else {
      float gridConsumpotionOneDec = (float)abs(prm.gridConsumption) / 1000;
      dtostrf(gridConsumpotionOneDec, 4, 1, buffer); // Convert float to string with 1 decimal place
      strcat(buffer, "kW");
    }
    lcd.setCursor(3,2);
    if (prm.gridConsumption > 0) {
      lcd.print(strcat(gridArrows, buffer));
    } else {
      lcd.print(strcat(buffer, gridArrows));
    }
    gridVoltage gv = getGridVoltage();
    lcd.setCursor(2,3);
    lcd.print(gv.devPercent); lcd.print(F("%gV ")); lcd.print(connectErrors); lcd.print(F("E ")); lcd.print(WiFi.RSSI()); lcd.print(F("RSSI"));

    //PV-Data display Timeout
    timeout = millis();
  }

}
