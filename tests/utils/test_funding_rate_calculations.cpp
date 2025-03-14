#include <gtest/gtest.h>
#include <exchange/types.h>
#include <chrono>

namespace funding {
namespace testing {

// Helper function to annualize a funding rate based on payment interval
double annualizeFundingRate(double rate, const std::chrono::hours& interval) {
    double hours_per_year = 24.0 * 365.0;
    double payments_per_year = hours_per_year / interval.count();
    return rate * payments_per_year * 100.0; // Convert to percentage
}

// Helper function to calculate estimated profit from funding
double calculateProfitFromFunding(double funding_rate, 
                                  std::chrono::hours interval,
                                  double entry_spread_pct,
                                  double transaction_cost_pct) {
    // Annualize the funding rate
    double annualized_rate = annualizeFundingRate(funding_rate, interval);
    
    // Calculate profit after transaction costs and entry spread
    // We take the absolute value of the annualized rate since we can profit from both positive and negative rates
    // by taking the appropriate position (long or short)
    return std::abs(annualized_rate) - std::abs(entry_spread_pct) - transaction_cost_pct;
}

// Helper to estimate how many funding periods to break even
double periodsToBreakeven(double funding_rate,
                         double transaction_cost_pct) {
    // Each period pays funding_rate as decimal, we need percentage
    double funding_per_period = std::abs(funding_rate) * 100.0;
    
    // Simple division of cost by funding per period
    return transaction_cost_pct / funding_per_period;
}

class FundingRateCalculationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common test values
    }
};

TEST_F(FundingRateCalculationsTest, AnnualizesFundingRateCorrectly) {
    // Test with common interval of 8 hours (like Binance)
    double rate = 0.001; // 0.1% per 8h
    auto interval = std::chrono::hours(8);
    
    double annualized = annualizeFundingRate(rate, interval);
    
    // 0.1% paid every 8 hours = 0.1% * (24*365/8) = 0.1% * 1095 = 109.5%
    EXPECT_NEAR(109.5, annualized, 0.1);
    
    // Test with 1-hour interval (like some other exchanges)
    rate = 0.0001; // 0.01% per 1h
    interval = std::chrono::hours(1);
    
    annualized = annualizeFundingRate(rate, interval);
    
    // 0.01% paid every hour = 0.01% * (24*365) = 0.01% * 8760 = 87.6%
    EXPECT_NEAR(87.6, annualized, 0.1);
}

TEST_F(FundingRateCalculationsTest, CalculatesExpectedProfitCorrectly) {
    // Test with positive funding rate
    double funding_rate = 0.001; // 0.1%
    auto interval = std::chrono::hours(8);
    double entry_spread = 0.2; // 0.2% price difference
    double transaction_cost = 0.1; // 0.1% total cost
    
    double profit = calculateProfitFromFunding(funding_rate, interval, entry_spread, transaction_cost);
    
    // Expected profit = annualized_rate - spread - transaction_cost
    // = 109.5% - 0.2% - 0.1% = 109.2%
    EXPECT_NEAR(109.2, profit, 0.1);
    
    // Test with negative funding rate (should use absolute value of rate)
    funding_rate = -0.001; // -0.1%
    profit = calculateProfitFromFunding(funding_rate, interval, entry_spread, transaction_cost);
    
    // Should still be positive because we're shorting the perpetual
    EXPECT_NEAR(109.2, profit, 0.1);
    
    // Test where costs exceed funding rate
    funding_rate = 0.00005; // 0.005%
    profit = calculateProfitFromFunding(funding_rate, interval, entry_spread, transaction_cost);
    
    // Annualized = 0.005% * (24*365/8) = 0.005% * 1095 = 5.475%
    // 5.475% - 0.2% - 0.1% = 5.175%
    EXPECT_NEAR(5.175, profit, 0.1);
}

TEST_F(FundingRateCalculationsTest, CalculatesBreakevenCorrectly) {
    // Test with standard values
    double funding_rate = 0.001; // 0.1%
    double transaction_cost = 0.1; // 0.1%
    
    double periods = periodsToBreakeven(funding_rate, transaction_cost);
    
    // 0.1% transaction cost / 0.1% funding per period = 1 period
    EXPECT_NEAR(1.0, periods, 0.001);
    
    // Test with lower funding rate
    funding_rate = 0.0002; // 0.02%
    periods = periodsToBreakeven(funding_rate, transaction_cost);
    
    // 0.1% transaction cost / 0.02% funding per period = 5 periods
    EXPECT_NEAR(5.0, periods, 0.001);
    
    // Test with higher transaction cost
    funding_rate = 0.001; // 0.1%
    transaction_cost = 0.3; // 0.3%
    periods = periodsToBreakeven(funding_rate, transaction_cost);
    
    // 0.3% transaction cost / 0.1% funding per period = 3 periods
    EXPECT_NEAR(3.0, periods, 0.001);
}

} // namespace testing
} // namespace funding 