if(NOT DEFINED BENCHMARK_EXE OR NOT DEFINED FIXTURE_MANIFEST OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "benchmark output guard requires executable, fixtures and output dir")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/user-owned-sentinel.txt" "preserve me")
execute_process(
  COMMAND "${BENCHMARK_EXE}"
    --fixtures "${FIXTURE_MANIFEST}"
    --output "${OUTPUT_DIR}"
    --iterations 1
    --dpi 96
    --fixture compact-selected
    --skin shenda
    --mode light
  RESULT_VARIABLE result
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 4)
  message(FATAL_ERROR "non-empty output must fail with 4; got ${result}: ${stderr}")
endif()
if(NOT EXISTS "${OUTPUT_DIR}/user-owned-sentinel.txt" OR
   EXISTS "${OUTPUT_DIR}/benchmark-v1.json")
  message(FATAL_ERROR "output guard overwrote a non-empty directory")
endif()
