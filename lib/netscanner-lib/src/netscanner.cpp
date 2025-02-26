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
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    tcpip_adapter_ip_info_t netif_network_info;
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &netif_network_info);
    strcpy(interface_ip, ip4addr_ntoa(&netif_network_info.ip));
  
    DEBUG_PRINTF2("Your IP: %s", ip4addr_ntoa(&netif_network_info.ip));
    DEBUG_PRINTF2("Your netmask: %s", ip4addr_ntoa(&netif_network_info.netmask));
    DEBUG_PRINTF2("Your Default Gateway: %s", ip4addr_ntoa(&netif_network_info.gw));
}

void NetScanner::splitIp(char* interface_ip, char* from_ip) {
    if (interface_ip == nullptr || from_ip == nullptr) {
        DEBUG_PRINTLN(F("splitIp: Null pointer passed"));
        return;
    }
	int string_index = 0;
	char *token = strtok(interface_ip, ".");
	for (int i = 0; i < 3; i++) {
        if (token == nullptr) {
            DEBUG_PRINTLN(F("splitIp: Invalid IP format"));
            return;
        }
		sprintf(from_ip + string_index, "%s.", token);
		string_index = string_index + strlen(token) + 1; //string index + '.'
		token = strtok(NULL, ".");
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
        //etharp_find_addr(NULL, &test_ip, &eth_ret, (const ip4_addr_t **)&ipaddr_ret)
        if(etharp_find_addr(NULL, &test_ip, &eth_ret, &ipaddr_ret) >= 0){
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
    void * netif = NULL;
    tcpip_adapter_get_netif(TCPIP_ADAPTER_IF_STA, &netif);
    struct netif *netif_interface = (struct netif *)netif;
    //since the default arp table size in lwip is 10, and after 10 it overrides existing entries,
    //after each 10 arp reqeusts sent, we'll try to read and store from the arp table.
    int counter = 0;
    int read_entry_from = 1;
    int read_entry_to = 10;
    for (char i = 1; i < 255; i++) {
	if (counter > 9){
            counter = 0; //zeoring arp table counter back to 0
            readArpTable(from_ip, read_entry_from, read_entry_to);
            read_entry_from = read_entry_from + 10;
            read_entry_to = read_entry_to + 10;
        }
        char test[32];
	sprintf(test, "%s%d", from_ip, i);
        ip4_addr_t test_ip;
        ip4addr_aton(test, &test_ip);
        
        // do arp request
        int8_t arp_request_ret = etharp_request(netif_interface, &test_ip);
        //DEBUG_PRINTLN( "etharp_request result: %d", arp_request_ret);
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
    for (char i = 1; i < 255; i++) {
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
    void * netif = NULL;
    tcpip_adapter_get_netif(TCPIP_ADAPTER_IF_STA, &netif);
    struct netif *netif_interface = (struct netif *)netif;
    /* Search MAC to IP*/
    ip4_addr_t test_ip;
    int retVal = ip4addr_aton(IP_ToFind, &test_ip);
    DEBUG_PRINT(F("\nIP4 ADDR TESTED: "));
    DEBUG_PRINTLN(ip4addr_ntoa(&test_ip));
    DEBUG_PRINT(F("IP4ADDR_ATON RETURNS: "));
    DEBUG_PRINTLN(retVal);
    int8_t arp_request_ret = etharp_request(netif_interface, &test_ip);
    //DEBUG_PRINTLN( "etharp_request result: %d", arp_request_ret);
    vTaskDelay( xDelay ); //sleep for 0.5 seconds
    const ip4_addr_t *ipaddr_ret;
    struct eth_addr *eth_ret = NULL;
    //etharp_find_addr(NULL, &test_ip, &eth_ret, (const ip4_addr_t **)&ipaddr_ret)
    if(etharp_find_addr(NULL, &test_ip, &eth_ret, &ipaddr_ret) >= 0){
        DEBUG_PRINTLN(F("FOUND"));
        return eth_ntoa(eth_ret);
    } else {
        DEBUG_PRINTLN(F("NOT FOUND"));
        return NULL;
    }

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
    for (char i = 1; i < 255; i++) {
        char entry_name[10];
        itoa(i, entry_name, 10);
        cJSON *entry = cJSON_GetObjectItem(arp_table_json, entry_name);
        if (entry!= NULL){
            strcpy (mac, cJSON_GetObjectItem(entry, "mac")->valuestring);
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
