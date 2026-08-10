if(NOT DEFINED DUMPBIN OR DUMPBIN STREQUAL "" OR NOT EXISTS "${DUMPBIN}")
  message(FATAL_ERROR "CMAKE_DUMPBIN is unavailable; cannot verify ${BINARY}")
endif()
if(NOT DEFINED BINARY OR BINARY STREQUAL "" OR NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "native payload is unavailable: ${BINARY}")
endif()

execute_process(
  COMMAND "${DUMPBIN}" /DEPENDENTS "${BINARY}"
  RESULT_VARIABLE dumpbin_result
  OUTPUT_VARIABLE dependencies
  ERROR_VARIABLE dumpbin_error)
if(NOT dumpbin_result EQUAL 0)
  message(FATAL_ERROR
    "dumpbin failed for ${BINARY} (${dumpbin_result}): ${dumpbin_error}")
endif()

string(TOUPPER "${dependencies}" dependencies_upper)
set(forbidden
  MSVCP140.DLL
  MSVCP140_ATOMIC_WAIT.DLL
  VCRUNTIME140.DLL
  VCRUNTIME140_1.DLL)
foreach(name IN LISTS forbidden)
  if(dependencies_upper MATCHES "(^|[\r\n ][\t ]*)${name}([\r\n ]|$)")
    message(FATAL_ERROR
      "${BINARY} depends on ${name}; clean Windows installs cannot load it")
  endif()
endforeach()
