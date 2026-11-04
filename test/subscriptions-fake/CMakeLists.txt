# Notifications with own handles in both parts against the fake router in the same process; no TwinCAT needed.
add_executable(subscriptions-fake main.cpp)
target_compile_features(subscriptions-fake PRIVATE cxx_std_17)
target_link_libraries(subscriptions-fake PRIVATE libads-client::ads-client)
target_compile_definitions(subscriptions-fake PRIVATE
    LIBADS_CLIENT_TEST_LOCAL=$<BOOL:${LIBADS_CLIENT_LOCAL}>
    LIBADS_CLIENT_TEST_ROUTE=$<BOOL:${LIBADS_CLIENT_ROUTE}>)
if(WIN32)
    target_compile_definitions(subscriptions-fake PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

math(EXPR port "${LIBADS_CLIENT_TEST_PORT} + 10")
add_test(NAME subscriptions-fake COMMAND subscriptions-fake ${port})
set_tests_properties(subscriptions-fake PROPERTIES TIMEOUT 180 LABELS fake-router)
