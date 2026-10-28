#include "lob/core/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("core units validate deterministic exchange inputs") {
  REQUIRE(lob::core::opposite(lob::core::Side::Buy) == lob::core::Side::Sell);
  REQUIRE(lob::core::is_valid_timestamp(0));
  REQUIRE(lob::core::is_valid_quantity(1U));
  REQUIRE_THROWS_AS(lob::core::require_valid_timestamp(-1), std::invalid_argument);
  REQUIRE_THROWS_AS(lob::core::require_valid_quantity(0U), std::invalid_argument);
}
