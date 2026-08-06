if(ESP_PLATFORM)
  idf_component_register(
    SRCS "src/ids.c" "src/backoff.c" "src/client.c"
    INCLUDE_DIRS "include"
    REQUIRES vendor_cjson app_core)
endif()
