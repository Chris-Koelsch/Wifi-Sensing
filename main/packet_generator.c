#include "packet_generator.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

#include "lwip/ip_addr.h"

#include "ping/ping_sock.h"

static const char *TAG = "packet_generator";

/*
 * Handle returned by esp_ping_new_session().
 *
 * A NULL handle means no ping session currently exists.
 */
static esp_ping_handle_t s_ping_handle = NULL;

static bool s_is_running = false;

/*
 * Called by the internal ESP-IDF ping task when a valid echo reply arrives.
 *
 * Keep this callback short. Its purpose is only diagnostic reporting.
 */
static void on_ping_success(
    esp_ping_handle_t ping_handle,
    void *callback_argument
)
{
    (void)callback_argument;

    uint16_t sequence_number = 0;
    uint32_t elapsed_time_ms = 0;
    uint32_t received_length = 0;
    uint8_t ttl = 0;
    ip_addr_t target_address = {0};

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_SEQNO,
        &sequence_number,
        sizeof(sequence_number)
    );

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_TIMEGAP,
        &elapsed_time_ms,
        sizeof(elapsed_time_ms)
    );

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_SIZE,
        &received_length,
        sizeof(received_length)
    );

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_TTL,
        &ttl,
        sizeof(ttl)
    );

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_IPADDR,
        &target_address,
        sizeof(target_address)
    );

    /*
     * This log should eventually be rate-limited or disabled because a
     * continuous ping session can produce many messages.
     */
    ESP_LOGI(
        TAG,
        "Reply: seq=%u bytes=%" PRIu32 " ttl=%u time=%" PRIu32 " ms",
        sequence_number,
        received_length,
        ttl,
        elapsed_time_ms
    );
}

/*
 * Called when one echo request does not receive a reply before timeout.
 */
static void on_ping_timeout(
    esp_ping_handle_t ping_handle,
    void *callback_argument
)
{
    (void)callback_argument;

    uint16_t sequence_number = 0;

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_SEQNO,
        &sequence_number,
        sizeof(sequence_number)
    );

    ESP_LOGW(
        TAG,
        "Ping timeout: seq=%u",
        sequence_number
    );
}

/*
 * Called when the ping session finishes.
 *
 * Because this project uses ESP_PING_COUNT_INFINITE, this callback should
 * normally occur only after the session is stopped.
 */
static void on_ping_end(
    esp_ping_handle_t ping_handle,
    void *callback_argument
)
{
    (void)callback_argument;

    uint32_t transmitted = 0;
    uint32_t received = 0;

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_REQUEST,
        &transmitted,
        sizeof(transmitted)
    );

    esp_ping_get_profile(
        ping_handle,
        ESP_PING_PROF_REPLY,
        &received,
        sizeof(received)
    );

    ESP_LOGI(
        TAG,
        "Ping session ended: sent=%" PRIu32 ", received=%" PRIu32,
        transmitted,
        received
    );

    s_is_running = false;
}

esp_err_t packet_generator_start(esp_ip4_addr_t gateway)
{
    if (s_ping_handle != NULL || s_is_running) {
        ESP_LOGW(
            TAG,
            "Packet generator is already running"
        );

        return ESP_ERR_INVALID_STATE;
    }

    if (gateway.addr == 0) {
        ESP_LOGE(
            TAG,
            "Invalid gateway address"
        );

        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Start with Espressif's recommended default ping configuration.
     */
    esp_ping_config_t ping_config =
        ESP_PING_DEFAULT_CONFIG();

    /*
     * Convert esp_ip4_addr_t into the generic lwIP ip_addr_t used by
     * esp_ping_config_t.
     */
    ip_addr_t target_address = {0};

    IP_ADDR4(
        &target_address,
        ip4_addr1(&gateway),
        ip4_addr2(&gateway),
        ip4_addr3(&gateway),
        ip4_addr4(&gateway)
    );

    ping_config.target_addr = target_address;

    /*
     * A zero count means the session continues indefinitely until
     * esp_ping_stop() is called.
     */
    ping_config.count = ESP_PING_COUNT_INFINITE;

    /*
     * Use your Kconfig-defined packet interval.
     */
    ping_config.interval_ms =
        CONFIG_CSI_PING_INTERVAL_MS;

    /*
     * Timeout for one echo request.
     *
     * This should normally be longer than the interval.
     */
    ping_config.timeout_ms = 1000;

    /*
     * Small packets are sufficient to generate Wi-Fi receive traffic.
     * This value is the ICMP payload size, not the full frame size.
     */
    ping_config.data_size = 32;

    esp_ping_callbacks_t callbacks = {
        .cb_args = NULL,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
    };

    ESP_LOGI(
        TAG,
        "Creating ping session for gateway " IPSTR,
        IP2STR(&gateway)
    );

    esp_err_t result = esp_ping_new_session(
        &ping_config,
        &callbacks,
        &s_ping_handle
    );

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ping_new_session failed: %s",
            esp_err_to_name(result)
        );

        s_ping_handle = NULL;
        return result;
    }

    result = esp_ping_start(s_ping_handle);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ping_start failed: %s",
            esp_err_to_name(result)
        );

        esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;

        return result;
    }

    s_is_running = true;

    ESP_LOGI(
        TAG,
        "Packet generation started: interval=%d ms",
        CONFIG_CSI_PING_INTERVAL_MS
    );

    return ESP_OK;
}

esp_err_t packet_generator_stop(void)
{
    if (s_ping_handle == NULL) {
        ESP_LOGW(
            TAG,
            "No packet-generator session exists"
        );

        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result =
        esp_ping_stop(s_ping_handle);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ping_stop failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    result = esp_ping_delete_session(s_ping_handle);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ping_delete_session failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    s_ping_handle = NULL;
    s_is_running = false;

    ESP_LOGI(
        TAG,
        "Packet generation stopped"
    );

    return ESP_OK;
}

bool packet_generator_is_running(void)
{
    return s_is_running;
}
