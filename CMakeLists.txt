cmake_minimum_required(VERSION 3.20)
project(lob_market_making CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

option(SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)

add_library(mm src/market_data.cpp src/features.cpp src/simulator.cpp)
target_include_directories(mm PUBLIC include)

add_executable(mmsim src/tools/mmsim_main.cpp)
target_link_libraries(mmsim PRIVATE mm)
add_executable(features src/tools/features_main.cpp)
target_link_libraries(features PRIVATE mm)

add_executable(bench_order_book bench/bench_order_book.cpp)
target_link_libraries(bench_order_book PRIVATE lob)

find_package(GTest REQUIRED)
enable_testing()
add_executable(unit_tests tests/test_order_book.cpp tests/test_market_data.cpp tests/test_features.cpp
               tests/test_simulator.cpp)
target_link_libraries(unit_tests PRIVATE lob mm GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)
