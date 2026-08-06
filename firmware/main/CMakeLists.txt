idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  REQUIRES board app_core htp_client ui vendor_cjson esp_wifi esp_http_client
           nvs_flash fatfs sdmmc esp_adc driver)
