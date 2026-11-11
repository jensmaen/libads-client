# libads in the version LIBADS_CLIENT_LIBADS_VERSION, taken from the first of:
#   1. the targets of a project that added libads before (add_subdirectory, FetchContent, find_package),
#   2. a source folder libads beside this one,
#   3. an installed package (find_package, CMAKE_PREFIX_PATH),
#   4. its Git tag, if LIBADS_CLIENT_FETCH_LIBADS is on.
# libads-client uses inner headers of libads (libads::ads-internal), so only exactly this version fits.

if(TARGET libads::ads)
    get_target_property(version libads::ads LIBADS_VERSION)
    if(NOT version)
        set(version ${libads_VERSION})
    endif()
    if(NOT version VERSION_EQUAL LIBADS_CLIENT_LIBADS_VERSION)
        message(FATAL_ERROR "libads-client ${PROJECT_VERSION} needs libads ${LIBADS_CLIENT_LIBADS_VERSION}, "
            "not '${version}'")
    endif()
    return()
endif()

# In a function, so that the settings for the libads subproject do not leak into the caller. Installed together
# with libads-client, libads has to be installed, too.
function(libads_client_add_libads)
    set(LIBADS_INSTALL ${LIBADS_CLIENT_INSTALL})
    set(LIBADS_BUILD_TESTS OFF)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../libads/CMakeLists.txt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libads ${CMAKE_CURRENT_BINARY_DIR}/libads)
        return()
    endif()
    find_package(libads ${LIBADS_CLIENT_LIBADS_VERSION} EXACT CONFIG QUIET)
    if(libads_FOUND)
        return()
    endif()
    if(NOT LIBADS_CLIENT_FETCH_LIBADS)
        message(FATAL_ERROR
            "libads-client needs libads ${LIBADS_CLIENT_LIBADS_VERSION}, which was not found. Either install it "
            "(and point CMAKE_PREFIX_PATH to it) or allow downloading it with -DLIBADS_CLIENT_FETCH_LIBADS=ON.")
    endif()
    include(FetchContent)
    FetchContent_Declare(libads
        GIT_REPOSITORY https://gitlab.com/fresenborg-development/libads.git
        GIT_TAG v${LIBADS_CLIENT_LIBADS_VERSION}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libads)
endfunction()
libads_client_add_libads()

if(NOT TARGET libads::ads)
    message(FATAL_ERROR "libads ${LIBADS_CLIENT_LIBADS_VERSION}: neither found nor fetched (no target libads::ads)")
endif()
