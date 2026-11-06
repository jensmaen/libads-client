# Terminal program that shows all variables of a PLC with their values (AdsSymbols.h); needs both parts and ncurses
# with wide characters.
add_executable(symbol-browser-tui main.cpp)
target_compile_features(symbol-browser-tui PRIVATE cxx_std_14)
target_include_directories(symbol-browser-tui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${CURSES_INCLUDE_DIRS})
target_compile_options(symbol-browser-tui PRIVATE ${CURSES_CFLAGS})
target_link_libraries(symbol-browser-tui PRIVATE libads-client::ads-client ${CURSES_LIBRARIES})
