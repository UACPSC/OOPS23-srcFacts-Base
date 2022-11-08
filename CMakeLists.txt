# @file CMakeLists.txt
# 
# CMake files for the srcFacts project

# Can be run by any version of CMake >= this
cmake_minimum_required(VERSION 3.20)

# project info
project(srcFacts)

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use Release build type if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Source files for the main program srcFacts
set(SOURCE srcFacts.cpp refillBuffer.cpp)

# srcFact application
add_executable(srcfacts ${SOURCE})

# cmake .. -DTRACE=ON|OFF
if(DEFINED TRACE)
    message(STATUS "TRACE is ${TRACE}")
    if(TRACE)
        target_compile_definitions(srcfacts PUBLIC TRACE)
    endif()
endif()

# Turn on warnings
target_compile_options(srcfacts PRIVATE
     $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wall -Wno-unused-variable>
     $<$<CXX_COMPILER_ID:MSVC>: /W4>
)

# Extract the demo input srcML file
file(ARCHIVE_EXTRACT INPUT ${CMAKE_SOURCE_DIR}/demo.xml.zip)

# Demo run command
add_custom_target(run
        COMMENT "Run demo"
        COMMAND $<TARGET_FILE:srcfacts> < demo.xml
        DEPENDS srcfacts
        USES_TERMINAL
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
