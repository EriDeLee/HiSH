# Ordinary AArch64 builds require a verified source runtime. Never silently use
# the historical payload directory or package stale diagnostic outputs.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT HISH_ENABLE_DIAGNOSTICS)
    if(NOT EXISTS "${HISH_LINUX_PAYLOAD_DIR}/runtime-manifest.json")
        message(FATAL_ERROR "Run node scripts/linux/build.cjs NATIVE_SDK before building the daily app")
    endif()
    file(READ "${HISH_LINUX_PAYLOAD_DIR}/runtime-manifest.json" HISH_RUNTIME_MANIFEST)
    string(JSON HISH_RUNTIME_TESTS GET "${HISH_RUNTIME_MANIFEST}" diagnostics)
    if(HISH_RUNTIME_TESTS)
        message(FATAL_ERROR "A diagnostic runtime cannot be packaged as the daily app")
    endif()
    foreach(LIST_NAME inputs files)
        string(JSON ITEM_COUNT LENGTH "${HISH_RUNTIME_MANIFEST}" ${LIST_NAME})
        math(EXPR LAST_ITEM "${ITEM_COUNT}-1")
        foreach(INDEX RANGE ${LAST_ITEM})
            if(LIST_NAME STREQUAL "inputs")
                string(JSON INPUT_FILE GET "${HISH_RUNTIME_MANIFEST}" inputs ${INDEX} file)
            else()
                string(JSON INPUT_NAME GET "${HISH_RUNTIME_MANIFEST}" files ${INDEX} name)
                set(INPUT_FILE "${HISH_LINUX_PAYLOAD_DIR}/${INPUT_NAME}")
            endif()
            string(JSON EXPECTED_HASH GET "${HISH_RUNTIME_MANIFEST}" ${LIST_NAME} ${INDEX} sha256)
            file(SHA256 "${INPUT_FILE}" ACTUAL_HASH)
            if(NOT ACTUAL_HASH STREQUAL EXPECTED_HASH)
                message(FATAL_ERROR "Stale or changed runtime input: ${INPUT_FILE}; rebuild scripts/linux/build.cjs")
            endif()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${INPUT_FILE}")
        endforeach()
    endforeach()
endif()
if(NOT HISH_ENABLE_DIAGNOSTICS AND CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    # Move only named generated test/intermediate libraries, retaining bytes and
    # never touching baseline payloads, user files, source or earlier HAPs.
    file(GLOB OLD_TEST_LIBS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_probe*.so"
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_mksh_trial*.so"
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_mrsh*.so"
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_bench*.so"
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_linux_*check.so"
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_linux_hishbench*.so")
    foreach(NAME musl_dynamic musl_static glibc_dynamic glibc_static glibc_static_compat fs_identity glibc_libc glibc_padded glibc_separate identity busybox dash)
        list(APPEND OLD_TEST_LIBS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_linux_${NAME}.so")
    endforeach()
    # Archive the exact former application/dependency outputs on base-only
    # builds. Keep historical payload generations and source trees untouched.
    foreach(NAME curl git git_remote_http git_http_fetch git_http_push gnu_printf gnu_uname tcc)
        list(APPEND OLD_TEST_LIBS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libhish_linux_${NAME}.so")
    endforeach()
    foreach(NAME brotlicommon1 brotlidec1 cares2 crypto3 curl4 expat1 idn20 nghttp214 pcre2-80 psl5 ssl3 unistring5 z1 zstd1)
        list(APPEND OLD_TEST_LIBS "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/lib${NAME}.so")
    endforeach()
    foreach(OLD_FILE ${OLD_TEST_LIBS})
        if(EXISTS "${OLD_FILE}")
            get_filename_component(OLD_NAME "${OLD_FILE}" NAME)
            file(SHA256 "${OLD_FILE}" OLD_HASH)
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/excluded-diagnostics/${OLD_HASH}")
            file(RENAME "${OLD_FILE}" "${CMAKE_CURRENT_BINARY_DIR}/excluded-diagnostics/${OLD_HASH}/${OLD_NAME}")
        endif()
    endforeach()
endif()
