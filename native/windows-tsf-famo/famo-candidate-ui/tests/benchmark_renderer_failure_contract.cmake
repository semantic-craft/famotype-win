if(NOT DEFINED BENCHMARK_EXE OR NOT DEFINED FIXTURE_MANIFEST OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "renderer failure contract requires executable, fixtures and output dir")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env FAMO_BENCHMARK_FORCE_RENDERER_FAILURE=1
    "${BENCHMARK_EXE}"
    --fixtures "${FIXTURE_MANIFEST}"
    --output "${OUTPUT_DIR}"
    --iterations 1
    --dpi 96
    --fixture compact-selected
    --skin shenda
    --mode light
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 3)
  message(FATAL_ERROR
    "forced renderer failure must exit 3; got ${result}\nstdout: ${stdout}\nstderr: ${stderr}")
endif()

set(benchmark_json "${OUTPUT_DIR}/benchmark-v1.json")
set(captures_json "${OUTPUT_DIR}/captures-v1.json")
if(NOT EXISTS "${benchmark_json}" OR NOT EXISTS "${captures_json}")
  message(FATAL_ERROR "renderer failure must retain JSON diagnostics")
endif()
file(READ "${benchmark_json}" benchmark_content)
file(READ "${captures_json}" captures_content)
string(JSON matrix_count LENGTH "${benchmark_content}" matrix)
string(JSON status GET "${benchmark_content}" matrix 0 status)
string(JSON error_message GET "${benchmark_content}" matrix 0 error)
string(JSON capture_count LENGTH "${captures_content}" captures)
file(GLOB pngs "${OUTPUT_DIR}/*.png")
list(LENGTH pngs png_count)
if(NOT matrix_count EQUAL 1 OR NOT status STREQUAL "error" OR
   NOT error_message STREQUAL "forced renderer failure" OR
   NOT capture_count EQUAL 0 OR NOT png_count EQUAL 0)
  message(FATAL_ERROR "renderer failure JSON contract mismatch")
endif()
