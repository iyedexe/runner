cmake_minimum_required(VERSION 3.20)
project(runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find vcpkg packages
find_package(Boost REQUIRED COMPONENTS system property_tree)
find_package(OpenSSL REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(quill CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(libxchange CONFIG REQUIRED)

# Common sources
set(COMMON_SOURCES
    src/common/Scheduler.cpp
)

# Trader sources
set(TRADER_SOURCES
    ${COMMON_SOURCES}
    src/fin/SymbolFilters.cpp
    src/strategies/circular_arbitrage/ArbitragePath.cpp
    src/strategies/circular_arbitrage/TriangularArbitrage.cpp
    src/market_connection/Admin.cpp
    src/market_connection/Feeder.cpp
    src/market_connection/Broker.cpp
    src/persistence/TradePersistence.cpp
    src/Runner.cpp
    src/trader_main.cpp
)

# Common libraries
set(COMMON_LIBS
    libxchange::libxchange
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    unofficial-sodium::sodium
    quill::quill
    nlohmann_json::nlohmann_json
    fmt::fmt
    prometheus-cpp::core
    prometheus-cpp::pull
)

# Trader executable
add_executable(trader ${TRADER_SOURCES})
target_include_directories(trader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/common
)
target_link_libraries(trader PRIVATE ${COMMON_LIBS})

# Enable Link-Time Optimization for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Enable architecture-specific optimizations
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(trader PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native -mtune=native -funroll-loops>
        $<$<NOT:$<CONFIG:Release>>:-march=native -funroll-loops>
    )
endif()
