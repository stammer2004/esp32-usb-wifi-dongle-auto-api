set(srcs)

list(APPEND srcs
    "usb_dongle_main.c"
    "CLI_Commands.c"
    "Command_Parse.c"
    "cmd_wifi.c"
    "data_back.c"
    "wifi_api.c"
    "usb_config_netif.c"
    "boot_button.c"
    )

if(CONFIG_UART_ENABLE)
    list(APPEND srcs
        "uart.c")
endif() # CONFIG_UART_ENABLE

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS .
                       PRIV_REQUIRES
                           nvs_flash
                           freertos
                           esp_wifi
                           esp_netif
                           esp_event
                           esp_system
                           esp_hw_support
                           lwip
                           spi_flash
                           driver
                           esp_http_server
                           json
                           FreeRTOS-Plus-CLI
                           tinyusb_dongle
                           led_strip
                           wpa_supplicant
                       )
