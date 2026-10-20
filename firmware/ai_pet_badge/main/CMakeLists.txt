idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer esp_rom esp_driver_usb_serial_jtag
             waveshare__esp32_s3_touch_lcd_1_85B
    EMBED_FILES "animations.badge"
)
