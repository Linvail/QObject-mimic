cmake_minimum_required(VERSION 3.11)
project(GSignals)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

# Integrate Boost library from git submodule
add_subdirectory(submodule/external/boost EXCLUDE_FROM_ALL)

# Define header-only interface library for Boost headers
add_library(Boost INTERFACE)
target_link_libraries(Boost INTERFACE Boost::signals2)

add_executable(GSignals
    main.cpp
    GAbstractEventDispatcher.cpp
    GThread.cpp
    GObject.cpp
    GCoreApplication.cpp
)

target_link_libraries(GSignals PRIVATE Boost)
