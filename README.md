# Cryptocurrency Funding Rate Arbitrage Bot

A C++ application for detecting and automatically executing funding rate arbitrage opportunities across multiple cryptocurrency exchanges.

## Overview

This bot identifies funding rate discrepancies between spot/margin and perpetual futures markets, as well as between perpetual futures on different exchanges. It automatically executes hedged positions to capture funding rate payments while maintaining market-neutral exposure.

## Supported Strategies

1. Spot/Margin vs Perpetual Futures (same exchange)
2. Perpetual Futures vs Perpetual Futures (different exchanges)
3. Spot/Margin vs Perpetual Futures (different exchanges)

## Key Features

- Support for multiple exchanges (minimum 2, up to 5)
- Dynamic handling of different funding rate payment schedules (1h, 4h, 8h)
- Risk management for position unwinding
- Real-time monitoring and execution
- Configurable risk parameters

## Requirements

- C++17 compatible compiler
- CMake 3.10+
- CURL library
- JSON library (nlohmann/json)
- WebSocket library
- Exchange API credentials

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Configuration

Create a `config.json` file in the `config` directory with your exchange API keys and strategy parameters.

## Running

```bash
./funding_bot
```

## Architecture

- Exchange Connectors: Interface with exchange APIs
- Data Collection: Monitor funding rates and market conditions
- Opportunity Detection: Find profitable discrepancies
- Risk Management: Position sizing and risk controls
- Execution Engine: Place and manage trades
- Configuration System: Flexible settings

## License

[Your License] 