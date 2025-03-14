#include <gtest/gtest.h>
#include <iostream>

int main(int argc, char** argv) {
    std::cout << "Running funding rate arbitrage bot tests\n";
    
    // Initialize Google Test
    ::testing::InitGoogleTest(&argc, argv);
    
    // Run all tests
    return RUN_ALL_TESTS();
} 