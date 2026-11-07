function(uvco_configure_test target)
    target_link_libraries(${target} PRIVATE uvco::uvco)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast
            -Werror=return-type -Werror=suggest-override)
    endif()
endfunction()

set(uvco_test_names udp_options_tests yield_turn_tests dir_tests buffered_reader_tests capture_tests wait_slot_tests work_tests dns_tests when_all_results_tests when_any_exception_tests process_portable_tests result_tests loop_tests thread_tests timer_tests sync_tests udp_tests lifecycle_tests
    run_wrong_thread_contract_test orphan_handle_use_contract_test timer_pending_destroy_contract_test
    sync_event_destroy_contract_test sync_when_any_empty_contract_test
    stream_concurrent_read_contract_test udp_concurrent_recv_contract_test)
if(UNIX)
    list(APPEND uvco_test_names stream_tests req_awaiter_tests fd_adoption_tests)
endif()
# FIFO, /proc/self/fd and waitpid lifecycle tests are Linux-specific.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND uvco_test_names fs_tests process_tests)
endif()
foreach(name IN LISTS uvco_test_names)
    string(REPLACE "_" "-" target "uvco-${name}")
    add_executable(${target} ${name}.cpp)
    uvco_configure_test(${target})
    add_test(NAME ${target} COMMAND ${target})
endforeach()

# 保活释放竞争测试需要 GNU ld / lld 的 --wrap（ELF 平台）。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MSVC)
    add_executable(uvco-keepalive-release-race-test keepalive_release_race_test.cpp)
    uvco_configure_test(uvco-keepalive-release-race-test)
    target_link_options(uvco-keepalive-release-race-test PRIVATE "-Wl,--wrap=uv_async_send")
    add_test(NAME uvco-keepalive-release-race-test COMMAND uvco-keepalive-release-race-test)
endif()

if(TARGET uvco-stream-tests)
    target_compile_definitions(uvco-stream-tests PRIVATE UVCO_TEST_PIPE_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(TARGET uvco-req-awaiter-tests)
    target_compile_definitions(uvco-req-awaiter-tests PRIVATE
        UVCO_TEST_SOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/req_awaiter_tests.cpp")
endif()
if(TARGET uvco-fs-tests)
    target_compile_definitions(uvco-fs-tests PRIVATE UVCO_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(uvco-fs-tests PROPERTIES ENVIRONMENT "UV_USE_IO_URING=1")
    add_test(NAME uvco-fs-tests-no-io-uring COMMAND uvco-fs-tests)
    set_tests_properties(uvco-fs-tests-no-io-uring PROPERTIES ENVIRONMENT "UV_USE_IO_URING=0")
endif()
add_executable(uvco-process-helper process_helper.cpp)
uvco_configure_test(uvco-process-helper)
# The child is a fixture, not the instrumented library under test; avoid a
# second sanitizer runtime in every subprocess (including signal-kill tests).
if(NOT MSVC)
    target_compile_options(uvco-process-helper PRIVATE -fno-sanitize=all)
    target_link_options(uvco-process-helper PRIVATE -fno-sanitize=all)
endif()
if(TARGET uvco-process-tests)
    add_dependencies(uvco-process-tests uvco-process-helper)
    target_compile_definitions(uvco-process-tests PRIVATE
        UVCO_TEST_PROCESS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")
endif()

# 每个公共头文件都必须能单独包含。
file(GLOB_RECURSE uvco_public_headers CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/include/uvco/*.hpp")
set(uvco_header_sources)
foreach(header IN LISTS uvco_public_headers)
    file(RELATIVE_PATH include "${PROJECT_SOURCE_DIR}/include" "${header}")
    string(MAKE_C_IDENTIFIER "${include}" identifier)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header-self-containment/${identifier}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <${include}>\n")
    list(APPEND uvco_header_sources "${source}")
endforeach()
add_library(uvco-header-self-containment OBJECT ${uvco_header_sources})
uvco_configure_test(uvco-header-self-containment)



add_dependencies(uvco-process-portable-tests uvco-process-helper)
target_compile_definitions(uvco-process-portable-tests PRIVATE
    UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-dns-fault-tests dns_fault_tests.cpp)
    uvco_configure_test(uvco-dns-fault-tests)
    # Wrap allocation calls instead of replacing global new/delete: TSan owns
    # strong new/delete interceptors. Keep the allocator itself instrumented.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_uvco_new_symbol _Znwm)
    else()
        set(_uvco_new_symbol _Znwj)
    endif()
    target_compile_definitions(uvco-dns-fault-tests PRIVATE
        UVCO_TEST_REAL_NEW=__real_${_uvco_new_symbol}
        UVCO_TEST_WRAP_NEW=__wrap_${_uvco_new_symbol})
    target_link_options(uvco-dns-fault-tests PRIVATE
        "-Wl,--wrap=uv_getaddrinfo" "-Wl,--wrap=uv_freeaddrinfo" "-Wl,--wrap=${_uvco_new_symbol}")
    unset(_uvco_new_symbol)
    add_test(NAME uvco-dns-fault-tests COMMAND uvco-dns-fault-tests)
endif()

set_tests_properties(uvco-work-tests PROPERTIES TIMEOUT 30)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-range-storage-tests range_storage_tests.cpp)
    uvco_configure_test(uvco-range-storage-tests)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_uvco_new_symbol _Znwm)
    else()
        set(_uvco_new_symbol _Znwj)
    endif()
    target_compile_definitions(uvco-range-storage-tests PRIVATE
        UVCO_TEST_REAL_NEW=__real_${_uvco_new_symbol} UVCO_TEST_WRAP_NEW=__wrap_${_uvco_new_symbol})
    target_link_options(uvco-range-storage-tests PRIVATE "-Wl,--wrap=${_uvco_new_symbol}")
    add_test(NAME uvco-range-storage-tests COMMAND uvco-range-storage-tests)
    unset(_uvco_new_symbol)
endif()

add_dependencies(uvco-capture-tests uvco-process-helper)
target_compile_definitions(uvco-capture-tests PRIVATE UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")
set_tests_properties(uvco-capture-tests PROPERTIES TIMEOUT 30)

target_compile_definitions(uvco-dir-tests PRIVATE UVCO_TEST_DIR_PARENT="${CMAKE_CURRENT_BINARY_DIR}")
