cmake_minimum_required(VERSION 3.14)

project(uvco VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

set(UVCO_BUILD_TESTS_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(UVCO_BUILD_TESTS_DEFAULT ON)
endif()
option(UVCO_BUILD_TESTS "Build uvco tests" ${UVCO_BUILD_TESTS_DEFAULT})
unset(UVCO_BUILD_TESTS_DEFAULT)

set(UVCO_CO2_GIT_REPOSITORY "https://github.com/lvyues1994/coro.git"
    CACHE STRING "co2 git repository used when find_package(co2) fails")
# 固定到已验证的提交，保证 FetchContent 回退可复现。
set(UVCO_CO2_GIT_TAG "a265e5771b0e3ca68d6d473b9bb410fadddf4c23"
    CACHE STRING "co2 git tag/commit used when find_package(co2) fails")
set(UVCO_LIBUV_GIT_TAG "v1.48.0"
    CACHE STRING "libuv git tag used when no system libuv is found")

# ---- co2 ----------------------------------------------------------------------
# co2 的预览包拒绝任何数字版本请求，因此不带版本查找。
if(NOT TARGET co2::co2)
    find_package(co2 CONFIG QUIET)
endif()
if(TARGET co2::co2)
    message(STATUS "uvco: using co2 from find_package / parent project")
else()
    message(STATUS "uvco: fetching co2 (${UVCO_CO2_GIT_TAG})")
    set(CO2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(co2
        GIT_REPOSITORY ${UVCO_CO2_GIT_REPOSITORY}
        GIT_TAG ${UVCO_CO2_GIT_TAG})
    FetchContent_MakeAvailable(co2)
endif()

# Capability probe, including installed co2 where a pinned FetchContent commit
# cannot protect us. Check exact detail hooks instead of a numeric preview version.
include(CheckCXXSourceCompiles)
get_target_property(_uvco_co2_includes co2::co2 INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/include")
foreach(_inc IN LISTS _uvco_co2_includes)
    if(_inc MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
        list(APPEND CMAKE_REQUIRED_INCLUDES "${CMAKE_MATCH_1}")
    elseif(NOT _inc MATCHES "\\$<")
        list(APPEND CMAKE_REQUIRED_INCLUDES "${_inc}")
    endif()
endforeach()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/co2_capability.cpp" _uvco_probe)
unset(UVCO_CO2_COMPATIBLE CACHE)
check_cxx_source_compiles("${_uvco_probe}" UVCO_CO2_COMPATIBLE)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT UVCO_CO2_COMPATIBLE)
    message(FATAL_ERROR "Incompatible co2 internal API. See cmake/co2_capability.cpp and CMakeConfigureLog.yaml")
endif()

# ---- libuv --------------------------------------------------------------------
# 顺序：CMake config 包 → pkg-config → FetchContent。
set(UVCO_LIBUV_TARGET "")
if(TARGET libuv::uv)
    set(UVCO_LIBUV_TARGET libuv::uv)
else()
    find_package(libuv CONFIG QUIET)
    if(TARGET libuv::uv)
        set(UVCO_LIBUV_TARGET libuv::uv)
    elseif(TARGET libuv::uv_a)
        set(UVCO_LIBUV_TARGET libuv::uv_a)
    endif()
endif()
if(UVCO_LIBUV_TARGET)
    message(STATUS "uvco: using libuv from CMake package (${UVCO_LIBUV_TARGET})")
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(UVCO_LIBUV QUIET IMPORTED_TARGET libuv>=1.40)
        if(UVCO_LIBUV_FOUND)
            set(UVCO_LIBUV_TARGET PkgConfig::UVCO_LIBUV)
            message(STATUS "uvco: using libuv ${UVCO_LIBUV_VERSION} from pkg-config")
        endif()
    endif()
endif()
if(NOT UVCO_LIBUV_TARGET)
    message(STATUS "uvco: fetching libuv (${UVCO_LIBUV_GIT_TAG})")
    set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libuv
        GIT_REPOSITORY https://github.com/libuv/libuv.git
        GIT_TAG ${UVCO_LIBUV_GIT_TAG})
    FetchContent_MakeAvailable(libuv)
    set(UVCO_LIBUV_TARGET uv_a)
endif()

# ---- uvco ---------------------------------------------------------------------
add_library(uvco INTERFACE)
add_library(uvco::uvco ALIAS uvco)
target_compile_features(uvco INTERFACE cxx_std_14)
target_include_directories(uvco INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(uvco INTERFACE co2::co2 ${UVCO_LIBUV_TARGET})

if(UVCO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
