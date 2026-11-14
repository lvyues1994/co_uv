cmake_minimum_required(VERSION 3.14)

project(uvco VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

# PROJECT_IS_TOP_LEVEL was introduced in 3.21; retain the 3.14 floor.
if(CMAKE_VERSION VERSION_LESS 3.21)
    set(PROJECT_IS_TOP_LEVEL OFF)
    if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL ON)
    endif()
endif()
option(UVCO_ENABLE_KCP "Build optional pinned KCP support" OFF)
option(UVCO_BUILD_KCP_BENCHMARK "Build the optional KCP/TCP benchmark" OFF)
option(UVCO_BUILD_TESTS "Build uvco tests" ${PROJECT_IS_TOP_LEVEL})
option(UVCO_ENABLE_INSTALL "Install/export uvco (installed deps required)" ${PROJECT_IS_TOP_LEVEL})
include(cmake/uvcoDependencies.cmake)

set(UVCO_CO2_GIT_REPOSITORY "https://github.com/lvyues1994/coro.git"
    CACHE STRING "co2 git repository used when find_package(co2) fails")
# 固定到已验证的提交，保证 FetchContent 回退可复现。
set(UVCO_CO2_GIT_TAG "0be5ab2f1b72be3755d8d13fb02d4bf32f652539"
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
    # Dependencies are not bundled into uvco's install. They can be installed
    # independently from their own build directories when desired.
    set_property(DIRECTORY "${co2_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()

uvco_check_co2(_uvco_co2_ok "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/co2_capability.cpp")
if(NOT _uvco_co2_ok)
    message(FATAL_ERROR "Incompatible co2 internal API; see cmake/co2_capability.cpp")
endif()

# ---- libuv --------------------------------------------------------------------
# 顺序：CMake config 包 → pkg-config → FetchContent。
set(UVCO_LIBUV_PROVIDER cmake)
uvco_select_libuv(UVCO_LIBUV_TARGET)
if(NOT UVCO_LIBUV_TARGET)
    find_package(libuv CONFIG QUIET)
    uvco_select_libuv(UVCO_LIBUV_TARGET)
endif()
if(UVCO_LIBUV_TARGET)
    message(STATUS "uvco: using libuv from CMake package (${UVCO_LIBUV_TARGET})")
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(UVCO_LIBUV QUIET IMPORTED_TARGET libuv>=1.40)
        if(UVCO_LIBUV_FOUND)
            set(UVCO_LIBUV_TARGET PkgConfig::UVCO_LIBUV)
            set(UVCO_LIBUV_PROVIDER pkgconfig)
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
    set_property(DIRECTORY "${libuv_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    set(UVCO_LIBUV_TARGET uv_a)
endif()

uvco_check_libuv(_uvco_libuv_ok ${UVCO_LIBUV_TARGET})
if(NOT _uvco_libuv_ok)
    message(FATAL_ERROR "uvco requires libuv >= 1.40 (checked actual headers on every discovery path)")
endif()

# ---- uvco ---------------------------------------------------------------------
add_library(uvco INTERFACE)
add_library(uvco::uvco ALIAS uvco)
target_compile_features(uvco INTERFACE cxx_std_14)
target_include_directories(uvco INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(uvco INTERFACE
    $<BUILD_INTERFACE:co2::co2>
    $<BUILD_INTERFACE:${UVCO_LIBUV_TARGET}>
    $<INSTALL_INTERFACE:uvco::dependencies>)

if(UVCO_ENABLE_KCP)
    add_library(uvco_kcp STATIC src/kcp_adapter.c)
    add_library(uvco::kcp ALIAS uvco_kcp)
    set_target_properties(uvco_kcp PROPERTIES EXPORT_NAME kcp POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(uvco_kcp PUBLIC uvco::uvco)
    target_compile_definitions(uvco_kcp PUBLIC UVCO_HAS_KCP=1)
    target_include_directories(uvco_kcp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/kcp>
        )
endif()

if(UVCO_ENABLE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    target_include_directories(uvco INTERFACE $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    set(_uvco_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/uvco")
    if(UVCO_ENABLE_KCP)
        target_include_directories(uvco_kcp PUBLIC
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/uvco/detail/kcp>)
        install(TARGETS uvco_kcp EXPORT uvcoTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT uvco)
        install(FILES third_party/kcp/ikcp.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uvco/detail/kcp COMPONENT uvco)
        install(FILES third_party/kcp/LICENSE third_party/kcp/VERSION.md
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/uvco/kcp COMPONENT uvco)
    endif()
    install(TARGETS uvco EXPORT uvcoTargets COMPONENT uvco)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT uvco)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/uvco COMPONENT uvco)
    install(EXPORT uvcoTargets FILE uvcoTargets.cmake NAMESPACE uvco::
        DESTINATION ${_uvco_package_dir} COMPONENT uvco)
    configure_package_config_file(cmake/uvcoConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/uvcoConfig.cmake
        INSTALL_DESTINATION ${_uvco_package_dir} PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
    # 0.x: patch compatible, not cross-minor compatible.
    set(_uvco_arch_option)
    if(NOT UVCO_ENABLE_KCP)
        set(_uvco_arch_option ARCH_INDEPENDENT)
    endif()
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/uvcoConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion ${_uvco_arch_option})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/uvcoConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/uvcoConfigVersion.cmake
        cmake/uvcoDependencies.cmake cmake/co2_capability.cpp
        DESTINATION ${_uvco_package_dir} COMPONENT uvco)
endif()

if(UVCO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(UVCO_BUILD_KCP_BENCHMARK)
    if(NOT UVCO_ENABLE_KCP)
        message(FATAL_ERROR "UVCO_BUILD_KCP_BENCHMARK requires UVCO_ENABLE_KCP=ON")
    endif()
    add_executable(uvco-kcp-latency benchmarks/kcp_latency.cpp)
    target_link_libraries(uvco-kcp-latency PRIVATE uvco::kcp)
endif()

option(UVCO_BUILD_PERF_BENCHMARK "Build raw libuv/uvco overhead benchmark" OFF)
if(UVCO_BUILD_PERF_BENCHMARK)
    add_executable(uvco-libuv-overhead benchmarks/libuv_overhead.cpp)
    target_link_libraries(uvco-libuv-overhead PRIVATE uvco::uvco)
endif()
