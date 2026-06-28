cmake_minimum_required(VERSION 3.10)
project(GSignals)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

add_executable(GSignals
    main.cpp
    GAbstractEventDispatcher.cpp
    GThread.cpp
    GObject.cpp
    GCoreApplication.cpp
)
