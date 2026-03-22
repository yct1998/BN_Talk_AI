cmake_minimum_required(VERSION 3.20)
project(bntalk_native_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(bntalk_bridge SHARED
    bridge_dll/bridge_dll.cpp
    common/bntalk_protocol.cpp
)

target_include_directories(bntalk_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge_dll
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_compile_definitions(bntalk_bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(bntalk_bridge PROPERTIES OUTPUT_NAME "bntalk_bridge")

add_executable(bntalk_injector
    injector/main.cpp
)

target_compile_definitions(bntalk_injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(bntalk_injector PROPERTIES OUTPUT_NAME "bntalk_injector")

add_executable(bntalk_bridge_client
    bridge_client/main.cpp
)

target_compile_definitions(bntalk_bridge_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(bntalk_bridge_client PROPERTIES OUTPUT_NAME "bntalk_bridge_client")
