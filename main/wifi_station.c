#include "wifi_station.h"

#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/*
 * Event-group bits used to notify wifi_sensing_station_start().
 *
 * BIT0: connected and received an IP address
 * BIT1: exhausted the configured retry count
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static const char *TAG = "wifi_station";

/*
 * These variables are private to this source file.
 */
static EventGroupHandle_t s_wifi_event_group = NULL;

static int s_retry_count = 0;
static bool s_is_connected = false;

static esp_ip4_addr_t s_gateway_address = {0};

/*
 * Event handler called by ESP-IDF for Wi-Fi and IP events.
 *
 * This is not the CSI callback. Its job is only to manage the station
 * connection lifecycle.
 */
static void wifi_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_argument;

    /*
     * esp_wifi_start() eventually generates WIFI_EVENT_STA_START.
     * Begin the asynchronous connection attempt when that occurs.
     */
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Wi-Fi station started; connecting to router");

        const esp_err_t result = esp_wifi_connect();

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "esp_wifi_connect failed: %s",
                esp_err_to_name(result)
            );

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAILED_BIT
            );
        }

        return;
    }

    /*
     * A disconnection may happen during the initial connection or later
     * because of a router restart, signal loss, or authentication issue.
     */
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {

        s_is_connected = false;

        xEventGroupClearBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        if (s_retry_count <
            CONFIG_WIFI_SENSING_MAX_RETRIES) {

            s_retry_count++;

            ESP_LOGW(
                TAG,
                "Disconnected; retrying connection (%d/%d)",
                s_retry_count,
                CONFIG_WIFI_SENSING_MAX_RETRIES
            );

            const esp_err_t result = esp_wifi_connect();

            if (result != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Reconnect request failed: %s",
                    esp_err_to_name(result)
                );
            }
        } else {
            ESP_LOGE(
                TAG,
                "Maximum Wi-Fi retry count reached"
            );

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAILED_BIT
            );
        }

        return;
    }

    /*
     * The station is not fully usable for IP traffic merely because it
     * associated with the access point. We wait for DHCP to assign an IP.
     */
    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP) {

        const ip_event_got_ip_t *ip_event =
            (const ip_event_got_ip_t *)event_data;

        s_gateway_address = ip_event->ip_info.gw;
        s_retry_count = 0;
        s_is_connected = true;

        ESP_LOGI(
            TAG,
            "Station IPv4 address: " IPSTR,
            IP2STR(&ip_event->ip_info.ip)
        );

        ESP_LOGI(
            TAG,
            "Network mask: " IPSTR,
            IP2STR(&ip_event->ip_info.netmask)
        );

        ESP_LOGI(
            TAG,
            "Router gateway: " IPSTR,
            IP2STR(&ip_event->ip_info.gw)
        );

        xEventGroupClearBits(
            s_wifi_event_group,
            WIFI_FAILED_BIT
        );

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}

esp_err_t wifi_sensing_station_start(void)
{
    /*
     * Prevent accidental initialization more than once.
     */
    if (s_wifi_event_group != NULL) {
        ESP_LOGW(
            TAG,
            "wifi_sensing_station_start called more than once"
        );

        return s_is_connected ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_wifi_event_group = xEventGroupCreate();

    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    /*
     * Initialize the network-interface abstraction over the LwIP stack.
     */
    esp_err_t result = esp_netif_init();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_netif_init failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Create the default ESP-IDF event loop used for Wi-Fi and IP events.
     */
    result = esp_event_loop_create_default();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not create default event loop: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Create and attach the default station network interface.
     */
    esp_netif_t *station_netif =
        esp_netif_create_default_wifi_sta();

    if (station_netif == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to create default station interface"
        );

        return ESP_FAIL;
    }

    /*
     * Espressif recommends initializing this structure with
     * WIFI_INIT_CONFIG_DEFAULT() so required internal fields and future
     * defaults are populated correctly.
     */
    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    result = esp_wifi_init(&wifi_init_config);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_wifi_init failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Register the same callback for:
     *
     *   - all Wi-Fi events;
     *   - the specific IP-assigned event.
     */
    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;

    result = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &wifi_event_instance
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register Wi-Fi event handler: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    result = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &ip_event_instance
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register IP event handler: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Zero-initialization is important because wifi_config_t contains
     * more fields than only the SSID and password arrays.
     */
    wifi_config_t wifi_config = {0};

    /*
     * Copy Kconfig values into the fixed-size station configuration arrays.
     */
    strlcpy(
        (char *)wifi_config.sta.ssid,
        CONFIG_WIFI_SENSING_SSID,
        sizeof(wifi_config.sta.ssid)
    );

    strlcpy(
        (char *)wifi_config.sta.password,
        CONFIG_WIFI_SENSING_PASSWORD,
        sizeof(wifi_config.sta.password)
    );

    /*
     * Reject access points weaker than WPA2 security.
     *
     * Change this only if you intentionally need to use an open or older
     * Wi-Fi network.
     */
    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    /*
     * A normal all-channel scan is preferable during development.
     */
    wifi_config.sta.scan_method =
        WIFI_ALL_CHANNEL_SCAN;

    /*
     * Prefer the matching AP with the strongest signal.
     */
    wifi_config.sta.sort_method =
        WIFI_CONNECT_AP_BY_SIGNAL;

    result = esp_wifi_set_mode(WIFI_MODE_STA);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_wifi_set_mode failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    result = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_wifi_set_config failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Start the driver. The actual connection begins asynchronously when
     * WIFI_EVENT_STA_START reaches wifi_event_handler().
     */
    result = esp_wifi_start();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_wifi_start failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Disable modem power saving for lower packet-reception latency and
     * more consistent timing during CSI experiments.
     */
    result = esp_wifi_set_ps(WIFI_PS_NONE);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not disable Wi-Fi power saving: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    ESP_LOGI(
        TAG,
        "Waiting for an IP address from the router"
    );

    /*
     * Wait until either:
     *
     *   - DHCP supplies an IP address, or
     *   - the configured retry limit is reached.
     */
    const EventBits_t event_bits =
        xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );

    if ((event_bits & WIFI_CONNECTED_BIT) != 0) {
        ESP_LOGI(
            TAG,
            "Connected to SSID \"%s\"",
            CONFIG_WIFI_SENSING_SSID
        );

        return ESP_OK;
    }

    if ((event_bits & WIFI_FAILED_BIT) != 0) {
        ESP_LOGE(
            TAG,
            "Failed to connect to SSID \"%s\"",
            CONFIG_WIFI_SENSING_SSID
        );

        return ESP_FAIL;
    }

    /*
     * The task should not normally reach this branch.
     */
    ESP_LOGE(
        TAG,
        "Unexpected Wi-Fi event-group result"
    );

    return ESP_FAIL;
}

esp_ip4_addr_t wifi_sensing_station_get_gateway(void)
{
    return s_gateway_address;
}

bool wifi_sensing_station_is_connected(void)
{
    return s_is_connected;
}
