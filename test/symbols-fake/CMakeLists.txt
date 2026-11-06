# Symbol and data type uploads taken apart, on data built after the format of TwinCAT; no TwinCAT, no network.
add_executable(symbols-fake main.cpp)
target_compile_features(symbols-fake PRIVATE cxx_std_17)
target_link_libraries(symbols-fake PRIVATE libads-client::ads-client)

add_test(NAME symbols-fake COMMAND symbols-fake)
set_tests_properties(symbols-fake PROPERTIES TIMEOUT 60)
if(WIN32)
    target_compile_definitions(symbols-fake PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
