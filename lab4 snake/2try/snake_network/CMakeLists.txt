cmake_minimum_required(VERSION 3.10)
project(snake_network)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

find_package(SFML 2.5 COMPONENTS graphics window system REQUIRED)
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS} src)

add_executable(snake_network
    src/main.cpp
    src/net.cpp
    src/game.cpp
    src/snakes.pb.cc  # <-- сгенерируй перед сборкой
)

target_link_libraries(snake_network sfml-graphics sfml-window sfml-system ${Protobuf_LIBRARIES} pthread)
