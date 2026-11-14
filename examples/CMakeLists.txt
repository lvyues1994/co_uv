function(uvco_configure_example target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
            -Werror=return-type)
    endif()
endfunction()

add_executable(uvco-example-echo-helper helpers/echo_helper.cpp)
uvco_configure_example(uvco-example-echo-helper)

set(uvco_example_sources
    01_quickstart
    02_tcp_echo
    03_udp
    04_fs
    05_watcher
    06_process_capture
    07_timeout_whenany
    08_cancellation
    09_work)

foreach(name IN LISTS uvco_example_sources)
    string(REPLACE "_" "-" target "uvco-example-${name}")
    add_executable(${target} ${name}.cpp)
    target_link_libraries(${target} PRIVATE uvco::uvco)
    uvco_configure_example(${target})
    add_test(NAME ${target} COMMAND ${target})
    # 示例在构建目录写临时文件并使用本目录中的 helper。
    set_tests_properties(${target} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

target_compile_definitions(uvco-example-06-process-capture PRIVATE
    UVCO_EXAMPLE_HELPER="$<TARGET_FILE:uvco-example-echo-helper>")
add_dependencies(uvco-example-06-process-capture uvco-example-echo-helper)

if(UVCO_ENABLE_KCP)
    add_executable(uvco-example-10-kcp 10_kcp.cpp)
    target_link_libraries(uvco-example-10-kcp PRIVATE uvco::kcp)
    uvco_configure_example(uvco-example-10-kcp)
    add_test(NAME uvco-example-10-kcp COMMAND uvco-example-10-kcp)
    set_tests_properties(uvco-example-10-kcp PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
