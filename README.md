# RTEX - Triangular Arbitrage Trading System

A low-latency cryptocurrency trading system for triangular arbitrage on Binance, implemented in C++ with FIX protocol connectivity.

## Strategy Overview

The system executes **triangular arbitrage** - exploiting price inefficiencies across three trading pairs that form a cycle back to the starting asset.

### Example

Starting with USDT, if the following trades yield more than 100 USDT:
```
100 USDT → BUY BTC/USDT → 0.001 BTC → SELL BTC/ETH → 0.05 ETH → SELL ETH/USDT → 100.05 USDT
```
The 0.05 USDT profit (minus fees) represents an arbitrage opportunity.

### Execution Flow

1. **Initialization**: Fetch exchange info (symbols, filters, fees) via REST API
2. **Route Discovery**: Compute all valid 3-leg arbitrage paths for the starting asset
3. **Market Data**: Subscribe to best bid/ask (BookTicker) via FIX for all relevant symbols
4. **Detection**: On each market data update:
   - Fast matrix evaluation screens all paths for approximate profitability
   - Top-K candidates undergo detailed validation (filters, rounding, fees)
   - Version counter ensures prices haven't changed during evaluation
5. **Execution**: If profitable path found, execute three market orders sequentially

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              INITIALIZATION                                  │
│  REST API → Exchange Info (symbols, filters) → Account Balances             │
│  FIX Connect → Feeder (MD) + Broker (OE) → Subscribe to BookTicker          │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           MARKET DATA PIPELINE                               │
│                                                                              │
│  ┌─────────────┐    ┌──────────────────┐    ┌────────────────────┐          │
│  │ FIX Feeder  │───▶│ MarketDataStore  │───▶│  CoalescingBuffer  │          │
│  │ (messages)  │    │ (bid/ask + ver#) │    │ (latest per symbol)│          │
│  └─────────────┘    └──────────────────┘    └─────────┬──────────┘          │
│                                                       │                      │
└───────────────────────────────────────────────────────┼──────────────────────┘
                                                        │
                                                        ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           SIGNAL DETECTION                                   │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────┐        │
│  │              Stage 1: Matrix Path Evaluation                     │        │
│  │  • Pre-computed coefficients (symbol indices, bid/ask flags)     │        │
│  │  • O(n) approximate PnL for ALL paths                            │        │
│  │  • Filter: keep paths where PnL > 0                              │        │
│  └──────────────────────────────┬──────────────────────────────────┘        │
│                                 │ Top-K candidates                           │
│                                 ▼                                            │
│  ┌─────────────────────────────────────────────────────────────────┐        │
│  │              Stage 2: Sequential Validation                      │        │
│  │  • Query current bid/ask prices                                  │        │
│  │  • Apply exchange filters (LOT_SIZE, NOTIONAL, etc.)             │        │
│  │  • Round quantities to valid step sizes                          │        │
│  │  • Calculate exact PnL after fees                                │        │
│  │  • Staleness check (version counter) - abort if data changed     │        │
│  └──────────────────────────────┬──────────────────────────────────┘        │
│                                 │ Best validated signal                      │
└─────────────────────────────────┼───────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           ORDER EXECUTION                                    │
│                                                                              │
│  For each order in path (sequentially):                                      │
│    1. Validate order against exchange filters                                │
│    2. Submit MARKET order via FIX Broker                                     │
│    3. Wait for fill confirmation (5s timeout)                                │
│    4. Abort if order not filled                                              │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Key Components

### Market Data Handling

| Component | File | Purpose |
|-----------|------|---------|
| **Feeder** | `include/fix/Feeder.h` | FIX Market Data handler, parses snapshots/incremental updates |
| **MarketDataStore** | `include/fix/MarketDataStore.h` | Thread-safe bid/ask storage with atomic version counter |
| **CoalescingBuffer** | `include/fix/CoalescingBuffer.h` | Batches updates, keeps only latest per symbol |

### Strategy Logic

| Component | File | Purpose |
|-----------|------|---------|
| **TriangularArb** | `include/strategies/TriangularArb.h` | Main strategy class - initialization, detection, execution |
| **MatrixPathEvaluator** | `include/strategies/MatrixPathEvaluator.h` | Fast O(n) path screening with pre-computed coefficients |
| **OrderSizer** | `include/fin/OrderSizer.h` | Validates and rounds orders to exchange filter requirements |
| **SymbolFilters** | `include/fin/SymbolFilters.h` | Parses and applies Binance filter rules (LOT_SIZE, NOTIONAL, etc.) |

### Order Management

| Component | File | Purpose |
|-----------|------|---------|
| **Broker** | `include/fix/Broker.h` | FIX Order Entry handler, submits orders and tracks execution |

## Matrix Path Evaluation

The `MatrixPathEvaluator` provides fast approximate PnL computation by pre-computing path coefficients at initialization.

### Pre-computed Coefficients

For each arbitrage path, we store:
- **Symbol indices**: Map symbol names to array indices for O(1) price lookup
- **Bid/Ask flags**: `true` for SELL (use bid), `false` for BUY (use ask)
- **Fee multiplier**: Product of `(1 - fee/100)` for all legs

### Evaluation Algorithm

```cpp
for each path:
    amount = initial_stake
    for each leg:
        if SELL:
            amount = amount * bid_price[leg]   // base → quote
        else: // BUY
            amount = amount / ask_price[leg]   // quote → base
    amount *= fee_multiplier
    pnl = amount - initial_stake
```

This ignores filter validation for speed - detailed validation happens only for top-K candidates.

## Staleness Detection

The `MarketDataStore` maintains an atomic version counter incremented on every update. During path evaluation:

1. Capture version at start: `startVersion = store.version()`
2. After each leg, check: `if (store.version() != startVersion) abort`

This prevents executing on stale prices when market data changes during multi-leg evaluation.

## Configuration

Create an INI file (see `config/test_config.ini` for example):

```ini
[TRIANGULAR_ARB_STRATEGY]
# Asset to start and end arbitrage cycles with
startingAsset=USDT

# Default trading fee percentage (0.1 = 0.1%)
defaultFee=0.1

# Percentage of balance to use per arbitrage (1.0 = 100%)
risk=1.0

# true = send real orders, false = simulate fills
liveMode=false

[FIX_CONNECTION]
# FIX Market Data endpoint
mdEndpoint=fix-md.testnet.binance.vision
mdPort=9000

# FIX Order Entry endpoint
oeEndpoint=fix-oe.testnet.binance.vision
oePort=9000

# REST API endpoint (for exchange info and account data)
restEndpoint=testnet.binance.vision

# API credentials
apiKey=YOUR_API_KEY
ed25519KeyPath=config/your_key.pem

[SYMBOL_FEES]
# Optional per-symbol fee overrides (e.g., for BNB discount)
BNBUSDT=0.075
BNBBTC=0.075
```

### Configuration Parameters

| Section | Parameter | Description | Default |
|---------|-----------|-------------|---------|
| `TRIANGULAR_ARB_STRATEGY` | `startingAsset` | Base asset for arbitrage cycles | Required |
| | `defaultFee` | Default fee % for all symbols | 0.1 |
| | `risk` | Fraction of balance to use | 1.0 |
| | `liveMode` | Enable live trading | false |
| `FIX_CONNECTION` | `mdEndpoint` | FIX Market Data server | Required |
| | `mdPort` | FIX MD port | 9000 |
| | `oeEndpoint` | FIX Order Entry server | Required |
| | `oePort` | FIX OE port | 9000 |
| | `restEndpoint` | REST API endpoint | Required |
| | `apiKey` | API key | Required |
| | `ed25519KeyPath` | Path to ED25519 private key | Required |
| `SYMBOL_FEES` | `<SYMBOL>` | Per-symbol fee override | - |

## Building

```bash
# Configure with vcpkg toolchain
cmake --preset release

# Build
cmake --build build/release

# The binary will be at build/release/trader
```

## Running

```bash
./trader --config /path/to/config.ini
```

### Test Mode (Recommended First)

Set `liveMode=false` in config to simulate order fills without sending real orders. This allows testing the detection logic safely.

### Live Mode

Set `liveMode=true` to send real orders to the exchange. **Use with caution** - ensure:
1. API key has trading permissions
2. Account has sufficient balance in the starting asset
3. `risk` parameter is set appropriately (start small)

## Performance Optimizations

The system is designed for low-latency arbitrage detection:

1. **Two-Stage Screening**: Fast matrix evaluation O(n) filters thousands of paths, then detailed validation only for top-K candidates
2. **Coalescing Buffer**: During high-frequency updates, only the latest price per symbol is processed
3. **Pre-computed Coefficients**: Symbol-to-index mapping eliminates string lookups in the hot path
4. **Lock-free Polling**: Atomic `hasUpdates()` check avoids mutex contention
5. **Version Counter**: Lightweight staleness detection without locking
6. **Direct MarketDataStore Access**: Strategy queries prices directly from store, no queue delays

## File Structure

```
runner/
├── include/
│   ├── common/          # Utilities (Scheduler)
│   ├── fin/             # Financial types (Order, Symbol, Signal, Filters)
│   ├── fix/             # FIX protocol (Feeder, Broker, MarketDataStore)
│   └── strategies/      # Strategy implementations
├── src/
│   ├── common/
│   ├── fin/
│   ├── fix/
│   ├── strategies/
│   └── trader_main.cpp  # Entry point
├── config/              # Configuration files
├── cmake/               # CMake modules
└── CMakeLists.txt
```

## Dependencies

- **libxchange**: FIX protocol library (local vcpkg port)
- **Boost**: Property tree (INI parsing)
- **OpenSSL**: ED25519 cryptography
- **nlohmann/json**: REST API parsing

## References

- [Binance FIX API Documentation](https://developers.binance.com/docs/binance-spot-api-docs/fix-api)
- [Binance Spot REST API](https://developers.binance.com/docs/binance-spot-api-docs/rest-api)
- [Binance Filters](https://developers.binance.com/docs/binance-spot-api-docs/filters)



/**
 * ArbitragePath - Represents a single triangular arbitrage path.
 *
 * ============================================================================
 * TRADING RULES (Binance Spot)
 * ============================================================================
 *
 * Symbol Convention:
 *   Symbol BASE/QUOTE (e.g., BTCJPY means BTC=base, JPY=quote)
 *   - To BUY 1 BASE, you pay ASK price in QUOTE
 *   - To SELL 1 BASE, you receive BID price in QUOTE
 *
 * Price Rules:
 *   - BUY  uses ASK price (you pay the higher price)
 *   - SELL uses BID price (you receive the lower price)
 *
 * Fee Rules:
 *   - BUY:  fee deducted from BASE  (what you receive)
 *   - SELL: fee deducted from QUOTE (what you receive)
 *
 * Trade Formulas:
 *   BUY BASE/QUOTE:
 *     input:  stake in QUOTE
 *     output: (stake / ask) * (1 - fee) in BASE
 *
 *   SELL BASE/QUOTE:
 *     input:  stake in BASE
 *     output: (stake * bid) * (1 - fee) in QUOTE
 *
 * ============================================================================
 * EXAMPLE: 1 BTC -> LPT -> JPY -> BTC
 * ============================================================================
 *
 * Leg 1: BUY LPTBTC (LPT/BTC) at ask=0.00003620
 *   input:  1 BTC (quote)
 *   raw:    1 / 0.00003620 = 27624.31 LPT
 *   output: 27624.31 * 0.999 = 27596.68 LPT (after 0.1% fee on base)
 *
 * Leg 2: SELL LPTJPY (LPT/JPY) at bid=513.90
 *   input:  27596.68 LPT (base)
 *   raw:    27596.68 * 513.90 = 14181,423.49 JPY
 *   output: 14181423.49 * 0.999 = 14167242.07 JPY (after 0.1% fee on quote)
 *
 * Leg 3: BUY BTCJPY (BTC/JPY) at ask=13807098
 *   input:  14167242.07 JPY (quote)
 *   raw:    14167242.07 / 13807098 = 1.02608 BTC
 *   output: 1.02608 * 0.999 = 1.02506 BTC (after 0.1% fee on base)
 *
 * PnL: 1.02506 - 1.0 = +0.02506 BTC (+2.506%)
 *
 * ============================================================================
 * PRICE MULTIPLIERS (why 1/ASK for BUY?)
 * ============================================================================
 *
 * For symbol BASE/QUOTE (e.g., BTCJPY: BTC=base, JPY=quote):
 *
 *   ASK = price to buy 1 BASE in QUOTE units
 *   BID = price to sell 1 BASE in QUOTE units
 *
 * BUY scenario:
 *   - To buy 1 BASE, you spend ASK QUOTE    →  1 BASE costs ASK QUOTE
 *   - If you have 1 QUOTE, you get 1/ASK BASE
 *   - Multiplier: 1/ASK (converts QUOTE → BASE)
 *
 * SELL scenario:
 *   - To sell 1 BASE, you receive BID QUOTE  →  1 BASE yields BID QUOTE
 *   - If you have 1 BASE, you get BID QUOTE
 *   - Multiplier: BID (converts BASE → QUOTE)
 *
 * ============================================================================
 * FAST RATIO (O(1) approximation)
 * ============================================================================
 *
 * For quick screening, we compute ratio by applying fee at EACH leg:
 *
 *   ratio = 1.0
 *   for each leg:
 *       ratio *= priceMultiplier[leg] * (1 - fee[leg])
 *
 * Where priceMultiplier is:
 *   - BUY:  1/ask  (converts quote→base)
 *   - SELL: bid    (converts base→quote)
 *
 * If ratio > 1.0, the path is potentially profitable.
 * Note: Fast ratio ignores order size constraints and rounding.
 *
 * ============================================================================
 */