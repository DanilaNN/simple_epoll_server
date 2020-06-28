cmake_minimum_required(VERSION 2.8)

project(epoll_server)

set(SOURCE_EXE main.c)

set(SOURCE_LIB http_parser.c)

add_library(http_parser STATIC ${SOURCE_LIB})

add_executable(server ${SOURCE_EXE})

target_link_libraries(server http_parser)

