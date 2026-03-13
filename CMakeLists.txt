cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)             # Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD_REQUIRED ON)    # Make the standard required
set(CMAKE_CXX_EXTENSIONS OFF)          # Disable compiler-specific extensions

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")    # Debug build flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3")     # Release build flags

# Define the project name
project(tests)

# Define the main executable target
add_executable(tests)

# Use address sanitizer for detecting memory leak and other memory errors.
# But only in Debug builds.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(tests PRIVATE -fsanitize=address)
  target_link_libraries(tests PRIVATE -fsanitize=address)
endif()

# Add source files from subdirectories
file(GLOB SOURCES 
"*.cpp"  
"tests/*.cpp"
"tests/testdata/*.cpp"
"codecs/*.cpp" 
"codecs/filters/*.cpp" 
"codecs/compressors/*.cpp"
"codecs/squelchers/*.cpp"
) 
target_sources(tests PRIVATE ${SOURCES})

