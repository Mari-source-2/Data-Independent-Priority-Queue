cmake_minimum_required(VERSION 3.16)
project(data_independent_priority_queues CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g")
add_compile_options(-Wall -Wextra)

find_package(OpenSSL 3.0 REQUIRED)

add_executable(server common/server.cpp)

add_executable(run_workload apps/run_workload.cpp)
target_link_libraries(run_workload OpenSSL::Crypto)

add_executable(dijkstra apps/dijkstra.cpp)
target_link_libraries(dijkstra OpenSSL::Crypto)

add_executable(generator generator/generator.cpp)

add_executable(tamper_test tests/tamper_test.cpp)
target_link_libraries(tamper_test OpenSSL::Crypto)

enable_testing()
add_test(NAME correctness
         COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh ${CMAKE_BINARY_DIR})

add_executable(edge_test tests/edge_test.cpp)
target_link_libraries(edge_test OpenSSL::Crypto)
