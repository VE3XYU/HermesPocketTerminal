if(ESP_PLATFORM)
  idf_component_register(
    SRCS "src/fb.c" "src/widgets.c"
    INCLUDE_DIRS "include" "fonts")
endif()
