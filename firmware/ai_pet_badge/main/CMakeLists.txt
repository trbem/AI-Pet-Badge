idf_component_register(
    SRCS "main.c" "badge_dashboard.c" "badge_network.c"
         "badge_ui_14.c" "badge_name_20.c" "badge_clock_30.c" "badge_icons_26.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer esp_rom esp_driver_usb_serial_jtag
             esp_wifi esp_event esp_netif esp_http_client esp-tls json lwip
             waveshare__esp32_s3_touch_lcd_1_85B waveshare__pcf85063a
    EMBED_FILES "animations.badge"
)
