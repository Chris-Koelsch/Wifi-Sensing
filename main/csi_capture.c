#include "csi_capture.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "csi_capture";

/*
 * This function is called by the ESP-IDF Wi-Fi task every time
 * CSI data is available.
 *
 * Keep it short. Do not run signal processing or large printf loops here.
 */
static void csi_receive_callback(
    void *ctx,
    wifi_csi_info_t *csi_info
)
{
    (void)ctx;

    if (csi_info == NULL) {
        return;
    }

    if (csi_info->buf == NULL) {
        return;
    }

    /*
     * Temporary debugging only.
     *
     * This confirms that the callback is receiving data.
     * Remove or rate-limit this log later because CSI callbacks can occur
     * very frequently.
     */
    ESP_LOGI(
        TAG,
        "CSI received: len=%d, RSSI=%d",
        csi_info->len,
        csi_info->rx_ctrl.rssi
    );
}

esp_err_t csi_capture_start(void)
{
    ESP_LOGI(TAG, "Configuring CSI reception");

    /*
     * Register the function that ESP-IDF calls whenever CSI arrives.
     *
     * The second argument is an optional context pointer. We do not
     * need one yet, so it is NULL.
     */
    esp_err_t result = esp_wifi_set_csi_rx_cb(
        csi_receive_callback,
        NULL
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register CSI callback: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Configure which CSI training fields should be captured.
     *
     * This initializer can vary slightly between ESP-IDF versions,
     * so verify the fields available in your installed esp_wifi_types.h.
     */
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0,
    };

    result = esp_wifi_set_csi_config(&csi_config);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure CSI: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    result = esp_wifi_set_csi(true);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to enable CSI: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    ESP_LOGI(TAG, "CSI reception enabled");

    return ESP_OK;
}

esp_err_t csi_capture_stop(void)
{
    ESP_LOGI(TAG, "Disabling CSI reception");

    esp_err_t result = esp_wifi_set_csi(false);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to disable CSI: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * Passing NULL unregisters the callback.
     */
    return esp_wifi_set_csi_rx_cb(NULL, NULL);
}
