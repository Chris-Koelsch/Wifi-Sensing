#include "network_stream.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "sdkconfig.h"

#include "csi_capture.h"
#include "csi_protocol.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"


static const char *TAG = "network_stream";

/*
 * This buffer contains one serialized CSI UDP packet.
 *
 * It is static so the potentially large buffer does not occupy the
 * FreeRTOS task stack.
 */
static uint8_t s_packet_buffer[CSI_PACKET_MAX_SIZE];

static TaskHandle_t s_network_task_handle = NULL;
static int s_socket_fd = -1;

static volatile uint32_t s_sent_count = 0;
static volatile uint32_t s_send_failure_count = 0;
static volatile uint32_t s_format_failure_count = 0;


/*
 * Write a 16-bit value in network byte order, also called big-endian.
 */
static void write_u16_be(
    uint8_t *destination,
    uint16_t value
)
{
    destination[0] =
        (uint8_t)((value >> 8) & 0xFFU);

    destination[1] =
        (uint8_t)(value & 0xFFU);
}


/*
 * Write a 32-bit value in network byte order.
 */
static void write_u32_be(
    uint8_t *destination,
    uint32_t value
)
{
    destination[0] =
        (uint8_t)((value >> 24) & 0xFFU);

    destination[1] =
        (uint8_t)((value >> 16) & 0xFFU);

    destination[2] =
        (uint8_t)((value >> 8) & 0xFFU);

    destination[3] =
        (uint8_t)(value & 0xFFU);
}


/*
 * Write a 64-bit value in network byte order.
 */
static void write_u64_be(
    uint8_t *destination,
    uint64_t value
)
{
    destination[0] =
        (uint8_t)((value >> 56) & 0xFFU);

    destination[1] =
        (uint8_t)((value >> 48) & 0xFFU);

    destination[2] =
        (uint8_t)((value >> 40) & 0xFFU);

    destination[3] =
        (uint8_t)((value >> 32) & 0xFFU);

    destination[4] =
        (uint8_t)((value >> 24) & 0xFFU);

    destination[5] =
        (uint8_t)((value >> 16) & 0xFFU);

    destination[6] =
        (uint8_t)((value >> 8) & 0xFFU);

    destination[7] =
        (uint8_t)(value & 0xFFU);
}


/*
 * Convert one csi_record_t into the binary network protocol.
 *
 * Returns the complete packet length.
 * Returns zero if the record is invalid or the destination is too small.
 */
