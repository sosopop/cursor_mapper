cmake_minimum_required(VERSION 3.20)
project(cursor_mapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(cursor_mapper src/main.cpp)

target_compile_definitions(cursor_mapper PRIVATE
    _WIN32_WINNT=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(cursor_mapper PRIVATE user32 shcore)

# Embed DPI-awareness manifest
set_target_properties(cursor_mapper PROPERTIES
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:\"${CMAKE_SOURCE_DIR}/app.manifest\""
)
