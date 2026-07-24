cmake_minimum_required(VERSION 3.20)
project(famo_runtime_protocol LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../engine-api
                 ${CMAKE_CURRENT_BINARY_DIR}/engine-api)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
endif()

add_library(famo_runtime_protocol STATIC
  src/protocol.cpp
  src/protocol_control.cpp
  src/utf8.cpp)
target_include_directories(famo_runtime_protocol PUBLIC include)

add_library(famo_runtime_core STATIC
  src/runtime_state.cpp
  src/runtime_service.cpp
  src/runtime_dispatch.cpp
  src/runtime_control_engine.cpp
  src/runtime_style_config.cpp
  src/runtime_control.cpp
  src/runtime_control_client.cpp
  src/pipe_security.cpp
  src/pipe_io.cpp
  src/runtime_pipe.cpp
  src/runtime_server.cpp)
target_include_directories(famo_runtime_core PUBLIC include)
target_compile_definitions(famo_runtime_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(famo_runtime_core PUBLIC
  famo_runtime_protocol famo_engine_host advapi32)

add_executable(protocol_selfcheck tests/protocol_selfcheck.cpp)
target_link_libraries(protocol_selfcheck PRIVATE famo_runtime_protocol)

add_executable(runtime_state_selfcheck tests/runtime_state_selfcheck.cpp)
target_link_libraries(runtime_state_selfcheck PRIVATE famo_runtime_core)

add_executable(runtime_service_selfcheck tests/runtime_service_selfcheck.cpp)
target_link_libraries(runtime_service_selfcheck PRIVATE famo_runtime_core)
add_dependencies(runtime_service_selfcheck FamoTestEngine)
add_custom_command(TARGET runtime_service_selfcheck POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTestEngine> $<TARGET_FILE_DIR:runtime_service_selfcheck>)

add_executable(FamoTestRuntime src/test_runtime_main.cpp)
target_link_libraries(FamoTestRuntime PRIVATE famo_runtime_core)
add_dependencies(FamoTestRuntime FamoTestEngine)
add_custom_command(TARGET FamoTestRuntime POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTestEngine> $<TARGET_FILE_DIR:FamoTestRuntime>)

add_executable(pipe_roundtrip_selfcheck tests/pipe_roundtrip_selfcheck.cpp)
target_link_libraries(pipe_roundtrip_selfcheck PRIVATE famo_runtime_core)
add_dependencies(pipe_roundtrip_selfcheck FamoTestRuntime)

add_executable(control_roundtrip_selfcheck tests/control_roundtrip_selfcheck.cpp)
target_link_libraries(control_roundtrip_selfcheck PRIVATE famo_runtime_core)
add_dependencies(control_roundtrip_selfcheck FamoTestEngine)
add_custom_command(TARGET control_roundtrip_selfcheck POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTestEngine> $<TARGET_FILE_DIR:control_roundtrip_selfcheck>)

enable_testing()
add_test(NAME protocol_selfcheck COMMAND protocol_selfcheck)
add_test(NAME runtime_state_selfcheck COMMAND runtime_state_selfcheck)
add_test(NAME runtime_service_selfcheck COMMAND runtime_service_selfcheck)
add_test(NAME pipe_roundtrip_selfcheck COMMAND pipe_roundtrip_selfcheck)
add_test(NAME control_roundtrip_selfcheck COMMAND control_roundtrip_selfcheck)
set_tests_properties(pipe_roundtrip_selfcheck PROPERTIES TIMEOUT 30)
set_tests_properties(control_roundtrip_selfcheck PROPERTIES TIMEOUT 10)

if(TARGET FamoRimeEngine)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../famo-candidate-ui
                   ${CMAKE_CURRENT_BINARY_DIR}/famo-candidate-ui)
  add_library(famo_candidate_window STATIC
    src/candidate_window.cpp
    src/candidate_skin.cpp
    src/status_ui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../famo-candidate-ui/render/famo_paint.cpp)
  target_include_directories(famo_candidate_window PUBLIC src)
  target_compile_features(famo_candidate_window PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(famo_candidate_window PRIVATE /EHsc)
  endif()
  target_link_libraries(famo_candidate_window PUBLIC
    famo_runtime_core famo_candidate_ui
    d2d1 dwrite gdiplus gdi32 user32 ole32 shell32)

  add_executable(FamoRuntime src/runtime_main.cpp)
  target_link_libraries(FamoRuntime PRIVATE famo_candidate_window)
  add_dependencies(FamoRuntime FamoRimeEngine)
  add_custom_command(TARGET FamoRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoRimeEngine> $<TARGET_FILE_DIR:FamoRuntime>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FAMO_RIME_ROOT}/output/rime.dll" $<TARGET_FILE_DIR:FamoRuntime>)

  # Manual process-level verification against a deployed, real Rime data root.
  # This intentionally is not a CTest because it consumes caller-owned data.
  add_executable(production_runtime_verify tests/production_runtime_verify.cpp)
  target_link_libraries(production_runtime_verify PRIVATE famo_runtime_core)
  add_dependencies(production_runtime_verify FamoRuntime)

  add_executable(candidate_window_selfcheck tests/candidate_window_selfcheck.cpp)
  target_link_libraries(candidate_window_selfcheck PRIVATE famo_candidate_window)
  add_dependencies(candidate_window_selfcheck FamoTestEngine)
  add_custom_command(TARGET candidate_window_selfcheck POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoTestEngine>
      $<TARGET_FILE_DIR:candidate_window_selfcheck>)
  add_test(NAME candidate_window_selfcheck COMMAND candidate_window_selfcheck)
  set_tests_properties(candidate_window_selfcheck PROPERTIES TIMEOUT 60)

  add_executable(status_ui_selfcheck tests/status_ui_selfcheck.cpp)
  target_link_libraries(status_ui_selfcheck PRIVATE famo_candidate_window)
  add_dependencies(status_ui_selfcheck FamoTestEngine)
  add_custom_command(TARGET status_ui_selfcheck POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoTestEngine> $<TARGET_FILE_DIR:status_ui_selfcheck>)
  add_test(NAME status_ui_selfcheck COMMAND status_ui_selfcheck)
  set_tests_properties(status_ui_selfcheck PROPERTIES TIMEOUT 60)

  add_executable(candidate_skin_selfcheck tests/candidate_skin_selfcheck.cpp)
  target_link_libraries(candidate_skin_selfcheck PRIVATE famo_candidate_window)
  add_test(NAME candidate_skin_selfcheck COMMAND candidate_skin_selfcheck)

  add_test(NAME runtime_rejects_shared_endpoint
    COMMAND FamoRuntime --endpoint-suffix shared --control-endpoint-suffix shared)
  set_tests_properties(runtime_rejects_shared_endpoint PROPERTIES
    WILL_FAIL TRUE TIMEOUT 5)
endif()
