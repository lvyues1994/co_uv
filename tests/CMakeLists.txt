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

foreach(name IN ITEMS result_tests loop_tests thread_tests req_awaiter_tests
                      run_wrong_thread_contract_test)
    string(REPLACE "_" "-" target "uvco-${name}")
    add_executable(${target} ${name}.cpp)
    uvco_configure_test(${target})
    add_test(NAME ${target} COMMAND ${target})
endforeach()

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
