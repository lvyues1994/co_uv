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

foreach(name IN ITEMS result_tests loop_tests thread_tests req_awaiter_tests timer_tests
                      sync_tests stream_tests udp_tests lifecycle_tests
                      run_wrong_thread_contract_test orphan_handle_use_contract_test timer_pending_destroy_contract_test
                      sync_event_destroy_contract_test sync_when_any_empty_contract_test
                      stream_concurrent_read_contract_test udp_concurrent_recv_contract_test)
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

# Pipe 测试的套接字文件放在构建目录里。
target_compile_definitions(uvco-stream-tests PRIVATE
    UVCO_TEST_PIPE_DIR="${CMAKE_CURRENT_BINARY_DIR}")

target_compile_definitions(uvco-req-awaiter-tests PRIVATE
    UVCO_TEST_SOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/req_awaiter_tests.cpp")

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
