cmake_minimum_required(VERSION 3.16)
project(PalworldVRInjector VERSION 0.1 LANGUAGES CXX)

# Compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

option(VRHOOK_DEBUG "Enable debug logging" OFF)

add_library(palworld_vr_hook SHARED vrhook.cpp)

target_include_directories(palworld_vr_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Link against required libraries
target_link_libraries(palworld_vr_hook PRIVATE dl pthread)

# Add debug flag if requested
if(VRHOOK_DEBUG)
    target_compile_definitions(palworld_vr_hook PRIVATE VRHOOK_DEBUG=1)
endif()

# Install the library
install(TARGETS palworld_vr_hook LIBRARY DESTINATION lib)
install(FILES include/vrhook.h DESTINATION include)
