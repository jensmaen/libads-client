# Fake TwinCAT router for the reconnect tests, as a program; independent of the library.
add_executable(fake-router main.cpp)
target_compile_features(fake-router PRIVATE cxx_std_17)
target_link_libraries(fake-router PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(fake-router PRIVATE ws2_32)
    target_compile_definitions(fake-router PRIVATE _CRT_SECURE_NO_WARNINGS=1)
endif()
