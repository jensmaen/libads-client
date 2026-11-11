cmake_minimum_required(VERSION 3.25)
project(libads-client VERSION 0.3.0 LANGUAGES CXX)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# Built on libads (the replacement for the TcAdsDll, through the local TwinCAT router):
#   route way: without TwinCAT, directly to the target over an ADS route (src/route), with Secure ADS and
#              reconnecting; its ports go through the C-API of libads
#   C++ classes and symbols (src/client), over both ways
option(LIBADS_CLIENT_SECURE "Secure ADS (TLS on TCP 8016) with mbedTLS" ON)
option(LIBADS_CLIENT_FETCH_MBEDTLS "Download mbedTLS 3.6 if find_package() does not find it" ON)
option(LIBADS_CLIENT_FETCH_LIBADS "Download libads if it is neither a target, beside this folder nor installed" ON)
# Off when the library is part of another project (FetchContent, add_subdirectory).
option(LIBADS_CLIENT_INSTALL "Install rules and CMake package" ${PROJECT_IS_TOP_LEVEL})
option(LIBADS_CLIENT_BUILD_TESTS "Build the test programs" ${PROJECT_IS_TOP_LEVEL})
option(LIBADS_CLIENT_BUILD_EXAMPLES "Build the example programs" ${PROJECT_IS_TOP_LEVEL})

# libads in exactly this version: libads-client uses its inner headers (libads::ads-internal).
set(LIBADS_CLIENT_LIBADS_VERSION 0.3.0)
include(cmake/FetchLibads.cmake)

