#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Start the task that receives CSI records from csi_capture
 * and transmits them to the configured computer over UDP.
 *
 * Wi-Fi and CSI capture must already be running.
 */
esp_err_t network_stream_start(void);

/**
 * Stop the UDP streaming task and close its socket.
 */
esp_err_t network_stream_stop(void);

/**
 * Return true while the network-stream task exists.
 */
bool network_stream_is_running(void);

/**
 * Return the number of CSI records successfully sent over UDP.
 */
uint32_t network_stream_get_sent_count(void);

/**
 * Return the number of records that could not be transmitted.
 */
uint32_t network_stream_get_send_failure_count(void);

/**
 * Return the number of records that could not be formatted.
 */
uint32_t network_stream_get_format_failure_count(void);
