cmake_minimum_required(VERSION 3.10)
project(FundingRateArbitrage VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to build tests
option(BUILD_TESTING "Build the testing tree" ON)

# Third-party dependencies
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# Find or fetch nlohmann_json if not found
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Include directories
include_directories(include)

# Add source files (excluding main.cpp for the library)
file(GLOB_RECURSE LIB_SOURCES "src/*.cpp")
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/main.cpp$")

# Create a library target that can be used by both the main executable and tests
add_library(funding_core STATIC ${LIB_SOURCES})

# Link library dependencies
target_link_libraries(funding_core PUBLIC
    Threads::Threads
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# Main executable
add_executable(funding_bot src/main.cpp)

# Link the executable to the library
target_link_libraries(funding_bot PRIVATE funding_core)

# Install
install(TARGETS funding_bot DESTINATION bin)

# Testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif() 