if(ESP_PLATFORM)
  idf_component_register(
    SRCS "src/util.c" "src/wav.c" "src/app_config.c" "src/wifi_select.c"
         "src/sidecar.c" "src/rec_index.c" "src/capture_flow.c" "src/sync.c"
         "src/gesture.c" "src/ui_flow.c"
    INCLUDE_DIRS "include"
    REQUIRES vendor_cjson htp_client ui)
endif()
