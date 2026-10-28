cmake_minimum_required(VERSION 3.25)

project(hawkes_lob_simulator VERSION 0.1.0 LANGUAGES CXX)

option(LOB_BUILD_TESTS "Build the Catch2 test executable." ON)
option(LOB_BUILD_BENCHMARKS "Build lightweight local benchmarks." ON)
option(LOB_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers." OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(lob_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

function(lob_enable_sanitizers target)
  if(LOB_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  elseif(LOB_ENABLE_SANITIZERS)
    message(WARNING "Sanitizers are not configured for ${CMAKE_CXX_COMPILER_ID}.")
  endif()
endfunction()

add_library(lob_core STATIC
  src/core/types.cpp
  src/exchange/matching_engine.cpp
  src/exchange/order_book.cpp
  src/simulation/simulator.cpp
  src/stochastic/hawkes_parameters.cpp
  src/stochastic/hawkes_process.cpp
  src/stochastic/hawkes_market_adapter.cpp
  src/stochastic/event_types.cpp
  src/analytics/types.cpp
  src/analytics/microstructure_analytics.cpp
  src/historical/event_parser.cpp
  src/estimation/hawkes_estimator.cpp
  src/strategy/order_broker.cpp
  src/strategy/strategy_evaluator.cpp
)
add_library(lob::core ALIAS lob_core)
target_include_directories(lob_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(lob_core PUBLIC cxx_std_20)
lob_enable_warnings(lob_core)
lob_enable_sanitizers(lob_core)
if(LOB_BUILD_TESTS)
  target_compile_definitions(lob_core PUBLIC LOB_ENABLE_TESTING=1)
endif()

add_executable(lob_simulator src/main.cpp)
target_link_libraries(lob_simulator PRIVATE lob::core)
lob_enable_warnings(lob_simulator)
lob_enable_sanitizers(lob_simulator)

if(LOB_BUILD_BENCHMARKS)
  add_executable(lob_benchmarks benchmarks/benchmark_main.cpp)
  target_link_libraries(lob_benchmarks PRIVATE lob::core)
  lob_enable_warnings(lob_benchmarks)
  lob_enable_sanitizers(lob_benchmarks)
endif()

if(LOB_BUILD_TESTS)
  include(CTest)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    # Catch2 v3.8.0; immutable upstream commit for reproducible test builds.
    GIT_TAG 914aeecfe23b1e16af6ea675a4fb5dbd5a5b8d0a
  )
  FetchContent_MakeAvailable(Catch2)

  add_executable(lob_tests
    tests/test_main.cpp
    tests/types_test.cpp
    tests/matching_engine_test.cpp
    tests/exchange_property_test.cpp
    tests/simulator_test.cpp
    tests/order_book_test.cpp
    tests/hawkes_parameters_test.cpp
    tests/hawkes_process_test.cpp
    tests/hawkes_market_adapter_test.cpp
    tests/microstructure_analytics_test.cpp
    tests/historical_ingestion_test.cpp
    tests/hawkes_estimation_test.cpp
    tests/strategy_test.cpp
  )
  target_link_libraries(lob_tests PRIVATE lob::core Catch2::Catch2WithMain)
  target_compile_definitions(
    lob_tests PRIVATE LOB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  lob_enable_warnings(lob_tests)
  lob_enable_sanitizers(lob_tests)
  include(Catch)
  catch_discover_tests(lob_tests)
endif()
