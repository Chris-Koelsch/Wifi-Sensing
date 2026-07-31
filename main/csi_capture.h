#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"

/*
* one self-contained CSI measurement.
* CSI callback copies all required info into this structure.
* nothing in this struct points back to temporary wifi driver memory
*/
typedef struct {
/* project generated record #
 */
	uint32_t sequence_number;

/* ms since the ESP timer began running
	*/
	int64_t timestamp_us;
/* MAC address of the packet source.
	uint8_t source_mac[6];
*/ 
	uint8_t source_mac[6];
/*
* Signal metadata supplied by the wifi driver
 */
	int8_t rssi;
	int8_t noise_floor;
	uint8_t channel;
/* length originally reported by the wifi driver.
	* */
	uint16_t original_length;
/*# of bytes actually copied into data
	*/
	uint16_t stored_length;
/* ESP32-S3 may report that its first 4 CSI bytes are invalid
	*/
	bool first_word_invalid;
/* True when original_length exceeded CONFIG_CSI_MAC_DATA_LENGTH.
	*/
	bool truncated;
/* signed CSI bytes. Espressif stores each complex value as an imaginary byte followed by a real one.
	*/
	int8_t data [CONFIG_CSI_MAX_DATA_LENGTH];
} csi_record_t;
/* Create the CSI queue, register callback, configure CSI, and enable CSI reception
* wifi must already be intitialized and started.
*/
esp_err_t csi_capture_start(void);
/* disable CSI reception and release the queue.
*/
esp_err_t csi_capture_stop(void);
/* Receive one copied CSI record 
* @param record destination for the copied queue item.
* @param timeout Max # of FreeRTOS ticks to wait.
*
* @return true when a record was received; otherwise false.
*/
bool csi_capture_receive(
	csi_record_t *record,
	TickType_t timeout
);
/* # of records successfully added to the queue.
*/
uint32_t csi_capture_get_received_count(void);
/* # of records discarded because the queue was full.
*/
uint32_t csi_capture_get_dropped_cound(void);


