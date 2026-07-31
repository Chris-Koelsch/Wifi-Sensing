#include "csi_capture.h"
#include "packet_generator.h"
#include "wifi_station.h"
#include "network_stream.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Wi-Fi sensing project");

    esp_err_t nvs_result = nvs_flash_init();

    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    /*
     * This function initializes and starts the Wi-Fi driver and waits
     * until the ESP32 connects to the router.
     */
    ESP_ERROR_CHECK(
        wifi_sensing_station_start()
    );
    /*
     * Wi-Fi is now running, so CSI can be configured and enabled.
     */
    ESP_ERROR_CHECK(
        csi_capture_start()
    );

    ESP_ERROR_CHECK(
        network_stream_start()
    );

    const esp_ip4_addr_t gateway =
        wifi_sensing_station_get_gateway();
    ESP_ERROR_CHECK(
        packet_generator_start(gateway)
    );

    ESP_LOGI(
        TAG,
        "Wi-Fi connected and CSI capture started"
    );
}
