# mbedTLS 3.6 for Secure ADS. Version 3.6, not 4.x: TwinCAT negotiates DHE-RSA, which mbedTLS 4 no longer has.
#
# An installed mbedTLS 3.6 (vcpkg, distribution package, own build) is taken first. Otherwise, if
# LIBADS_CLIENT_FETCH_MBEDTLS is on, the release below is downloaded, checked against its SHA256 and built as a
# subproject. Either way the target MbedTLS::mbedtls exists afterwards.
#
# A new mbedTLS 3.6 release (security fixes): change version and hash here and release a new libads-client version.
set(LIBADS_CLIENT_MBEDTLS_VERSION 3.6.7)
set(LIBADS_CLIENT_MBEDTLS_SHA256 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)

if(NOT LIBADS_CLIENT_FETCH_MBEDTLS)
    find_package(MbedTLS 3.6 QUIET)
    if(NOT MbedTLS_FOUND)
        message(FATAL_ERROR
            "Secure ADS needs mbedTLS 3.6, which was not found. Either install it (and point CMAKE_PREFIX_PATH to it), "
            "allow downloading it with -DLIBADS_CLIENT_FETCH_MBEDTLS=ON, or build without Secure ADS with "
            "-DLIBADS_CLIENT_SECURE=OFF.")
    endif()
    return()
endif()

include(FetchContent)

# In a function, so that the settings for the mbedTLS subproject do not leak into the caller.
function(libads_client_fetch_mbedtls)
    # mbedTLS asks for CMake 3.5.1; without this its option() calls would ignore the variables below.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    # Its cmake_minimum_required(3.5.1) is deprecated in CMake 4 (warning nobody can act on); policies as of 3.10.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(ENABLE_PROGRAMS OFF)
    set(ENABLE_TESTING OFF)
    set(MBEDTLS_FATAL_WARNINGS OFF)
    set(USE_STATIC_MBEDTLS_LIBRARY ON)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF)
    # The installed package of libads-client needs mbedTLS beside it (find_dependency in the config).
    if(LIBADS_CLIENT_INSTALL)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF)
    else()
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON)
    endif()

    FetchContent_Declare(MbedTLS
        URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${LIBADS_CLIENT_MBEDTLS_VERSION}/mbedtls-${LIBADS_CLIENT_MBEDTLS_VERSION}.tar.bz2"
        URL_HASH SHA256=${LIBADS_CLIENT_MBEDTLS_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        FIND_PACKAGE_ARGS 3.6)
    FetchContent_MakeAvailable(MbedTLS)
endfunction()
libads_client_fetch_mbedtls()

if(NOT TARGET MbedTLS::mbedtls)
    message(FATAL_ERROR "mbedTLS 3.6: neither found nor fetched (no target MbedTLS::mbedtls)")
endif()
