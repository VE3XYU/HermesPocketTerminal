idf_component_register(
  SRCS "src/power.c" "src/buttons.c"
  INCLUDE_DIRS "include"
  REQUIRES driver esp_adc nvs_flash fatfs sdmmc)
