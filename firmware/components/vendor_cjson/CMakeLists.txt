if(ESP_PLATFORM)
  idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")
endif()
