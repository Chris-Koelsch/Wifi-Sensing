idf_component_register(
	SRCS
		"app_main.c"
		"csi_capture.c"
		"wifi_station.c"
		"packet_generator.c"
		"network_stream.c"

	INCLUDE_DIRS
		"."

	PRIV_REQUIRES
		esp_wifi
		esp_event
		esp_netif
		nvs_flash
		esp_timer
		lwip
)
