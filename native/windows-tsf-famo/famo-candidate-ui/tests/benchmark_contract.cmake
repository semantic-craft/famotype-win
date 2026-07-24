if(NOT DEFINED BENCHMARK_EXE OR NOT DEFINED FIXTURE_MANIFEST OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "benchmark contract requires executable, fixture manifest and output dir")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
  COMMAND "${BENCHMARK_EXE}"
    --fixtures "${FIXTURE_MANIFEST}"
    --output "${OUTPUT_DIR}"
    --iterations 3
    --dpi 96
    --fixture compact-selected
    --skin shenda
    --mode light
  RESULT_VARIABLE benchmark_result
  OUTPUT_VARIABLE benchmark_stdout
  ERROR_VARIABLE benchmark_stderr)

if(NOT benchmark_result EQUAL 0)
  message(FATAL_ERROR
    "benchmark failed (${benchmark_result})\nstdout: ${benchmark_stdout}\nstderr: ${benchmark_stderr}")
endif()

set(benchmark_json "${OUTPUT_DIR}/benchmark-v1.json")
set(captures_json "${OUTPUT_DIR}/captures-v1.json")
set(capture_png "${OUTPUT_DIR}/compact-selected__shenda__light__96dpi.png")
foreach(required IN ITEMS "${benchmark_json}" "${captures_json}" "${capture_png}")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "missing benchmark artifact: ${required}")
  endif()
endforeach()

file(READ "${benchmark_json}" benchmark_content)
string(JSON schema_version GET "${benchmark_content}" schemaVersion)
string(JSON renderer GET "${benchmark_content}" renderer)
string(JSON host GET "${benchmark_content}" host)
string(JSON git_dirty ERROR_VARIABLE git_dirty_error GET "${benchmark_content}" gitDirty)
string(JSON matrix_count LENGTH "${benchmark_content}" matrix)
if(NOT schema_version EQUAL 1 OR NOT renderer STREQUAL "windows-current-native" OR
   NOT host STREQUAL "synthetic-layered-hwnd" OR git_dirty_error OR
   NOT matrix_count EQUAL 1)
  message(FATAL_ERROR "benchmark metadata contract mismatch")
endif()

string(JSON visible_pixels GET "${benchmark_content}" matrix 0 visiblePixelCount)
if(visible_pixels LESS_EQUAL 0)
  message(FATAL_ERROR "capture must contain at least one visible pixel")
endif()

foreach(stage IN ITEMS snapshot_prepare layout paint window_submit window_move total)
  foreach(metric IN ITEMS count p50Us p95Us p99Us maxUs)
    string(JSON value ERROR_VARIABLE json_error
      GET "${benchmark_content}" matrix 0 warm ${stage} ${metric})
    if(json_error)
      message(FATAL_ERROR "missing warm.${stage}.${metric}: ${json_error}")
    endif()
  endforeach()
endforeach()

foreach(resource_path IN ITEMS
    "gdiObjects before" "gdiObjects after" "gdiObjects delta"
    "workingSetBytes before" "workingSetBytes after"
    "workingSetBytes peakAfter" "workingSetBytes delta" "workingSetBytes peakDelta"
    "creates hostSurface" "creates textSurface" "creates d2dTarget"
    "creates brush" "creates textLayout")
  string(REPLACE " " ";" keys "${resource_path}")
  string(JSON value ERROR_VARIABLE json_error
    GET "${benchmark_content}" matrix 0 resources ${keys})
  if(json_error)
    message(FATAL_ERROR "missing resources.${resource_path}: ${json_error}")
  endif()
endforeach()

file(READ "${captures_json}" captures_content)
string(JSON capture_schema GET "${captures_content}" schemaVersion)
string(JSON capture_count LENGTH "${captures_content}" captures)
string(JSON capability GET "${captures_content}" captures 0 capabilities sourceProvenance)
if(NOT capture_schema EQUAL 1 OR NOT capture_count EQUAL 1 OR
   NOT capability STREQUAL "unsupported-current")
  message(FATAL_ERROR "capture manifest contract mismatch")
endif()
