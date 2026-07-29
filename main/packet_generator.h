#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"


esp_err_t packet_generator_start(esp_ip4_addr_t gateway);

esp_err_t packet_generator_stop(void);

bool packet_generator_is_running(void);

