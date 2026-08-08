idf_component_register(
  SRCS "main.c" "idf_ports.c" "idf_wifi.c" "idf_transport.c"
  INCLUDE_DIRS "."
  REQUIRES board app_core htp_client ui vendor_cjson esp_wifi esp_http_client
           esp_netif esp_event mbedtls
           nvs_flash fatfs sdmmc esp_adc driver esp_timer esp_driver_usb_serial_jtag
           esp_app_format)
