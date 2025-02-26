project(netscanner-lib)

cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

file(GLOB SOURCES "src/*.cpp")

add_library(netscanner ${SOURCES})

target_link_libraries(netscanner PRIVATE esp32 freertos)