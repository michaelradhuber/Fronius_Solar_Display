#ifndef NETSCANNER_H
#define NETSCANNER_H

#include <Arduino.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/etharp.h"
//#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

class NetScanner {
public:
    cJSON* arp_table_json;
    char interface_ip[16]; //used for arp quering
    NetScanner();
    void begin();
    void printArpTable();
    const char* findIP(const char* IP_ToFind);
    char* findIPbyMAC(const char* MAC_ToFind);
    void end();
    ~NetScanner();
private:
    const char * eth_ntoa(struct eth_addr *eth_ret);
    void splitIp(char* interface_ip, char* from_ip);
    void sendArp(char* from_ip);
    void readArpTable(char* from_ip, int read_from, int read_to);
    void cleanup();
};

#endif // NETSCANNER_H