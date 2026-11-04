# Reconnecting connection against the fake router in the same process; no TwinCAT needed.
add_executable(reconnect-fake main.cpp)
target_compile_features(reconnect-fake PRIVATE cxx_std_17)
target_link_libraries(reconnect-fake PRIVATE libads-client::ads-client)

math(EXPR port "${LIBADS_CLIENT_TEST_PORT} + 0")
add_test(NAME reconnect-fake COMMAND reconnect-fake ${port})
set_tests_properties(reconnect-fake PROPERTIES TIMEOUT 180 LABELS fake-router)
