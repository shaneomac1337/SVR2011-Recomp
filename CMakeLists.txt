# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(svr2011 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(SVR2011_SOURCES
    src/main.cpp
    src/d3d_census.cpp
    src/native/native_renderer.cpp
)

if(WIN32)
    add_executable(svr2011 WIN32 ${SVR2011_SOURCES})
else()
    add_executable(svr2011 ${SVR2011_SOURCES})
endif()

rexglue_setup_target(svr2011 GPU_PLUGINS xenos)
target_include_directories(svr2011 PRIVATE src)

# The v0.10.0 source-tree runtime does not propagate ImGui's include directory
# to the host, although its public UI headers include imgui.h.
if(REXSDK_DIR)
    target_include_directories(svr2011 PRIVATE
        $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)
    # The native renderer includes the SDK's Vulkan headers, which include RenderDoc's.
    target_include_directories(svr2011 PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)
endif()
