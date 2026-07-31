#include "csi_capture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "csi_capture";

static QueueHandle_t s_csi_queue = NULL;
/*
 * diagnostic counters
 */
static volatile uint32_t s_received_count = 0;
static volatile uint32_t s_dropped_count = 0;
static uint32_t s_next_sequence_number = 0;

/* called by the esp-idf task whenever CSI is available 
 * validates pointers
 * copy metadata
 * copy CSI bytes
 * attempt a nonblocking queue send.
 * Return
 */
static void csi_receive_callback(
    void *ctx,
    wifi_csi_info_t *csi_info
)
{
    (void)ctx;

    if (csi_info == NULL ||
        csi_info->buf == NULL ||
        s_csi_queue == NULL) {
        return;
    }

/* Init every field, including any compiler= added padding
 */
    csi_record_t record = {0};

    record.sequence_number =
        s_next_sequence_number++;

    record.timestamp_us =
        esp_timer_get_time();

    memcpy(
        record.source_mac,
        csi_info->mac,
        sizeof(record.source_mac)
    );

    record.rssi =
        csi_info->rx_ctrl.rssi;

    record.noise_floor =
        csi_info->rx_ctrl.noise_floor;

    record.channel =
        csi_info->rx_ctrl.channel;

    record.original_length =
        csi_info->len;

    record.first_word_invalid =
        csi_info->first_word_invalid;

/* never copy more bytes then the dest array can hold.
 */
    size_t bytes_to_copy = 
        (size_t)csi_info->len;

    if (bytes_to_copy >
        (size_t)CONFIG_CSI_MAX_DATA_LENGTH) {

        bytes_to_copy =
            CONFIG_CSI_MAX_DATA_LENGTH;

        record.truncated = true;
    }
    
    record.stored_length =
        (uint16_t)bytes_to_copy;

    memcpy(
        record.data,
        csi_info->buf,
        bytes_to_copy
    );

/* Use zero waiting time
 * csi callback must not block the wifi task if the queue is full
 */

    const BaseType_t queue_result =
        xQueueSend(
            s_csi_queue,
            &record,
            0
        );

    if (queue_result == pdPASS) {
        s_received_count++;
    } else {
        s_dropped_count++;
    }
}
esp_err_t csi_capture_start(void)
{
    if (s_csi_queue != NULL) {
        ESP_LOGW(
            TAG,
            "CSI capture is already initialized"
        );
        return ESP_ERR_INVALID_STATE;
    }
    /* Each queue slot contains on complete csi_record_t
     */
    s_csi_queue = xQueueCreate(
        CONFIG_CSI_QUEUE_LENGTH,
        sizeof(csi_record_t)
    );

    if (s_csi_queue == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to create CSI queue"
        );
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Created CSI queue: %d record x %zu bytes",
        CONFIG_CSI_QUEUE_LENGTH,
        sizeof(csi_record_t)
    );

    ESP_LOGI(
        TAG,
        "Approximate queue payload storage: %zu bytes",
        (size_t)CONFIG_CSI_QUEUE_LENGTH *
            sizeof(csi_record_t)
    );

    /* register our callback first* 
     */
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
        
        vQueueDelete(s_csi_queue);
        s_csi_queue = NULL;

        return result;
    }

    /*
     * Configure which CSI training fields should be captured.
     *
     * This initializer can vary slightly between ESP-IDF versions,
     * so verify the fields available in your installed esp_wifi_types.h.
     */
    const wifi_csi_config_t csi_config = {
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

        esp_wifi_set_csi_rx_cb(NULL, NULL);

        vQueueDelete(s_csi_queue);
        s_csi_queue = NULL;

        return result;
    }

    result = esp_wifi_set_csi(true);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to enable CSI: %s",
            esp_err_to_name(result)
        );
        
        esp_wifi_set_csi_rx_cb(NULL, NULL);

        vQueueDelete(s_csi_queue);
        s_csi_queue = NULL;

        return result;
    }

    s_received_count = 0;
    s_dropped_count = 0;
    s_next_sequence_number = 0;

    ESP_LOGI(
        TAG,
        "CSI reception enabled"
    );

    return ESP_OK;
}

esp_err_t csi_capture_stop(void)
{
    if (s_csi_queue == NULL) {
        ESP_LOGW(
            TAG,
            "CSI capture is not running"
        );

        return ESP_ERR_INVALID_STATE;
    }

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
    
    if (result != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Failed to unregister CSI callback: %s",
        esp_err_to_name(result)
    );

    return result;
}

vQueueDelete(s_csi_queue);
s_csi_queue = NULL;

ESP_LOGI(
    TAG,
    "CSI capture stopped"
);

return ESP_OK;
}

bool csi_capture_receive(
    csi_record_t *record,
    TickType_t timeout
)
{
    if (record == NULL ||
        s_csi_queue == NULL) {

        return false;
    }

    return xQueueReceive(
        s_csi_queue,
        record,
        timeout
    ) == pdPASS;
}

uint32_t csi_capture_get_received_count(void)
{
    return s_received_count;
}

uint32_t csi_capture_get_dropped_count(void)
{
    return s_dropped_count;
}

