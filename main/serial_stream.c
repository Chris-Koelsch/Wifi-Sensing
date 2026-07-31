#include "serial_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "csi_capture.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos.task.h"


static const char *TAG = "serial_stream";


static TaskHandle_t s_serial_task_handle = NULL;


static void print_csi_record(
	const csi_record_t *record
)
{
	if (record == NULL) {
		return;
	}

	printf(
		"CSI_DATA,"
		"%"PRIu32","
		"%"PRId64","
		"%d,"
		"%d,"
		"%u,"
		"%02X:%02X:%02X:%02X:%02X:%02X"
		"%u,"
		"%u,"
		"%u,"
		"%u",
		record->sequence_number,
		record->timestamp_us,
		(int)record->rssi,
		(int)record->noise_floor,
		(unsigned)record->channel,
		record->source_mac[0],
		record->source_mac[1],
		record->source_mac[2],
		record->source_mac[3],
		record->source_mac[4],
		record->source_mac[5],
		(unsigned)record->original_length,
		(unsigned)record->stored_length,
		record->first_word_invalid ? 1U : 0U,
		record->truncated ? 1U : 0U
	);


	for (
		uint16_t index = 0;
		index < record->stored_length;
		index++
	) {
		printf(
			",%d",
			(int)record->data[index]
		);
	}

	putchar('\n');
	
	fflush(stdout);
}

static void serial_stream_task(
	void *task_argument
)
{
	(void)task_argument;

	ESP_LOGI(
		TAG,
		"serial-stream task started"
	);

	csi_record_t record;

	while (true) {
		const bool received =
		csi_capture_receive(
				&record,
			portMAX_DELAY
			);
		if (received) {
			print_csi_record(&record);
		}
	}
}

esp_err_t serial_stream_start(void)
{
	if (s_serial_task_handle != NULL) {
		ESP_LOGW(
			TAG,
			"serial-stream task is already running"
		);

		return ESP_ERR_INVALID_STATE;
	}

	const BaseType_t task_result =
	xTaskCreate(
		serial_stream_task,
		"serial_stream",
		6144,
		NULL,
		4,
		&s_serial_task_handle
		);
	if (task_result != pdPASS) {
		s_serial_task_handle = NULL;

		ESP_LOGE(
			TAG,
			"failed to create serial-stream task"
		);
		return ESP_ERR_NO_MEM;
	}

	ESP_LOGI(
		TAG,
		"serial-stream task created"
	);
	return ESP_OK;
}

esp_err_t serial_stream_stop(void)
{
	if (s_serial_task_handle == NULL) {
		ESP_LOGW(
			TAG,
			"serial-stream task is not running"
		);
		return ESP_ERR_INVALID_STATE;
	}

	vTaskDelete(s_serial_task_handle);
	s_serial_task_handle = NULL;

	ESP_LOGI(
		TAG,
		"serial stream task stopped"
	);
	return ESP_OK;
}

bool serial_stream_is_running(void)
{
	return s_serial_task_handle != NULL;
}