static size_t serialize_csi_record(
    const csi_record_t *record,
    uint8_t *packet,
    size_t packet_capacity
)
{
    if (record == NULL || packet == NULL) {
        return 0;
    }

    if (
        record->stored_length >
        CONFIG_CSI_MAX_DATA_LENGTH
    ) {
        return 0;
    }

    const size_t packet_length =
        CSI_PACKET_HEADER_SIZE +
        record->stored_length;

    if (packet_length > packet_capacity) {
        return 0;
    }

    /*
     * Clear the header so reserved bytes always have a known value.
     */
    memset(
        packet,
        0,
        CSI_PACKET_HEADER_SIZE
    );

    size_t offset = 0;

    /*
     * Bytes 0–3: protocol magic.
     */
    write_u32_be(
        packet + offset,
        CSI_PACKET_MAGIC
    );

    offset += 4;

    /*
     * Byte 4: protocol version.
     */
    packet[offset] =
        CSI_PACKET_VERSION;

    offset += 1;

    /*
     * Byte 5: flags.
     */
    uint8_t flags = 0;

    if (record->first_word_invalid) {
        flags |=
            CSI_PACKET_FLAG_FIRST_WORD_INVALID;
    }

    if (record->truncated) {
        flags |=
            CSI_PACKET_FLAG_TRUNCATED;
    }

    packet[offset] = flags;
    offset += 1;

    /*
     * Bytes 6–7: fixed header length.
     */
    write_u16_be(
        packet + offset,
        CSI_PACKET_HEADER_SIZE
    );

    offset += 2;

    /*
     * Bytes 8–11: sequence number.
     */
    write_u32_be(
        packet + offset,
        record->sequence_number
    );

    offset += 4;

    /*
     * Bytes 12–19: microsecond timestamp.
     *
     * esp_timer_get_time() produces nonnegative values during
     * normal operation, so it is serialized as an unsigned 64-bit value.
     */
    write_u64_be(
        packet + offset,
        (uint64_t)record->timestamp_us
    );

    offset += 8;

    /*
     * Bytes 20–25: source MAC address.
     */
    memcpy(
        packet + offset,
        record->source_mac,
        sizeof(record->source_mac)
    );

    offset += sizeof(record->source_mac);

    /*
     * Byte 26: RSSI.
     *
     * Copy the bit pattern from int8_t into uint8_t.
     */
    packet[offset] =
        (uint8_t)record->rssi;

    offset += 1;

    /*
     * Byte 27: noise floor.
     */
    packet[offset] =
        (uint8_t)record->noise_floor;

    offset += 1;

    /*
     * Byte 28: Wi-Fi channel.
     */
    packet[offset] =
        record->channel;

    offset += 1;

    /*
     * Byte 29: reserved for future use.
     */
    packet[offset] = 0;
    offset += 1;

    /*
     * Bytes 30–31: original driver-reported CSI length.
     */
    write_u16_be(
        packet + offset,
        record->original_length
    );

    offset += 2;

    /*
     * Bytes 32–33: number of CSI bytes stored in this packet.
     */
    write_u16_be(
        packet + offset,
        record->stored_length
    );

    offset += 2;

    /*
     * Bytes 34 onward: raw signed CSI bytes.
     *
     * memcpy preserves each int8_t byte's exact bit pattern.
     */
    memcpy(
        packet + offset,
        record->data,
        record->stored_length
    );

    offset += record->stored_length;

    /*
     * This should equal packet_length.
     */
    return offset;
}


static esp_err_t create_udp_socket(
    struct sockaddr_in *destination
)
{
    if (destination == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        destination,
        0,
        sizeof(*destination)
    );

    destination->sin_family =
        AF_INET;

    destination->sin_port =
        htons(CONFIG_CSI_UDP_RECEIVER_PORT);

    const int conversion_result =
        inet_pton(
            AF_INET,
            CONFIG_CSI_UDP_RECEIVER_IP,
            &destination->sin_addr
        );

    if (conversion_result != 1) {
        ESP_LOGE(
            TAG,
            "Invalid receiver IPv4 address: %s",
            CONFIG_CSI_UDP_RECEIVER_IP
        );

        return ESP_ERR_INVALID_ARG;
    }

    s_socket_fd =
        socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_IP
        );

    if (s_socket_fd < 0) {
        ESP_LOGE(
            TAG,
            "Unable to create UDP socket: errno=%d",
            errno
        );

        return ESP_FAIL;
    }

    return ESP_OK;
}


static void close_udp_socket(void)
{
    if (s_socket_fd >= 0) {
        shutdown(s_socket_fd, 0);
        close(s_socket_fd);
        s_socket_fd = -1;
    }
}


/*
 * Send one datagram, retrying briefly when lwIP reports ENOMEM.
 */
