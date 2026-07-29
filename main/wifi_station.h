#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

/**
 * @brief Initialize Wi-Fi station mode and connect to the configured router.
 *
 * This function:
 *   1. Initializes ESP-NETIF.
 *   2. Creates the default event loop.
 *   3. Creates the default Wi-Fi station interface.
 *   4. Initializes and starts the Wi-Fi driver.
 *   5. Waits until the station receives an IP address or exhausts retries.
 *
 * NVS must already be initialized before this function is called.
 *
 * @return
 *      - ESP_OK if the station connected and received an IP address.
 *      - ESP_FAIL if the retry limit was reached.
 *      - Another ESP-IDF error code if initialization failed.
 */
esp_err_t wifi_station_start(void);

/**
 * @brief Return the IPv4 gateway address received through DHCP.
 *
 * This will normally be the local address of the Wi-Fi router.
 * Call this only after wifi_station_start() returns ESP_OK.
 *
 * @return Router gateway IPv4 address.
 */
esp_ip4_addr_t wifi_station_get_gateway(void);

/**
 * @brief Report whether the Wi-Fi station currently has an IP address.
 *
 * @return true if connected and assigned an IP address; otherwise false.
 */
bool wifi_station_is_connected(void);
