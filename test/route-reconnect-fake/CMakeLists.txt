# Route way reconnecting through the C-API against the fake router in the same process; no TwinCAT needed.
add_executable(route-reconnect-fake main.cpp)
target_compile_features(route-reconnect-fake PRIVATE cxx_std_17)
target_include_directories(route-reconnect-fake PRIVATE ${LIBADS_CLIENT_SHARED_TESTS}/fake-router)
target_link_libraries(route-reconnect-fake PRIVATE libads-client::ads-client)
if(WIN32)
    target_compile_definitions(route-reconnect-fake PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

math(EXPR port "${LIBADS_CLIENT_TEST_PORT} + 20")
add_test(NAME route-reconnect-fake COMMAND route-reconnect-fake ${port})
set_tests_properties(route-reconnect-fake PROPERTIES TIMEOUT 180 LABELS fake-router)