static bool send_udp_packet(
    const uint8_t *packet,
    size_t packet_length,
    const struct sockaddr_in *destination
)
{
    if (
        packet == NULL ||
        packet_length == 0 ||
        destination == NULL ||
        s_socket_fd < 0
    ) {
        return false;
    }

    static const unsigned int max_attempts = 3;

    for (
        unsigned int attempt = 0;
        attempt < max_attempts;
        attempt++
    ) {
        const ssize_t bytes_sent =
            sendto(
                s_socket_fd,
                packet,
                packet_length,
                0,
                (const struct sockaddr *)destination,
                sizeof(*destination)
            );

        if (
            bytes_sent >= 0 &&
            (size_t)bytes_sent == packet_length
        ) {
            return true;
        }

        if (errno != ENOMEM) {
            return false;
        }

        /*
         * Allow lwIP and the Wi-Fi driver to release transmit buffers.
         */
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    return false;
}


static void network_stream_task(
    void *task_argument
)
{
    (void)task_argument;

    struct sockaddr_in destination;

    const esp_err_t socket_result =
        create_udp_socket(&destination);

    if (socket_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Network stream stopped because socket setup failed"
        );

        s_network_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(
        TAG,
        "Sending binary CSI packets to %s:%d",
        CONFIG_CSI_UDP_RECEIVER_IP,
        CONFIG_CSI_UDP_RECEIVER_PORT
    );

    ESP_LOGI(
        TAG,
        "Binary packet size: %u-byte header, up to %u total bytes",
        (unsigned)CSI_PACKET_HEADER_SIZE,
        (unsigned)CSI_PACKET_MAX_SIZE
    );

    csi_record_t record;

    while (true) {
        const bool received =
            csi_capture_receive(
                &record,
                portMAX_DELAY
            );

        if (!received) {
            continue;
        }

        const size_t packet_length =
            serialize_csi_record(
                &record,
                s_packet_buffer,
                sizeof(s_packet_buffer)
            );

        if (packet_length == 0) {
            s_format_failure_count++;

            if (
                (s_format_failure_count % 100U) ==
                1U
            ) {
                ESP_LOGW(
                    TAG,
                    "Binary serialization failed; failures=%" PRIu32,
                    s_format_failure_count
                );
            }

            continue;
        }

        const bool sent =
            send_udp_packet(
                s_packet_buffer,
                packet_length,
                &destination
            );

        if (!sent) {
            s_send_failure_count++;

            if (
                (s_send_failure_count % 100U) ==
                1U
            ) {
                ESP_LOGW(
                    TAG,
                    "UDP send failed: errno=%d, failures=%" PRIu32,
                    errno,
                    s_send_failure_count
                );
            }

            continue;
        }

        s_sent_count++;

        if ((s_sent_count % 500U) == 0U) {
            ESP_LOGI(
                TAG,
                "Sent=%" PRIu32
                ", send failures=%" PRIu32
                ", serialization failures=%" PRIu32,
                s_sent_count,
                s_send_failure_count,
                s_format_failure_count
            );
        }
    }
}


esp_err_t network_stream_start(void)
{
    if (s_network_task_handle != NULL) {
        ESP_LOGW(
            TAG,
            "Network stream is already running"
        );

        return ESP_ERR_INVALID_STATE;
    }

    s_sent_count = 0;
    s_send_failure_count = 0;
    s_format_failure_count = 0;

    const BaseType_t task_result =
        xTaskCreate(
            network_stream_task,
            "network_stream",
            6144,
            NULL,
            4,
            &s_network_task_handle
        );

    if (task_result != pdPASS) {
        s_network_task_handle = NULL;

        ESP_LOGE(
            TAG,
            "Failed to create network-stream task"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Network-stream task created"
    );

    return ESP_OK;
}


esp_err_t network_stream_stop(void)
{
    if (s_network_task_handle == NULL) {
        ESP_LOGW(
            TAG,
            "Network stream is not running"
        );

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Stop the task before closing the socket it may be using.
     */
    vTaskDelete(s_network_task_handle);
    s_network_task_handle = NULL;

    close_udp_socket();

    ESP_LOGI(
        TAG,
        "Network stream stopped"
    );

    return ESP_OK;
}


bool network_stream_is_running(void)
{
    return s_network_task_handle != NULL;
}


uint32_t network_stream_get_sent_count(void)
{
    return s_sent_count;
}


uint32_t network_stream_get_send_failure_count(void)
{
    return s_send_failure_count;
}


uint32_t network_stream_get_format_failure_count(void)
{
    return s_format_failure_count;
}
