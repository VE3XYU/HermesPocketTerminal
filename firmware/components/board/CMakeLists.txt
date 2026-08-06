idf_component_register(
  SRCS "src/power.c" "src/buttons.c" "src/epd_ssd1681.c"
  INCLUDE_DIRS "include"
  REQUIRES driver esp_adc nvs_flash fatfs sdmmc)
