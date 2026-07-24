df_component_register(
	SRCS
		"app_main.c"
		"wifi_station.c"
		"packet_generator.c"
		"csi_capture.c"
		"serial_stream.c"

	INCLUDE_DIRS
		"."

	PRIV_REQUIRES
		esp_wifi
		esp_event
		esp_netif
		nvs_flash
		lwip
)
