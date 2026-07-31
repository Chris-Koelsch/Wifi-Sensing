# pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 
* Start task that receives copied CSI records and wrights them to the serial console.
*/
esp_err_t serial_stream_start(void);

esp_err_t serial_stream_stop(void);

bool serial_stream_is_running(void);
