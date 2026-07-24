cmake_minimum_required(VERSION 3.20)
project(famo_candidate_ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # Same flags as engine-api: /utf-8 source+exec charset, warnings, conformance.
  add_compile_options(/utf-8 /W3 /permissive-)
endif()

# ---------------------------------------------------------------------------
# FamoCandidateUI — clean-room candidate-window component (M2).
#
# Static lib = the HEADLESS layout engine + compiled-in skin. Pure math: no HDC,
# no DirectWrite/D2D/GDI+ CALLS. No librime, no upstream IME, no Boost. Renders
# from the neutral FamoCompositionView (engine-api) + our own FamoSkin, so no
# upstream lib is linked and the per-app text-service DLL is never locked.
# ---------------------------------------------------------------------------
add_library(famo_candidate_ui STATIC
  layout/famo_layout.cpp
  skin/famo_skin.cpp)
target_include_directories(famo_candidate_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The layout core stays headless (no <windows.h>, no device); the B5 paint TU +
# its device libs are confined to the bitmap_smoke target below, so this lib and
# geometry_selfcheck link nothing extra.

# Headless geometry self-check (deterministic, like roundtrip_selfcheck).
add_executable(geometry_selfcheck tests/geometry_selfcheck.cpp)
target_link_libraries(geometry_selfcheck PRIVATE famo_candidate_ui)

# Candidate Window V2 shared-fixture contract. The loader belongs to benchmark/
# and is deliberately not linked into the production/headless renderer library.
add_executable(fixture_contract
  tests/fixture_contract.cpp
  benchmark/famo_fixture.cpp)
target_link_libraries(fixture_contract PRIVATE famo_candidate_ui)
get_filename_component(FAMO_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
target_compile_definitions(fixture_contract PRIVATE
  FAMO_FIXTURE_MANIFEST_PATH="${FAMO_REPO_ROOT}/design/candidate-window-v2/fixtures-v1.tsv")

# ---------------------------------------------------------------------------
# B5 bitmap smoke — real D2D/DWrite/GDI+ paint into a DIB-backed memDC + pixel
# readback (no window). Compiles the paint TU here and links the device libs so
# they stay off the headless lib. On the target machine the same render/*.cpp is
# added to the WeaselUI .vcxproj by candidate-ui.patch (Phase C).
# ---------------------------------------------------------------------------
add_executable(bitmap_smoke tests/bitmap_smoke.cpp render/famo_paint.cpp)
target_link_libraries(bitmap_smoke PRIVATE famo_candidate_ui
  d2d1 dwrite gdiplus gdi32 user32 ole32)

# Candidate Window V2 Release x64 baseline executable. This reuses the real
# layout/paint source but keeps fixture parsing, diagnostics and synthetic HWND
# hosting outside the production renderer library/Weasel process.
add_executable(candidate_window_benchmark
  benchmark/benchmark_main.cpp
  benchmark/famo_benchmark.cpp
  benchmark/famo_benchmark_fixture.cpp
  benchmark/famo_benchmark_frame.cpp
  benchmark/famo_benchmark_report.cpp
  benchmark/famo_benchmark_surface.cpp
  benchmark/famo_fixture.cpp
  render/famo_paint.cpp)
target_compile_definitions(candidate_window_benchmark PRIVATE
  FAMO_CANDIDATE_UI_BENCHMARK_COUNTERS=1)
target_link_libraries(candidate_window_benchmark PRIVATE famo_candidate_ui
  d2d1 dwrite gdiplus gdi32 user32 ole32 psapi)
execute_process(
  COMMAND git -C ${FAMO_REPO_ROOT} rev-parse --short=12 HEAD
  OUTPUT_VARIABLE FAMO_BENCHMARK_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT FAMO_BENCHMARK_GIT_COMMIT)
  set(FAMO_BENCHMARK_GIT_COMMIT unknown)
endif()
execute_process(
  COMMAND git -C ${FAMO_REPO_ROOT} status --porcelain --untracked-files=normal
  OUTPUT_VARIABLE FAMO_BENCHMARK_GIT_STATUS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(FAMO_BENCHMARK_GIT_STATUS)
  set(FAMO_BENCHMARK_GIT_DIRTY 1)
else()
  set(FAMO_BENCHMARK_GIT_DIRTY 0)
endif()
target_compile_definitions(candidate_window_benchmark PRIVATE
  FAMO_BENCHMARK_GIT_COMMIT="${FAMO_BENCHMARK_GIT_COMMIT}"
  FAMO_BENCHMARK_GIT_DIRTY=${FAMO_BENCHMARK_GIT_DIRTY})

enable_testing()
add_test(NAME geometry COMMAND geometry_selfcheck)
add_test(NAME bitmap COMMAND bitmap_smoke)
add_test(NAME fixtures COMMAND fixture_contract)
add_test(NAME benchmark_contract
  COMMAND ${CMAKE_COMMAND}
    -DBENCHMARK_EXE=$<TARGET_FILE:candidate_window_benchmark>
    -DFIXTURE_MANIFEST=${FAMO_REPO_ROOT}/design/candidate-window-v2/fixtures-v1.tsv
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/benchmark-contract-output
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_contract.cmake)
add_test(NAME benchmark_matrix_contract
  COMMAND ${CMAKE_COMMAND}
    -DBENCHMARK_EXE=$<TARGET_FILE:candidate_window_benchmark>
    -DFIXTURE_MANIFEST=${FAMO_REPO_ROOT}/design/candidate-window-v2/fixtures-v1.tsv
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/benchmark-matrix-contract-output
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_matrix_contract.cmake)
add_test(NAME benchmark_output_guard_contract
  COMMAND ${CMAKE_COMMAND}
    -DBENCHMARK_EXE=$<TARGET_FILE:candidate_window_benchmark>
    -DFIXTURE_MANIFEST=${FAMO_REPO_ROOT}/design/candidate-window-v2/fixtures-v1.tsv
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/benchmark-output-guard
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_output_guard_contract.cmake)
add_test(NAME benchmark_renderer_failure_contract
  COMMAND ${CMAKE_COMMAND}
    -DBENCHMARK_EXE=$<TARGET_FILE:candidate_window_benchmark>
    -DFIXTURE_MANIFEST=${FAMO_REPO_ROOT}/design/candidate-window-v2/fixtures-v1.tsv
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/benchmark-renderer-failure
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_renderer_failure_contract.cmake)
