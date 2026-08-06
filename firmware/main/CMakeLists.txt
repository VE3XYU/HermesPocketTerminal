idf_component_register(
  SRCS "main.c" "idf_ports.c"
  INCLUDE_DIRS "."
  REQUIRES board app_core htp_client ui vendor_cjson esp_wifi esp_http_client
           nvs_flash fatfs sdmmc esp_adc driver esp_timer esp_driver_usb_serial_jtag)
