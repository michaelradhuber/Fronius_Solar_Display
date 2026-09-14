#include "netscanner.h"

#define DEBUG //Enable for debugging output over Serial

/* ---- DEBUG SECTION ---- */

#ifdef DEBUG
  #define DEBUG_PRINT(x) Serial.print (x)
  #define DEBUG_PRINTLN(x) Serial.println (x)
  #define DEBUG_PRINTF2(x,y) Serial.printf (x,y)
  #define DEBUG_PRINTF3(x,y,z) Serial.printf (x,y,z)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF2(x,y)
  #define DEBUG_PRINTF3(x,y,z)  
#endif

/* ---- END DEBUG SECTION ---- */

cJSON *arp_table_json;
static const char *TAG = "Network_Scanner";
char interface_ip[16]; //used for arp quering

//Constructor
NetScanner::NetScanner() {
}

//Destructor
NetScanner::~NetScanner() {
    cleanup();
}

//Manual Destructor
void NetScanner::end() {
    cleanup();
}

//Cleanup function
void NetScanner::cleanup() {
    if (arp_table_json != NULL) {
        cJSON_Delete(arp_table_json);
        arp_table_json = NULL;
    }
}

//Explicit begin
void NetScanner::begin() {
    arp_table_json = cJSON_CreateObject();

    // No nvs_flash_init()/nvs_flash_erase() here. Arduino already initialises NVS
    // during startup, so re-running it is at best a no-op - and the upstream
    // ESP_ERR_NVS_NO_FREE_PAGES branch erased the WHOLE NVS partition, taking the
    // WiFi credentials with it. A network scanner has no business touching NVS.

    // The tcpip_adapter API this used (tcpip_adapter_get_ip_info /
    // TCPIP_ADAPTER_IF_STA) was removed in ESP-IDF 5. Under Arduino the STA
    // address is simply WiFi.localIP(), which avoids esp_netif entirely.
    strlcpy(interface_ip, WiFi.localIP().toString().c_str(), sizeof(interface_ip));

    DEBUG_PRINT(F("Your IP: "));      DEBUG_PRINTLN(WiFi.localIP());
    DEBUG_PRINT(F("Your netmask: ")); DEBUG_PRINTLN(WiFi.subnetMask());
    DEBUG_PRINT(F("Your Gateway: ")); DEBUG_PRINTLN(WiFi.gatewayIP());
}

// Turns "192.168.1.57" into the "192.168.1." prefix used to enumerate the subnet.
// The original called strtok() directly on interface_ip, which overwrites the '.'
// separators with NULs - so interface_ip was destroyed by the first call and every
// later scan silently saw a truncated address. Work on a copy.
void NetScanner::splitIp(char* interface_ip, char* from_ip) {
    if (interface_ip == nullptr || from_ip == nullptr) {
        DEBUG_PRINTLN(F("splitIp: Null pointer passed"));
        return;
    }
    char work[16];
    strlcpy(work, interface_ip, sizeof(work));

    from_ip[0] = '\0';
    int string_index = 0;
    char *saveptr = nullptr;
    char *token = strtok_r(work, ".", &saveptr);
    for (int i = 0; i < 3; i++) {
        if (token == nullptr) {
            DEBUG_PRINTLN(F("splitIp: Invalid IP format"));
            from_ip[0] = '\0';
            return;
        }
        string_index += snprintf(from_ip + string_index, 16 - string_index, "%s.", token);
        token = strtok_r(NULL, ".", &saveptr);
    }
}

