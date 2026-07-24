cmake_minimum_required(VERSION 3.20)
project(famo_engine_api LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # /utf-8: source+exec charset UTF-8 (defensive; strings also use \x escapes).
  add_compile_options(/utf-8 /W4 /permissive-)
endif()

# ---------------------------------------------------------------------------
# Tier A: host loader + deterministic reference engine + roundtrip self-check.
# Builds standalone; no librime, no Weasel pin, never locks weaselx64.dll.
# ---------------------------------------------------------------------------

# Deterministic reference engine (no librime).
add_library(FamoTestEngine SHARED
  engines/test/famo_test_engine.cpp
  engines/test/FamoTestEngine.def)
target_include_directories(FamoTestEngine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Reusable host-side engine loader.
add_library(famo_engine_host STATIC host/famo_engine_host.cpp)
target_include_directories(famo_engine_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Roundtrip self-check executable.
add_executable(roundtrip_selfcheck tests/roundtrip_selfcheck.cpp)
target_link_libraries(roundtrip_selfcheck PRIVATE famo_engine_host)
add_dependencies(roundtrip_selfcheck FamoTestEngine)

# Place FamoTestEngine.dll next to the exe so LoadLibraryW resolves it by name.
add_custom_command(TARGET roundtrip_selfcheck POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:FamoTestEngine> $<TARGET_FILE_DIR:roundtrip_selfcheck>)

enable_testing()
add_test(NAME roundtrip COMMAND roundtrip_selfcheck)

# ---------------------------------------------------------------------------
# Tier B: FamoRimeEngine.dll - librime adapter for FamoEngineApi.
# Behind an option so Tier A stays librime-free; auto-skips if rime.lib absent.
# ---------------------------------------------------------------------------
set(FAMO_RIME_ROOT "C:/fb/weasel" CACHE PATH
    "librime prefix providing include/, lib64/rime.lib, output/rime.dll")
option(FAMO_BUILD_RIME_ENGINE "Build FamoRimeEngine.dll (needs librime)" ON)

if(FAMO_BUILD_RIME_ENGINE AND EXISTS "${FAMO_RIME_ROOT}/lib64/rime.lib")
  add_library(FamoRimeEngine SHARED
    engines/rime/famo_rime_engine.cpp
    engines/rime/FamoRimeEngine.def)
  target_include_directories(FamoRimeEngine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${FAMO_RIME_ROOT}/include")
  target_link_libraries(FamoRimeEngine PRIVATE "${FAMO_RIME_ROOT}/lib64/rime.lib")

  add_executable(rime_smoke tests/rime_smoke.cpp)
  target_link_libraries(rime_smoke PRIVATE famo_engine_host)
  add_dependencies(rime_smoke FamoRimeEngine)
  # FamoRimeEngine.dll + rime.dll must sit next to the smoke exe.
  add_custom_command(TARGET rime_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoRimeEngine> $<TARGET_FILE_DIR:rime_smoke>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FAMO_RIME_ROOT}/output/rime.dll" $<TARGET_FILE_DIR:rime_smoke>)
  add_test(NAME rime_smoke COMMAND rime_smoke)

  # Manual real-candidate verification tool (needs a deployed data root; not a
  # ctest). Usage: rime_verify <data_root> <schema_id> <ascii_keys>
  add_executable(rime_verify tests/rime_verify.cpp)
  target_link_libraries(rime_verify PRIVATE famo_engine_host)
  add_dependencies(rime_verify FamoRimeEngine)
  add_custom_command(TARGET rime_verify POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:FamoRimeEngine> $<TARGET_FILE_DIR:rime_verify>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FAMO_RIME_ROOT}/output/rime.dll" $<TARGET_FILE_DIR:rime_verify>)
else()
  message(STATUS "FamoRimeEngine skipped (no rime.lib at ${FAMO_RIME_ROOT})")
endif()