add_library(ads-client
    src/route/AdsLib.cpp
    src/route/AmsPort.cpp
    src/route/AmsRouter.cpp
    src/client/AdsDevice.cpp
    src/client/AdsSymbols.cpp
)
add_library(libads-client::ads-client ALIAS ads-client)
# Static only (no export macros); Debug and Release can be installed side by side.
set_target_properties(ads-client PROPERTIES DEBUG_POSTFIX d)
# Settings as in Beckhoff ADS.
target_compile_features(ads-client PUBLIC cxx_std_14)
target_compile_definitions(ads-client PRIVATE CONFIG_DEFAULT_LOGLEVEL=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(ads-client PRIVATE _GNU_SOURCE)
endif()
# Installed flat beside the headers of libads (include/libads), so that #include "AdsLib.h" works.
target_include_directories(ads-client PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/route>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/client>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/libads-client>"
)
# AdsLib.h, AdsDevice.h, AdsVariable.h, AdsNotificationOOI.h, AdsSymbols.h and what they include.
set(LIBADS_CLIENT_PUBLIC_HEADERS
    src/client/AdsLib.h
    src/client/AdsDevice.h
    src/client/AdsException.h
    src/client/AdsVariable.h
    src/client/AdsNotificationOOI.h
    src/client/AdsSymbols.h
    src/route/TlsSettings.h
)
target_link_libraries(ads-client PUBLIC libads::ads PRIVATE libads::ads-internal)

# Secure ADS in the route way: mbedTLS 3.6, found or fetched (cmake/FetchMbedTLS.cmake).
if(LIBADS_CLIENT_SECURE)
    include(cmake/FetchMbedTLS.cmake)
    target_sources(ads-client PRIVATE src/route/TlsTransport.cpp)
    target_compile_definitions(ads-client PRIVATE LIBADS_CLIENT_SECURE=1)
    target_link_libraries(ads-client PRIVATE MbedTLS::mbedtls)
endif()
if(WIN32)
    target_compile_definitions(ads-client PRIVATE _CRT_NONSTDC_NO_WARNINGS=1 _CRT_SECURE_NO_WARNINGS=1)
endif()

if(LIBADS_CLIENT_INSTALL)
    include(CMakePackageConfigHelpers)
    install(TARGETS ads-client EXPORT libads-clientTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${LIBADS_CLIENT_PUBLIC_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libads-client)
    install(FILES LICENSE LICENSE.Beckhoff DESTINATION ${CMAKE_INSTALL_DOCDIR})

    set(LIBADS_CLIENT_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/libads-client)
    install(EXPORT libads-clientTargets NAMESPACE libads-client:: DESTINATION ${LIBADS_CLIENT_CMAKE_DIR})
    # Secure ADS as TRUE/FALSE for the config.
    if(LIBADS_CLIENT_SECURE)
        set(LIBADS_CLIENT_SECURE_BOOL TRUE)
    else()
        set(LIBADS_CLIENT_SECURE_BOOL FALSE)
    endif()
    configure_package_config_file(cmake/libads-clientConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/libads-clientConfig.cmake
        INSTALL_DESTINATION ${LIBADS_CLIENT_CMAKE_DIR})
    # Before 1.0 a new minor version may change the API.
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libads-clientConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/libads-clientConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/libads-clientConfigVersion.cmake
        DESTINATION ${LIBADS_CLIENT_CMAKE_DIR})
endif()

if(LIBADS_CLIENT_BUILD_TESTS)
    # CTest runs only what needs neither TwinCAT nor a VM: the parts check, the symbol parser and the tests against
    # the fake router. The others need TwinCAT and are started by hand (below). Each fake-router test has its own
    # ports, so they can run in parallel (ctest -j). Under WSL Linux and Windows share ports, so Linux uses others by
    # default.
    enable_testing()
    if(WIN32)
        set(LIBADS_CLIENT_TEST_PORT_DEFAULT 48910)
    else()
        set(LIBADS_CLIENT_TEST_PORT_DEFAULT 49110)
    endif()
    set(LIBADS_CLIENT_TEST_PORT ${LIBADS_CLIENT_TEST_PORT_DEFAULT} CACHE STRING
        "First port of the fake-router tests in CTest (uses this one up to +21)")
    # The fake router and the notification test are the same as in libads: here as copies, or in a source tree with
    # libads beside this folder, taken from there.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/fake-router/FakeRouter.h)
        set(LIBADS_CLIENT_SHARED_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/test)
    else()
        set(LIBADS_CLIENT_SHARED_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../libads/test)
    endif()
    if(NOT TARGET fake-router)
        add_subdirectory(${LIBADS_CLIENT_SHARED_TESTS}/fake-router ${CMAKE_CURRENT_BINARY_DIR}/test/fake-router)
    endif()
    add_subdirectory(test/parts)
    add_subdirectory(test/symbols-fake)
    add_subdirectory(test/route-reconnect-fake)
    # The route way of the notification test (libads runs it for its local router).
    add_executable(subscriptions-fake-route ${LIBADS_CLIENT_SHARED_TESTS}/subscriptions-fake/main.cpp)
    target_compile_features(subscriptions-fake-route PRIVATE cxx_std_17)
    target_include_directories(subscriptions-fake-route PRIVATE ${LIBADS_CLIENT_SHARED_TESTS}/fake-router)
    target_link_libraries(subscriptions-fake-route PRIVATE ads-client libads::ads-internal)
    target_compile_definitions(subscriptions-fake-route PRIVATE LIBADS_TEST_LOCAL=0 LIBADS_TEST_ROUTE=1)
    if(WIN32)
        target_compile_definitions(subscriptions-fake-route PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    math(EXPR port "${LIBADS_CLIENT_TEST_PORT} + 10")
    add_test(NAME subscriptions-fake-route COMMAND subscriptions-fake-route ${port})
    set_tests_properties(subscriptions-fake-route PROPERTIES TIMEOUT 180 LABELS fake-router)

    # Programs against a real TwinCAT system, started by hand. Not part of every source copy, hence only if present.
    function(libads_client_manual_test dir)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/${dir}/CMakeLists.txt)
            add_subdirectory(test/${dir})
        endif()
    endfunction()
    libads_client_manual_test(write-control)
    libads_client_manual_test(symbols-plc)
    libads_client_manual_test(route-client)
    libads_client_manual_test(local-client) # opens a route port beside the local ones
endif()

if(LIBADS_CLIENT_BUILD_EXAMPLES AND WIN32)
    add_subdirectory(examples/symbol-browser)
endif()
if(LIBADS_CLIENT_BUILD_EXAMPLES AND NOT WIN32)
    # Skipped without ncurses (wide characters), e.g. without the package libncurses-dev.
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses QUIET)
    if(CURSES_FOUND)
        add_subdirectory(examples/symbol-browser-tui)
    else()
        message(STATUS "symbol-browser-tui skipped: ncurses with wide characters not found")
    endif()
endif()
