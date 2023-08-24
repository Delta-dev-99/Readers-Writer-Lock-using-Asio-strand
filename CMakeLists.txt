cmake_minimum_required(VERSION 3.20.0)
project(scratch9 VERSION 0.1.0)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_LIST_DIR}/cmake/modules/")


# find_package(Boost REQUIRED)
find_package(ASIO REQUIRED)

set(target_name scratch9)
add_executable(${target_name} main.cpp)
target_compile_features(${target_name} PRIVATE cxx_std_20)
target_link_libraries(${target_name}
    ASIO::ASIO
)