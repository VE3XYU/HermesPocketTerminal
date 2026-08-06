cmake_minimum_required(VERSION 3.16)
project(htp_host_tests C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -g)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(FIXDIR ${FW}/../bridge/tests/fixtures/contract)

add_library(vendor_cjson ${FW}/components/vendor/cjson/cJSON.c)
target_include_directories(vendor_cjson PUBLIC ${FW}/components/vendor/cjson)

# Component source lists grow as tasks add files; keep them explicit.
add_library(fw_components
  ${FW}/components/htp_client/src/ids.c
  ${FW}/components/htp_client/src/backoff.c
  ${FW}/components/htp_client/src/client.c
  ${FW}/components/app_core/src/util.c
  ${FW}/components/app_core/src/wav.c
  ${FW}/components/app_core/src/app_config.c
  ${FW}/components/app_core/src/wifi_select.c
  ${FW}/components/app_core/src/sidecar.c
  ${FW}/components/app_core/src/rec_index.c
  ${FW}/components/app_core/src/capture_flow.c
  ${FW}/components/app_core/src/sync.c
)
target_include_directories(fw_components PUBLIC
  ${FW}/components/htp_client/include
  ${FW}/components/app_core/include
  ${FW}/components/ui/include
  ${FW}/components/ui/fonts
)
target_link_libraries(fw_components PUBLIC vendor_cjson)

enable_testing()
set(FAKES fakes/fake_transport.c fakes/fake_kv.c fakes/fake_storage.c)
function(host_test name)
  add_executable(${name} ${name}.c ${FAKES})
  target_link_libraries(${name} fw_components)
  target_compile_definitions(${name} PRIVATE FIXDIR="${FIXDIR}")
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

host_test(test_ids)
host_test(test_backoff)
host_test(test_wav)
host_test(test_client)
host_test(test_config)
host_test(test_wifi_select)
host_test(test_sidecar)
host_test(test_rec_index)
host_test(test_capture_flow)
host_test(test_sync)
