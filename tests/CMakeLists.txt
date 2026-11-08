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

set(uvco_test_names process_group_tests connect_host_tests resolver_tests tty_tests phase_tests signal_tests timer_migration_tests fs_poll_migration_tests fs_event_migration_tests watcher_policy_tests watcher_core_tests fs_event_tests fs_poll_tests fs_watch_concurrent_next_contract_test libuv_header_tests listener_tests udp_options_tests yield_turn_tests dir_tests buffered_reader_tests capture_tests wait_slot_tests work_tests dns_tests when_all_results_tests when_any_exception_tests process_portable_tests result_tests loop_tests thread_tests timer_tests sync_tests udp_tests lifecycle_tests
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
    if(NOT UVCO_ENABLE_KCP AND include MATCHES "^uvco/kcp")
        continue()
    endif()
    string(MAKE_C_IDENTIFIER "${include}" identifier)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header-self-containment/${identifier}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <${include}>\n")
    list(APPEND uvco_header_sources "${source}")
endforeach()
add_library(uvco-header-self-containment OBJECT ${uvco_header_sources})
uvco_configure_test(uvco-header-self-containment)
if(UVCO_ENABLE_KCP)
    target_link_libraries(uvco-header-self-containment PRIVATE uvco::kcp)
endif()



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
        set(_uvco_array_new_symbol _Znam)
    else()
        set(_uvco_new_symbol _Znwj)
        set(_uvco_array_new_symbol _Znaj)
    endif()
    target_compile_definitions(uvco-range-storage-tests PRIVATE
        UVCO_TEST_REAL_NEW=__real_${_uvco_new_symbol} UVCO_TEST_WRAP_NEW=__wrap_${_uvco_new_symbol}
        UVCO_TEST_REAL_ARRAY_NEW=__real_${_uvco_array_new_symbol} UVCO_TEST_WRAP_ARRAY_NEW=__wrap_${_uvco_array_new_symbol})
    target_link_options(uvco-range-storage-tests PRIVATE "-Wl,--wrap=${_uvco_new_symbol}" "-Wl,--wrap=${_uvco_array_new_symbol}")
    add_test(NAME uvco-range-storage-tests COMMAND uvco-range-storage-tests)
    unset(_uvco_new_symbol)
    unset(_uvco_array_new_symbol)
endif()

add_dependencies(uvco-capture-tests uvco-process-helper)
target_compile_definitions(uvco-capture-tests PRIVATE UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")
set_tests_properties(uvco-capture-tests PROPERTIES TIMEOUT 30)

target_compile_definitions(uvco-dir-tests PRIVATE UVCO_TEST_DIR_PARENT="${CMAKE_CURRENT_BINARY_DIR}")

target_compile_definitions(uvco-listener-tests PRIVATE UVCO_TEST_PIPE_DIR="${CMAKE_CURRENT_BINARY_DIR}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-dir-fault-tests dir_fault_tests.cpp)
    uvco_configure_test(uvco-dir-fault-tests)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_uvco_new_symbol _Znwm)
    else()
        set(_uvco_new_symbol _Znwj)
    endif()
    target_compile_definitions(uvco-dir-fault-tests PRIVATE
        UVCO_TEST_DIR_PATH="${PROJECT_SOURCE_DIR}" UVCO_TEST_REAL_NEW=__real_${_uvco_new_symbol} UVCO_TEST_WRAP_NEW=__wrap_${_uvco_new_symbol})
    target_link_options(uvco-dir-fault-tests PRIVATE "-Wl,--wrap=uv_fs_opendir" "-Wl,--wrap=uv_fs_closedir" "-Wl,--wrap=${_uvco_new_symbol}")
    add_test(NAME uvco-dir-fault-tests COMMAND uvco-dir-fault-tests)
    unset(_uvco_new_symbol)
endif()

# The output-string fault fixture wraps a libstdc++ C++11-ABI append symbol.
# Only this fixture is ABI-specific; ordinary capture tests remain portable.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    check_cxx_source_compiles("#include <string>
#if !defined(__GLIBCXX__) || !_GLIBCXX_USE_CXX11_ABI
#error unsupported string ABI
#endif
int main() {}" UVCO_TEST_GLIBCXX_ABI)
    if(UVCO_TEST_GLIBCXX_ABI)
        add_executable(uvco-capture-fault-tests capture_fault_tests.cpp)
        uvco_configure_test(uvco-capture-fault-tests)
        # Preserve the intercepted append boundary even in Release builds.
        target_compile_options(uvco-capture-fault-tests PRIVATE -fno-inline)
        add_dependencies(uvco-capture-fault-tests uvco-process-helper)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_uvco_append_symbol _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKcm)
        else()
            set(_uvco_append_symbol _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKcj)
        endif()
        target_compile_definitions(uvco-capture-fault-tests PRIVATE
            UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>"
            UVCO_TEST_APPEND_SYMBOL="${_uvco_append_symbol}")
        target_link_options(uvco-capture-fault-tests PRIVATE "-Wl,--wrap=uv_read_start" "-Wl,--wrap=${_uvco_append_symbol}")
        add_test(NAME uvco-capture-fault-tests COMMAND uvco-capture-fault-tests)
        set_tests_properties(uvco-capture-fault-tests PROPERTIES TIMEOUT 30)
        unset(_uvco_append_symbol)
    endif()
endif()

if(NOT DEFINED UVCO_ENABLE_INSTALL OR UVCO_ENABLE_INSTALL)
    configure_file(package_tests.cmake.in package_tests.cmake @ONLY)
    add_test(NAME uvco-package-tests COMMAND ${CMAKE_COMMAND} -DCONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_BINARY_DIR}/package_tests.cmake)
    set_tests_properties(uvco-package-tests PROPERTIES TIMEOUT 180)
