cmake_minimum_required(VERSION 3.25)
project(libads-client VERSION 0.2.1 LANGUAGES CXX)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# One library in two parts that can be switched off:
#   local: replacement for the TcAdsDll, through the local TwinCAT router (src/local)
#   route: without TwinCAT, directly to the target over an ADS route (src/route), with Secure ADS and reconnecting
# Common to both: src/core, src/connection and the C-API in src/client. A part that is off keeps its functions; they
# fail with ADSERR_DEVICE_SRVNOTSUPP (AdsPortOpenEx()/AdsPortOpenRouteEx() return 0).
option(LIBADS_CLIENT_LOCAL "Local way through the TwinCAT router (replacement for the TcAdsDll)" ON)
option(LIBADS_CLIENT_ROUTE "Route way without TwinCAT: own routes, Secure ADS, reconnecting" ON)
option(LIBADS_CLIENT_SECURE "Secure ADS (TLS on TCP 8016) with mbedTLS, only with LIBADS_CLIENT_ROUTE" ON)
option(LIBADS_CLIENT_FETCH_MBEDTLS "Download mbedTLS 3.6 if find_package() does not find it" ON)
# Off when the library is part of another project (FetchContent, add_subdirectory).
option(LIBADS_CLIENT_INSTALL "Install rules and CMake package" ${PROJECT_IS_TOP_LEVEL})
option(LIBADS_CLIENT_BUILD_TESTS "Build the test programs" ${PROJECT_IS_TOP_LEVEL})
option(LIBADS_CLIENT_BUILD_EXAMPLES "Build the example programs" ${PROJECT_IS_TOP_LEVEL})

