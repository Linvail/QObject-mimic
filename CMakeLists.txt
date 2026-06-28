cmake_minimum_required(VERSION 3.11)
project(GSignals)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

# Integrate header-only Boost library using FetchContent
include(FetchContent)

FetchContent_Declare(
    boost
    URL https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz
    URL_HASH SHA256=a5800f405508f5df8114558ca9855d2640a2de8f0445f051fa1c7c3383045724
)

FetchContent_GetProperties(boost)
if(NOT boost_POPULATED)
    message(STATUS "Fetching Boost 1.84.0 header-only...")
    FetchContent_Populate(boost)
endif()

# Define header-only interface library for Boost headers
add_library(Boost INTERFACE)
target_include_directories(Boost INTERFACE ${boost_SOURCE_DIR})

add_executable(GSignals
    main.cpp
    GAbstractEventDispatcher.cpp
    GThread.cpp
    GObject.cpp
    GCoreApplication.cpp
)

target_link_libraries(GSignals PRIVATE Boost)
