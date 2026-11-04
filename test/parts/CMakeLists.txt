# The parts that are switched on or off behave as documented; no TwinCAT, no network.
add_executable(parts main.cpp)
target_compile_features(parts PRIVATE cxx_std_17)
target_link_libraries(parts PRIVATE libads-client::ads-client)
target_compile_definitions(parts PRIVATE
    LIBADS_CLIENT_TEST_LOCAL=$<BOOL:${LIBADS_CLIENT_LOCAL}>
    LIBADS_CLIENT_TEST_ROUTE=$<BOOL:${LIBADS_CLIENT_ROUTE}>
    LIBADS_CLIENT_TEST_SECURE=$<AND:$<BOOL:${LIBADS_CLIENT_ROUTE}>,$<BOOL:${LIBADS_CLIENT_SECURE}>>)

add_test(NAME parts COMMAND parts)
set_tests_properties(parts PROPERTIES TIMEOUT 30)
