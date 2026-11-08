cmake_minimum_required(VERSION 3.16)
project(wirekit_package_consumer LANGUAGES CXX)

find_package(wirekit 0.1 CONFIG REQUIRED)

enable_testing()
foreach(mode IN ITEMS wirekit single_thread)
  add_executable(consumer_${mode} main.cpp)
  target_link_libraries(consumer_${mode} PRIVATE wirekit::${mode})
  add_test(NAME consumer_${mode} COMMAND consumer_${mode})
endforeach()