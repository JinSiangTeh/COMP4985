cmake_minimum_required(VERSION 3.20)
project(untitled17 C)

set(CMAKE_C_STANDARD 11)

# 1. Look for the ncurses library on the system
find_package(Curses REQUIRED)

add_executable(untitled17 main.c
        client.c
        test.c)

# 2. Link the ncurses library and pthreads to your executable
target_link_libraries(untitled17 PRIVATE ${CURSES_LIBRARIES} pthread)

# 3. Include the ncurses directory so it finds the headers
target_include_directories(untitled17 PRIVATE ${CURSES_INCLUDE_DIRS})