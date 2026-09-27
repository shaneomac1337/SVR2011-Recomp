# svr2011 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(svr2011 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SVR2011_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(svr2011 WIN32 ${SVR2011_SOURCES})
else()
    add_executable(svr2011 ${SVR2011_SOURCES})
endif()

rexglue_setup_target(svr2011 GPU_PLUGINS xenos)