void NetScanner::readArpTable(char * from_ip, int read_from, int read_to){
        DEBUG_PRINTF3("Reading ARP table from: %d to %d", read_from, read_to);
    for (int i = read_from; i <= read_to; i++) {
		char test[32];
		sprintf(test, "%s%d", from_ip, i);
        ip4_addr_t test_ip;
        ip4addr_aton(test, &test_ip);
        
        const ip4_addr_t *ipaddr_ret;
        struct eth_addr *eth_ret = NULL;
        // etharp_* is lwIP core: must hold the TCPIP core lock off the lwIP task.
        LOCK_TCPIP_CORE();
        s8_t found = etharp_find_addr(NULL, &test_ip, &eth_ret, &ipaddr_ret);
        UNLOCK_TCPIP_CORE();
        if(found >= 0){
            DEBUG_PRINTF2("Adding found IP: %s", ip4addr_ntoa(&test_ip));
            cJSON *entry;
            char entry_name[10];
            char mac[18];
            strncpy (mac, eth_ntoa(eth_ret), 18);

            itoa(i, entry_name, 10);
            cJSON_AddItemToObject(arp_table_json, entry_name, entry=cJSON_CreateObject()); //the key name will be the last ip
            cJSON_AddStringToObject(entry, "ip", ip4addr_ntoa(&test_ip));
            cJSON_AddStringToObject(entry, "mac", mac);
        }
	}
}


void NetScanner::sendArp(char * from_ip){
    DEBUG_PRINTLN( "Sending ARP requests to the whole network");
    const TickType_t xDelay = (500) / portTICK_PERIOD_MS; //set sleep time for 0.5 seconds
    // tcpip_adapter_get_netif() is gone in ESP-IDF 5. With WiFi STA as the only
    // interface, the default lwIP netif is the one we want.
    struct netif *netif_interface = netif_default;
    if (netif_interface == nullptr) {
        DEBUG_PRINTLN(F("sendArp: no default netif"));
        return;
    }
    //since the default arp table size in lwip is 10, and after 10 it overrides existing entries,
    //after each 10 arp reqeusts sent, we'll try to read and store from the arp table.
    int counter = 0;
    int read_entry_from = 1;
    int read_entry_to = 10;
    // NOTE: `char i` here was a bug - a signed char overflows at 127, so `i < 255`
    // never became false and the loop ran forever.
    for (int i = 1; i < 255; i++) {
        if (counter > 9){
            counter = 0; //zeoring arp table counter back to 0
            readArpTable(from_ip, read_entry_from, read_entry_to);
            read_entry_from = read_entry_from + 10;
            read_entry_to = read_entry_to + 10;
        }
        char test[32];
        snprintf(test, sizeof(test), "%s%d", from_ip, i);
        ip4_addr_t test_ip;
        ip4addr_aton(test, &test_ip);

        // do arp request. Lock only around the lwIP call - never hold the core lock
        // across vTaskDelay(), that would stall the whole TCPIP task.
        LOCK_TCPIP_CORE();
        etharp_request(netif_interface, &test_ip);
        UNLOCK_TCPIP_CORE();
        vTaskDelay( xDelay ); //sleep for 0.5 seconds
        counter++;
    }
    //reading last entries
    readArpTable(from_ip, read_entry_from, 255);
}

void NetScanner::printArpTable(){
    char from_ip[16];
    splitIp(interface_ip, from_ip);
    sendArp(from_ip);
    DEBUG_PRINTLN( "Printing ARP table");
    for (int i = 1; i < 255; i++) {   // was `char i`: overflows at 127, loops forever
        char entry_name[10];
        itoa(i, entry_name, 10);
        cJSON *entry = cJSON_GetObjectItem(arp_table_json, entry_name);
        if (entry!= NULL){
            printf("\n**********************************************\n");
            printf("IP: %s\n", cJSON_GetObjectItem(entry, "ip")->valuestring);
            printf("MAC address: %s\n", cJSON_GetObjectItem(entry, "mac")->valuestring);
        
        }
    }
    cJSON_Delete(arp_table_json);
}