add_library(ads-client
    src/core/AdsDef.cpp
    src/core/AmsNetId.cpp
    src/core/Frame.cpp
    src/core/Log.cpp
    src/connection/AmsConnection.cpp
    src/connection/AmsTransport.cpp
    src/connection/NotificationDispatcher.cpp
    src/connection/Sockets.cpp
    src/connection/Subscriptions.cpp
    src/client/AdsDevice.cpp
    src/client/AdsSymbols.cpp
    src/client/StandaloneAdsLib.cpp
)
add_library(libads-client::ads-client ALIAS ads-client)
# Static only (no export macros); Debug and Release can be installed side by side.
set_target_properties(ads-client PROPERTIES DEBUG_POSTFIX d)
# Settings as in Beckhoff ADS (ADS/CMakeLists.txt).
target_compile_features(ads-client PUBLIC cxx_std_14)
target_compile_definitions(ads-client PRIVATE CONFIG_DEFAULT_LOGLEVEL=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(ads-client PRIVATE _GNU_SOURCE)
endif()
# All folders, whatever is switched off: the API stays the same. In the build tree also the inner ones (tests);
# installed only the public headers, flat, so that #include "TcAdsAPI.h" works as with the TcAdsDll.
target_include_directories(ads-client PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/connection>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/local>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/route>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/client>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/libads-client>"
)
# TcAdsAPI.h, AdsLib.h, AdsDevice.h, AdsVariable.h, AdsNotificationOOI.h, AdsSymbols.h and what they include.
set(LIBADS_CLIENT_PUBLIC_HEADERS
    src/local/TcAdsAPI.h
    src/local/TcAdsDef.h
    src/client/AdsLib.h
    src/client/StandaloneAdsLib.h
    src/client/AdsDevice.h
    src/client/AdsException.h
    src/client/AdsVariable.h
    src/client/AdsNotificationOOI.h
    src/client/AdsSymbols.h
    src/route/TlsSettings.h
    src/core/AdsDef.h
    src/core/StandaloneAdsDef.h
    src/core/wrap_endian.h
)
target_link_libraries(ads-client PUBLIC Threads::Threads)

if(LIBADS_CLIENT_LOCAL)
    target_sources(ads-client PRIVATE src/local/LocalRouter.cpp)
    target_compile_definitions(ads-client PRIVATE LIBADS_CLIENT_LOCAL=1)
endif()

# Secure ADS in the route way: mbedTLS 3.6, found or fetched (cmake/FetchMbedTLS.cmake).
if(LIBADS_CLIENT_ROUTE)
    target_sources(ads-client PRIVATE
        src/route/AdsLib.cpp
        src/route/AmsPort.cpp
        src/route/AmsRouter.cpp
    )
    target_compile_definitions(ads-client PRIVATE LIBADS_CLIENT_ROUTE=1)
    if(LIBADS_CLIENT_SECURE)
        include(cmake/FetchMbedTLS.cmake)
        target_sources(ads-client PRIVATE src/route/TlsTransport.cpp)
        target_compile_definitions(ads-client PRIVATE LIBADS_CLIENT_SECURE=1)
        target_link_libraries(ads-client PRIVATE MbedTLS::mbedtls)
    endif()
endif()
if(NOT LIBADS_CLIENT_LOCAL AND NOT LIBADS_CLIENT_ROUTE)
    message(FATAL_ERROR "LIBADS_CLIENT_LOCAL and LIBADS_CLIENT_ROUTE are both off: nothing to talk ADS with")
endif()
if(WIN32)
    target_link_libraries(ads-client PUBLIC ws2_32)
    # The public headers pull in winsock2.h, so NOMINMAX has to reach the users as well.
    target_compile_definitions(ads-client PUBLIC NOMINMAX=1 PRIVATE _CRT_NONSTDC_NO_WARNINGS=1 _CRT_SECURE_NO_WARNINGS=1)
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
    # Parts as TRUE/FALSE for the config; Secure ADS only exists with the route way.
    foreach(part LOCAL ROUTE)
        if(LIBADS_CLIENT_${part})
            set(LIBADS_CLIENT_${part}_BOOL TRUE)
        else()
            set(LIBADS_CLIENT_${part}_BOOL FALSE)
        endif()
    endforeach()
    if(LIBADS_CLIENT_ROUTE AND LIBADS_CLIENT_SECURE)
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
    # CTest runs only what needs neither TwinCAT nor a VM: the parts check and the tests against the fake router.
    # The others need TwinCAT and are started by hand (below). Each fake-router test has its own ports, so they can
    # run in parallel (ctest -j). Under WSL Linux and Windows share ports, so Linux uses others by default.
    enable_testing()
    if(WIN32)
        set(LIBADS_CLIENT_TEST_PORT_DEFAULT 48910)
    else()
        set(LIBADS_CLIENT_TEST_PORT_DEFAULT 49110)
    endif()
    set(LIBADS_CLIENT_TEST_PORT ${LIBADS_CLIENT_TEST_PORT_DEFAULT} CACHE STRING
        "First port of the fake-router tests in CTest (uses this one up to +21)")
    add_subdirectory(test/parts)
    # Connection layer and fake router, independent of the parts.
    add_subdirectory(test/fake-router)
    add_subdirectory(test/reconnect-fake)
    add_subdirectory(test/subscriptions-fake)
    add_subdirectory(test/symbols-fake)
    if(LIBADS_CLIENT_ROUTE)
        add_subdirectory(test/route-reconnect-fake)
    endif()

    # Programs against a real TwinCAT system, started by hand. Not part of every source copy, hence only if present.
    function(libads_client_manual_test dir)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/${dir}/CMakeLists.txt)
            add_subdirectory(test/${dir})
        endif()
    endfunction()
    if(LIBADS_CLIENT_LOCAL)
        libads_client_manual_test(tc-control)
        libads_client_manual_test(reconnect)
        libads_client_manual_test(write-control)
        libads_client_manual_test(symbols-plc)
        if(WIN32) # windows.h directly
            libads_client_manual_test(tcadsdll-compat)
        endif()
    endif()
    if(LIBADS_CLIENT_ROUTE)
        libads_client_manual_test(route-client)
    endif()
    if(LIBADS_CLIENT_LOCAL AND LIBADS_CLIENT_ROUTE)
        libads_client_manual_test(local-client) # opens a route port beside the local ones
    endif()
    # These use Winsock directly, no library.
    if(WIN32)
        libads_client_manual_test(router-probe)
        libads_client_manual_test(router-watch)
    endif()
endif()

if(LIBADS_CLIENT_BUILD_EXAMPLES AND WIN32 AND LIBADS_CLIENT_LOCAL AND LIBADS_CLIENT_ROUTE)
    add_subdirectory(examples/symbol-browser)
endif()
if(LIBADS_CLIENT_BUILD_EXAMPLES AND NOT WIN32 AND LIBADS_CLIENT_LOCAL AND LIBADS_CLIENT_ROUTE)
    # Skipped without ncurses (wide characters), e.g. without the package libncurses-dev.
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses QUIET)
    if(CURSES_FOUND)
        add_subdirectory(examples/symbol-browser-tui)
    else()
        message(STATUS "symbol-browser-tui skipped: ncurses with wide characters not found")
    endif()
endif()
