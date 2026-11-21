cmake_minimum_required(VERSION 3.16)
project(lua_draw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)
find_package(Lua 5.1 REQUIRED)

add_library(draw MODULE draw.c)

target_include_directories(draw PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(draw PRIVATE ${LUA_LIBRARIES} SDL2::SDL2)

if(UNIX)
    target_link_libraries(draw PRIVATE m)
endif()

set_target_properties(draw PROPERTIES
    PREFIX ""
    OUTPUT_NAME "draw"
)

if(WIN32)
    set_target_properties(draw PROPERTIES SUFFIX ".dll")
else()
    set_target_properties(draw PROPERTIES SUFFIX ".so")
endif()