const char* NetScanner::findIP(const char* IP_ToFind){
    char from_ip[16];
    splitIp(interface_ip, from_ip);
    const TickType_t xDelay = (500) / portTICK_PERIOD_MS; //set sleep time for 0.5 seconds
    struct netif *netif_interface = netif_default;
    if (netif_interface == nullptr) {
        DEBUG_PRINTLN(F("findIP: no default netif"));
        return NULL;
    }
    /* Search MAC to IP*/
    ip4_addr_t test_ip;
    int retVal = ip4addr_aton(IP_ToFind, &test_ip);
    DEBUG_PRINT(F("\nIP4 ADDR TESTED: "));
    DEBUG_PRINTLN(ip4addr_ntoa(&test_ip));
    DEBUG_PRINT(F("IP4ADDR_ATON RETURNS: "));
    DEBUG_PRINTLN(retVal);

    // One request and one 500 ms look was not enough: a single lost frame at a weak
    // RSSI, or a slow ARP reply, made a present inverter look absent - and the caller
    // used to answer that by deleting the stored IP. Ask up to five times and return
    // on the first hit.
    for (int attempt = 1; attempt <= 5; attempt++) {
        LOCK_TCPIP_CORE();
        etharp_request(netif_interface, &test_ip);
        UNLOCK_TCPIP_CORE();

        vTaskDelay( xDelay ); //sleep for 0.5 seconds

        const ip4_addr_t *ipaddr_ret;
        struct eth_addr *eth_ret = NULL;
        struct eth_addr mac_copy;
        LOCK_TCPIP_CORE();
        s8_t found = etharp_find_addr(NULL, &test_ip, &eth_ret, &ipaddr_ret);
        if (found >= 0) mac_copy = *eth_ret;  // the entry can be evicted once unlocked
        UNLOCK_TCPIP_CORE();

        if (found >= 0) {
            DEBUG_PRINTF2("FOUND on ARP attempt %d\n", attempt);
            return eth_ntoa(&mac_copy);
        }
    }
    DEBUG_PRINTLN(F("NOT FOUND after 5 ARP attempts"));
    return NULL;
}

char* NetScanner::findIPbyMAC(const char* MAC_ToFind){
    char from_ip[16];
    if (interface_ip[0] == '\0') {
        DEBUG_PRINTLN(F("findIPbyMAC: interface_ip is empty"));
        return nullptr;
    }
    splitIp(interface_ip, from_ip);
    sendArp(from_ip);
    DEBUG_PRINTLN(F("Searching for IP address: "));
    char mac[18];
    for (int i = 1; i < 255; i++) {   // was `char i`: overflows at 127, loops forever
        char entry_name[10];
        itoa(i, entry_name, 10);
        cJSON *entry = cJSON_GetObjectItem(arp_table_json, entry_name);
        if (entry!= NULL){
            strlcpy (mac, cJSON_GetObjectItem(entry, "mac")->valuestring, sizeof(mac));
            DEBUG_PRINT(MAC_ToFind);
            DEBUG_PRINT(F(" == "));
            DEBUG_PRINT(mac);
            /*DEBUG_PRINT(F(" || STRINGCOMP: "));
            DEBUG_PRINTLN(strcmp(mac, MAC_ToFind));*/
            if (strcmp(mac, MAC_ToFind) == 0){
                DEBUG_PRINTLN(F("FOUND"));
                return cJSON_GetObjectItem(entry, "ip")->valuestring;
            }
        }
    }
    DEBUG_PRINTLN(F("NOT FOUND"));
    return NULL;
}    

/**
 * Transcribe Ethernet address
 *
 * @v ll_addr		Link-layer address
 * @ret string		Link-layer address in human-readable format
 */
const char * NetScanner::eth_ntoa(struct eth_addr *eth_ret) {
	static char buf[18]; /* "00:00:00:00:00:00" */

	sprintf (buf, "%02x:%02x:%02x:%02x:%02x:%02x",
        eth_ret->addr[0], eth_ret->addr[1], eth_ret->addr[2],
        eth_ret->addr[3], eth_ret->addr[4], eth_ret->addr[5] );
    DEBUG_PRINT(F("\nBuffered MAC address: "));
    DEBUG_PRINTLN(buf);
	return buf;
}
