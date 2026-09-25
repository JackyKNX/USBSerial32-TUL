idf_component_register(
    SRCS
        "serial_bridge.c"
        "probe.c"
        "web_manager.c"
        "mqtt_manager.c"

    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_driver_gpio
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        app_update
        nvs_flash
        esp_partition
        esp_system
        mqtt
)

# Produkt kommt von aussen: build.sh uebergibt -DBW_PRODUCT=TUL bzw. EUL.
if(NOT DEFINED BW_PRODUCT)
    message(FATAL_ERROR
        "BW_PRODUCT nicht gesetzt. Mit build.sh bauen, oder "
        "idf.py -DBW_PRODUCT=TUL ... bzw. -DBW_PRODUCT=EUL ...")
endif()

if(NOT DEFINED BW_VERSION)
    set(BW_VERSION "0.0.0")
endif()

if(NOT DEFINED BW_QUIET)
    set(BW_QUIET 0)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BUSWARE_${BW_PRODUCT}=1
    BW_VERSION="${BW_VERSION}"
    BW_QUIET=${BW_QUIET})
