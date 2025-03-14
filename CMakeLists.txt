cmake_minimum_required(VERSION 3.10)
project(FundingRateArbitrage VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to build tests
option(BUILD_TESTING "Build the testing tree" ON)

# Third-party dependencies
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

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

# Find libwebsockets using pkg-config as fallback
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

# Include directories
include_directories(include ${LIBWEBSOCKETS_INCLUDE_DIRS})

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
    OpenSSL::Crypto
    ${LIBWEBSOCKETS_LIBRARIES}
)

# Main executable
add_executable(funding_bot src/main.cpp)

# Link the executable to the library
target_link_libraries(funding_bot PRIVATE funding_core)

# Add WebSocket test
add_executable(websocket_test src/test/websocket_test.cpp)
target_link_libraries(websocket_test PRIVATE funding_core)
target_include_directories(websocket_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Install
install(TARGETS funding_bot DESTINATION bin)

# Testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif() 