idf_component_register(
    SRCS "app_main.c" "board.c" "gateway_client.c" "audio_player.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_codec_dev esp_event esp_http_server esp_lcd esp_netif esp_timer esp_wifi nvs_flash esp_websocket_client json wpa_supplicant
)