endif()

# This matrix uses the real pkg-config library to construct CMake metadata and
# local FetchContent fixtures. Native CMake-only platforms still run package-tests.
if(UVCO_ENABLE_INSTALL AND UVCO_LIBUV_PROVIDER STREQUAL "pkgconfig")
    configure_file(dependency_tests.cmake.in dependency_tests.cmake @ONLY)
    add_test(NAME uvco-dependency-tests COMMAND ${CMAKE_COMMAND} -DCONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_BINARY_DIR}/dependency_tests.cmake)
    set_tests_properties(uvco-dependency-tests PROPERTIES TIMEOUT 180)
endif()

target_compile_definitions(uvco-fs-event-tests PRIVATE UVCO_TEST_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(uvco-fs-event-tests PROPERTIES TIMEOUT 45)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-fs-event-fault-tests fs_event_fault_tests.cpp)
    uvco_configure_test(uvco-fs-event-fault-tests)
    target_compile_definitions(uvco-fs-event-fault-tests PRIVATE UVCO_TEST_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    target_link_options(uvco-fs-event-fault-tests PRIVATE "-Wl,--wrap=uv_fs_event_init" "-Wl,--wrap=uv_fs_event_start")
    if(UVCO_TEST_GLIBCXX_ABI)
        target_compile_options(uvco-fs-event-fault-tests PRIVATE -fno-inline)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_uvco_append_symbol _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKcm)
        else()
            set(_uvco_append_symbol _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6appendEPKcj)
        endif()
        target_compile_definitions(uvco-fs-event-fault-tests PRIVATE UVCO_TEST_APPEND_SYMBOL="${_uvco_append_symbol}")
        target_link_options(uvco-fs-event-fault-tests PRIVATE "-Wl,--wrap=${_uvco_append_symbol}")
        unset(_uvco_append_symbol)
    endif()
    add_test(NAME uvco-fs-event-fault-tests COMMAND uvco-fs-event-fault-tests)
    set_tests_properties(uvco-fs-event-fault-tests PROPERTIES TIMEOUT 45)
endif()

