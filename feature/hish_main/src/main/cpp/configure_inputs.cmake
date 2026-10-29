# CMake may otherwise emit the same Ninja phony output twice when a manifest
# uses Windows separators and another dependency contains relative segments.
get_property(HISH_CONFIGURE_INPUTS DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
set(HISH_CANONICAL_INPUTS)
foreach(INPUT ${HISH_CONFIGURE_INPUTS})
    file(TO_CMAKE_PATH "${INPUT}" INPUT)
    get_filename_component(INPUT "${INPUT}" ABSOLUTE)
    list(APPEND HISH_CANONICAL_INPUTS "${INPUT}")
endforeach()
list(REMOVE_DUPLICATES HISH_CANONICAL_INPUTS)
set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS "${HISH_CANONICAL_INPUTS}")
