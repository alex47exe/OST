# Copies a freshly built artifact next to ost.exe.
# A payload that is currently mapped by a running Steam cannot be replaced
# (sharing violation); that must not break the build -- warn and let the next
# build after Steam exits refresh it.
if(NOT EXISTS "${SRC}")
    message(FATAL_ERROR "staging source missing: ${SRC}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}"
    RESULT_VARIABLE copy_result
    ERROR_QUIET)
if(NOT copy_result EQUAL 0)
    message(WARNING "could not stage ${DST}: file is in use (close Steam and rebuild to refresh it)")
endif()
