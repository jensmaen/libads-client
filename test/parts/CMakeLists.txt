# The route way through the C-API of libads, Secure ADS on or off as built; no TwinCAT, no network.
add_executable(parts main.cpp)
target_compile_features(parts PRIVATE cxx_std_17)
target_link_libraries(parts PRIVATE libads-client::ads-client)
target_compile_definitions(parts PRIVATE LIBADS_CLIENT_TEST_SECURE=$<BOOL:${LIBADS_CLIENT_SECURE}>)

add_test(NAME parts COMMAND parts)
set_tests_properties(parts PROPERTIES TIMEOUT 30)
