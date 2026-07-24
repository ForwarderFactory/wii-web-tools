cmake_minimum_required(VERSION 4.2)
project(wii-web-tools)

set(CMAKE_CXX_STANDARD 23)

find_package(netkit REQUIRED)
find_package(nlohmann_json REQUIRED)

set(sources
        src/main.cpp
)

add_executable(wii-web-tools ${sources})

target_include_directories(wii-web-tools PRIVATE
        include
)

target_link_libraries(wii-web-tools PRIVATE
        netkit::netkit
        nlohmann_json::nlohmann_json
)