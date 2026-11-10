idf_component_register(
    SRCS "app_main.c" "board.c" "gateway_client.c" "audio_player.c" "usb_diag.c"
    INCLUDE_DIRS "."
    EMBED_FILES "../fonts/icebreaker_text16.bin"
    REQUIRES driver esp_codec_dev esp_event esp_http_server esp_lcd esp_netif esp_timer esp_wifi nvs_flash esp_websocket_client json usb usb_host_uvc wpa_supplicant
)

# Opt-in bench build only; normal builds leave microphone behavior unchanged.
if(ICEBREAKER_TEXT_UI_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ICEBREAKER_TEXT_UI_TEST=1)
endif()
