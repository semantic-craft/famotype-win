cmake_minimum_required(VERSION 3.20)
project(famo_text_service LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "FamoTextService requires the Windows SDK")
endif()

set(FAMO_IDENTITY "Development" CACHE STRING
    "Compile-time identity set: Development or Stable")
set_property(CACHE FAMO_IDENTITY PROPERTY STRINGS Development Stable)
if(FAMO_IDENTITY STREQUAL "Stable")
  add_compile_definitions(FAMO_STABLE_IDENTITY=1)
elseif(NOT FAMO_IDENTITY STREQUAL "Development")
  message(FATAL_ERROR "FAMO_IDENTITY must be Development or Stable")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime-protocol
                 ${CMAKE_CURRENT_BINARY_DIR}/runtime-protocol)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
endif()

add_library(famo_tsf_host_model STATIC
  src/commit_behavior.cpp
  src/host_model.cpp
  src/input_injection.cpp
  src/utf_conversion.cpp)
target_include_directories(famo_tsf_host_model PUBLIC include)
target_compile_definitions(famo_tsf_host_model PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(famo_tsf_host_model PUBLIC famo_runtime_protocol)

add_executable(host_model_selfcheck tests/host_model_selfcheck.cpp)
target_link_libraries(host_model_selfcheck PRIVATE famo_tsf_host_model)

add_executable(commit_behavior_selfcheck tests/commit_behavior_selfcheck.cpp)
target_link_libraries(commit_behavior_selfcheck PRIVATE famo_tsf_host_model)

add_executable(candidate_ui_selfcheck
  tests/candidate_ui_selfcheck.cpp
  src/candidate_ui_element.cpp
  src/module_state.cpp)
target_include_directories(candidate_ui_selfcheck PRIVATE include src)
target_compile_definitions(candidate_ui_selfcheck PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(candidate_ui_selfcheck PRIVATE
  famo_tsf_host_model oleaut32)

add_library(FamoTextService SHARED
  src/candidate_ui_element.cpp
  src/composition_controller.cpp
  src/dll_module.cpp
  src/module_state.cpp
  src/registration.cpp
  src/text_service.cpp
  src/text_service_key.cpp
  src/text_service_ui.cpp
  src/FamoTextService.def)
target_include_directories(FamoTextService PRIVATE include src)
target_compile_definitions(FamoTextService PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(FamoTextService PRIVATE
  famo_tsf_host_model famo_runtime_core ole32 oleaut32 advapi32 bcrypt)
set_target_properties(FamoTextService PROPERTIES PREFIX "")
add_dependencies(FamoTextService FamoTestRuntime FamoTestEngine)
add_custom_command(TARGET FamoTextService POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTestRuntime> $<TARGET_FILE_DIR:FamoTextService>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTestEngine> $<TARGET_FILE_DIR:FamoTextService>)
if(TARGET FamoRuntime)
  add_dependencies(FamoTextService FamoRuntime)
  add_custom_command(TARGET FamoTextService POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoRuntime> $<TARGET_FILE_DIR:FamoTextService>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoRimeEngine> $<TARGET_FILE_DIR:FamoTextService>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FAMO_RIME_ROOT}/output/rime.dll" $<TARGET_FILE_DIR:FamoTextService>)
endif()

if(FAMO_IDENTITY STREQUAL "Stable")
  set(FAMO_PROFILE_TOOL FamoProfileTool)
else()
  set(FAMO_PROFILE_TOOL FamoDevProfile)
endif()
add_executable(${FAMO_PROFILE_TOOL} tools/dev_profile_main.cpp)
target_include_directories(${FAMO_PROFILE_TOOL} PRIVATE
  src ${CMAKE_CURRENT_SOURCE_DIR}/../runtime-protocol/include)
target_compile_definitions(${FAMO_PROFILE_TOOL} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(${FAMO_PROFILE_TOOL} PRIVATE
  ole32 advapi32 Rstrtmgr shell32 ntdll)
add_dependencies(${FAMO_PROFILE_TOOL} FamoTextService)
add_custom_command(TARGET ${FAMO_PROFILE_TOOL} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTextService> $<TARGET_FILE_DIR:${FAMO_PROFILE_TOOL}>)

add_executable(module_selfcheck tests/module_selfcheck.cpp)
target_include_directories(module_selfcheck PRIVATE
  src ${CMAKE_CURRENT_SOURCE_DIR}/../runtime-protocol/include)
target_compile_definitions(module_selfcheck PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(module_selfcheck PRIVATE ole32)
add_dependencies(module_selfcheck FamoTextService ${FAMO_PROFILE_TOOL})

add_executable(identity_broker_selfcheck
  tests/identity_broker_selfcheck.cpp)
target_compile_definitions(identity_broker_selfcheck PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(identity_broker_selfcheck PRIVATE ole32)
add_dependencies(identity_broker_selfcheck ${FAMO_PROFILE_TOOL})

add_executable(user_data_cleanup_selfcheck
  tests/user_data_cleanup_selfcheck.cpp)
target_include_directories(user_data_cleanup_selfcheck PRIVATE
  src ${CMAKE_CURRENT_SOURCE_DIR}/../runtime-protocol/include)
target_compile_definitions(user_data_cleanup_selfcheck PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(user_data_cleanup_selfcheck PRIVATE
  ole32 advapi32 Rstrtmgr shell32 ntdll)

add_executable(tsf_integration_selfcheck
  tests/fake_text_store.cpp
  tests/tsf_integration_support.cpp
  tests/tsf_integration_selfcheck.cpp)
target_include_directories(tsf_integration_selfcheck PRIVATE include src tests)
target_compile_definitions(tsf_integration_selfcheck PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(tsf_integration_selfcheck PRIVATE
  famo_runtime_core ole32 oleaut32)
add_dependencies(tsf_integration_selfcheck FamoTextService FamoTestRuntime)

enable_testing()
add_test(NAME host_model_selfcheck COMMAND host_model_selfcheck)
add_test(NAME commit_behavior_selfcheck COMMAND commit_behavior_selfcheck)
add_test(NAME candidate_ui_selfcheck COMMAND candidate_ui_selfcheck)
add_test(NAME module_selfcheck
  COMMAND module_selfcheck $<TARGET_FILE:FamoTextService>
                           $<TARGET_FILE:${FAMO_PROFILE_TOOL}>)
add_test(NAME identity_broker_selfcheck
  COMMAND identity_broker_selfcheck $<TARGET_FILE:${FAMO_PROFILE_TOOL}>)
set_tests_properties(identity_broker_selfcheck PROPERTIES TIMEOUT 70)
add_test(NAME user_data_cleanup_selfcheck
  COMMAND user_data_cleanup_selfcheck)
add_test(NAME tsf_integration_selfcheck
  COMMAND tsf_integration_selfcheck
    $<TARGET_FILE:FamoTextService>
    $<TARGET_FILE_DIR:FamoTextService>/FamoTestRuntime.exe)
set_tests_properties(tsf_integration_selfcheck PROPERTIES TIMEOUT 20)
