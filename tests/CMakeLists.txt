# Tests CMakeLists.txt
include(FetchContent)

# Fetch Google Test
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0  # Using the latest stable release
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Define test executables
set(TEST_SOURCES
    test_main.cpp
    exchange/test_exchange_config.cpp
    strategy/test_same_exchange_spot_perp_strategy.cpp
    strategy/test_cross_exchange_perp_strategy.cpp 
    strategy/test_cross_exchange_spot_perp_strategy.cpp
    strategy/test_composite_strategy.cpp
    risk/test_risk_manager.cpp
    utils/test_funding_rate_calculations.cpp
)

# Create test executable
add_executable(funding_tests ${TEST_SOURCES})

# Link test executable with Google Test and project libraries
target_link_libraries(funding_tests PRIVATE
    gtest
    gtest_main
    Threads::Threads
    CURL::libcurl
    nlohmann_json::nlohmann_json
    funding_core  # We'll create this library to share code between main app and tests
)

# Discover tests
include(GoogleTest)
gtest_discover_tests(funding_tests) 