target_compile_definitions(uvco-fs-poll-tests PRIVATE UVCO_TEST_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(uvco-fs-poll-tests PROPERTIES TIMEOUT 45)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-fs-poll-fault-tests fs_poll_fault_tests.cpp)
    uvco_configure_test(uvco-fs-poll-fault-tests)
    target_compile_definitions(uvco-fs-poll-fault-tests PRIVATE UVCO_TEST_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    target_link_options(uvco-fs-poll-fault-tests PRIVATE "-Wl,--wrap=uv_fs_poll_init" "-Wl,--wrap=uv_fs_poll_start")
    add_test(NAME uvco-fs-poll-fault-tests COMMAND uvco-fs-poll-fault-tests)
    set_tests_properties(uvco-fs-poll-fault-tests PROPERTIES TIMEOUT 45)
endif()

target_compile_definitions(uvco-fs-watch-concurrent-next-contract-test PRIVATE
    UVCO_TEST_WATCH_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fs_poll_tests.cpp")
set_tests_properties(uvco-fs-watch-concurrent-next-contract-test PROPERTIES TIMEOUT 15)

add_dependencies(uvco-signal-tests uvco-process-helper)
target_compile_definitions(uvco-signal-tests PRIVATE UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")
set_tests_properties(uvco-signal-tests PROPERTIES TIMEOUT 30)

set_tests_properties(uvco-phase-tests uvco-tty-tests PROPERTIES TIMEOUT 30)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-tty-fault-tests tty_fault_tests.cpp)
    uvco_configure_test(uvco-tty-fault-tests)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_uvco_new_symbol _Znwm)
    else()
        set(_uvco_new_symbol _Znwj)
    endif()
    target_compile_definitions(uvco-tty-fault-tests PRIVATE
        UVCO_TEST_REAL_NEW=__real_${_uvco_new_symbol} UVCO_TEST_WRAP_NEW=__wrap_${_uvco_new_symbol})
    target_link_options(uvco-tty-fault-tests PRIVATE "-Wl,--wrap=uv_tty_init" "-Wl,--wrap=${_uvco_new_symbol}")
    add_test(NAME uvco-tty-fault-tests COMMAND uvco-tty-fault-tests)
    unset(_uvco_new_symbol)
endif()

add_dependencies(uvco-process-group-tests uvco-process-helper)
target_compile_definitions(uvco-process-group-tests PRIVATE UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")
set_tests_properties(uvco-connect-host-tests uvco-resolver-tests uvco-process-group-tests PROPERTIES TIMEOUT 30)
if(UNIX)
    add_executable(uvco-process-group-helper process_group_helper.cpp)
    uvco_configure_test(uvco-process-group-helper)
    target_compile_options(uvco-process-group-helper PRIVATE -fno-sanitize=all)
    target_link_options(uvco-process-group-helper PRIVATE -fno-sanitize=all)
    add_dependencies(uvco-process-group-tests uvco-process-group-helper)
    target_compile_definitions(uvco-process-group-tests PRIVATE UVCO_TEST_PROCESS_GROUP_HELPER="$<TARGET_FILE:uvco-process-group-helper>")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-process-group-fault-tests process_group_fault_tests.cpp)
    uvco_configure_test(uvco-process-group-fault-tests)
    target_link_options(uvco-process-group-fault-tests PRIVATE "-Wl,--wrap=waitid" "-Wl,--wrap=uv_kill" "-Wl,--wrap=kill")
    add_dependencies(uvco-process-group-fault-tests uvco-process-helper)
    target_compile_definitions(uvco-process-group-fault-tests PRIVATE UVCO_TEST_PROCESS_HELPER="$<TARGET_FILE:uvco-process-helper>")
    add_test(NAME uvco-process-group-fault-tests COMMAND uvco-process-group-fault-tests)
    set_tests_properties(uvco-process-group-fault-tests PROPERTIES TIMEOUT 30)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(uvco-connect-host-fault-tests connect_host_fault_tests.cpp)
    uvco_configure_test(uvco-connect-host-fault-tests)
    target_link_options(uvco-connect-host-fault-tests PRIVATE "-Wl,--wrap=_Znwm")
    add_test(NAME uvco-connect-host-fault-tests COMMAND uvco-connect-host-fault-tests)
    set_tests_properties(uvco-connect-host-fault-tests PROPERTIES TIMEOUT 30)
endif()

add_executable(uvco-write-cancel-tests write_cancel_tests.cpp)
uvco_configure_test(uvco-write-cancel-tests)
add_test(NAME uvco-write-cancel-tests COMMAND uvco-write-cancel-tests)
set_tests_properties(uvco-write-cancel-tests PROPERTIES TIMEOUT 5)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
 add_executable(uvco-schedule-fault-tests schedule_fault_tests.cpp)
 uvco_configure_test(uvco-schedule-fault-tests)
 target_link_options(uvco-schedule-fault-tests PRIVATE "-Wl,--wrap=_Znwm")
 add_test(NAME uvco-schedule-fault-tests COMMAND uvco-schedule-fault-tests)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
 add_executable(uvco-factory-fault-tests factory_fault_tests.cpp)
 uvco_configure_test(uvco-factory-fault-tests)
 target_link_options(uvco-factory-fault-tests PRIVATE "-Wl,--wrap=_Znwm" "-Wl,--wrap=uv_tcp_init" "-Wl,--wrap=uv_udp_init" "-Wl,--wrap=uv_timer_init" "-Wl,--wrap=uv_async_init")
 add_test(NAME uvco-factory-fault-tests COMMAND uvco-factory-fault-tests)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
 add_executable(uvco-write-partial-fault-tests write_partial_fault_tests.cpp)
 uvco_configure_test(uvco-write-partial-fault-tests)
 target_link_options(uvco-write-partial-fault-tests PRIVATE "-Wl,--wrap=uv_try_write" "-Wl,--wrap=uv_write")
 add_test(NAME uvco-write-partial-fault-tests COMMAND uvco-write-partial-fault-tests)
endif()

if(UVCO_ENABLE_KCP)
    add_test(NAME uvco-kcp-vendor-tests COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/kcp_vendor_tests.cmake)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-core-tests kcp_core_tests.cpp)
    uvco_configure_test(uvco-kcp-core-tests)
    target_link_libraries(uvco-kcp-core-tests PRIVATE uvco::kcp)
    add_test(NAME uvco-kcp-core-tests COMMAND uvco-kcp-core-tests)
    set_tests_properties(uvco-kcp-core-tests PROPERTIES TIMEOUT 30)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-runtime-tests kcp_runtime_tests.cpp)
    uvco_configure_test(uvco-kcp-runtime-tests)
    target_link_libraries(uvco-kcp-runtime-tests PRIVATE uvco::kcp)
    add_test(NAME uvco-kcp-runtime-tests COMMAND uvco-kcp-runtime-tests)
    set_tests_properties(uvco-kcp-runtime-tests PROPERTIES TIMEOUT 30)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-stream-tests kcp_stream_tests.cpp)
    uvco_configure_test(uvco-kcp-stream-tests)
    target_link_libraries(uvco-kcp-stream-tests PRIVATE uvco::kcp)
    add_test(NAME uvco-kcp-stream-tests COMMAND uvco-kcp-stream-tests)
    set_tests_properties(uvco-kcp-stream-tests PROPERTIES TIMEOUT 30)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-accept-tests kcp_accept_tests.cpp)
    uvco_configure_test(uvco-kcp-accept-tests)
    target_link_libraries(uvco-kcp-accept-tests PRIVATE uvco::kcp)
    add_test(NAME uvco-kcp-accept-tests COMMAND uvco-kcp-accept-tests)
    set_tests_properties(uvco-kcp-accept-tests PROPERTIES TIMEOUT 30)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-pressure-tests kcp_pressure_tests.cpp)
    uvco_configure_test(uvco-kcp-pressure-tests)
    target_link_libraries(uvco-kcp-pressure-tests PRIVATE uvco::kcp)
    add_test(NAME uvco-kcp-pressure-tests COMMAND uvco-kcp-pressure-tests)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-flush-tests kcp_flush_tests.cpp)
    uvco_configure_test(uvco-kcp-flush-tests)
    target_link_libraries(uvco-kcp-flush-tests PRIVATE uvco::kcp)
    add_test(NAME uvco-kcp-flush-tests COMMAND uvco-kcp-flush-tests)
endif()

if(UVCO_ENABLE_KCP)
    add_executable(uvco-kcp-latency-stats-tests kcp_latency_stats_tests.cpp)
    uvco_configure_test(uvco-kcp-latency-stats-tests)
    add_test(NAME uvco-kcp-latency-stats-tests COMMAND uvco-kcp-latency-stats-tests)
endif()
