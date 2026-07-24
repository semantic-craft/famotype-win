if(NOT DEFINED BENCHMARK_EXE OR NOT DEFINED FIXTURE_MANIFEST OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "benchmark matrix contract requires executable, fixtures and output dir")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
foreach(dpi IN ITEMS 96 144 192)
  set(dpi_output "${OUTPUT_DIR}/${dpi}dpi")
  file(MAKE_DIRECTORY "${dpi_output}")
  execute_process(
    COMMAND "${BENCHMARK_EXE}"
      --fixtures "${FIXTURE_MANIFEST}"
      --output "${dpi_output}"
      --iterations 1
      --dpi ${dpi}
      --all
      --all-skins
      --all-modes
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "${dpi} DPI matrix benchmark failed (${result})\nstdout: ${stdout}\nstderr: ${stderr}")
  endif()

  file(READ "${dpi_output}/benchmark-v1.json" benchmark_content)
  file(READ "${dpi_output}/captures-v1.json" captures_content)
  string(JSON matrix_count LENGTH "${benchmark_content}" matrix)
  string(JSON capture_count LENGTH "${captures_content}" captures)
  file(GLOB pngs "${dpi_output}/*.png")
  list(LENGTH pngs png_count)
  if(NOT matrix_count EQUAL 48 OR NOT capture_count EQUAL 48 OR
     NOT png_count EQUAL 48)
    message(FATAL_ERROR
      "${dpi} DPI expected 48 matrix entries/captures/pngs; got ${matrix_count}/${capture_count}/${png_count}")
  endif()

  foreach(index RANGE 0 47)
    string(JSON status GET "${benchmark_content}" matrix ${index} status)
    string(JSON visible_pixels GET "${benchmark_content}" matrix ${index} visiblePixelCount)
    string(JSON renderer GET "${captures_content}" captures ${index} renderer)
    if(NOT status STREQUAL "ok" OR visible_pixels LESS_EQUAL 0 OR
       NOT renderer STREQUAL "windows-current-native")
      message(FATAL_ERROR
        "${dpi} DPI matrix entry ${index} is not a successful current-renderer capture")
    endif()
  endforeach()
endforeach()
