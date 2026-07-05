cmake_minimum_required(VERSION 3.15)
project(SignalLogger VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# MSVC specific settings
if(MSVC)
    add_compile_options(/W4 /WX-) # High warning level
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# Directories
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Include directories
include_directories(${INC_DIR})

# Source files
set(SOURCES
    ${SRC_DIR}/main.cpp
)

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})
