# Minimal program that uses libads-client the three usual ways:
#   LIBADS_CONSUMER_MODE=package  installed library, find_package() (CMAKE_PREFIX_PATH to the installation)
#   LIBADS_CONSUMER_MODE=fetch    FetchContent from the Git tag (FETCHCONTENT_SOURCE_DIR_LIBADS-CLIENT for a local copy)
#   LIBADS_CONSUMER_MODE=subdir   add_subdirectory() of a source copy (LIBADS_CLIENT_DIR)
cmake_minimum_required(VERSION 3.25)
project(libads-client-consumer LANGUAGES CXX)

set(LIBADS_CONSUMER_MODE package CACHE STRING "package, fetch or subdir")

if(LIBADS_CONSUMER_MODE STREQUAL "package")
    find_package(libads-client 0.1 CONFIG REQUIRED COMPONENTS local route)
    message(STATUS "libads-client ${libads-client_VERSION}: local ${libads-client_LOCAL}, route ${libads-client_ROUTE}, "
        "secure ${libads-client_SECURE}")
elseif(LIBADS_CONSUMER_MODE STREQUAL "fetch")
    include(FetchContent)
    FetchContent_Declare(libads-client
        GIT_REPOSITORY https://gitlab.com/fresenborg-development/libads-client.git
        GIT_TAG v0.2.1)
    FetchContent_MakeAvailable(libads-client)
elseif(LIBADS_CONSUMER_MODE STREQUAL "subdir")
    set(LIBADS_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Source folder of libads-client")
    add_subdirectory(${LIBADS_CLIENT_DIR} libads-client)
else()
    message(FATAL_ERROR "LIBADS_CONSUMER_MODE: package, fetch or subdir, not '${LIBADS_CONSUMER_MODE}'")
endif()

add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE libads-client::ads-client)
