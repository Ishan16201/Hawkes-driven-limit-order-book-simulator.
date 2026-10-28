#include "lob/stochastic/hawkes_parameters.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("stationary Hawkes parameters use integrated branching coefficients") {
  const lob::stochastic::HawkesParameters stable{
      .baseline_events_per_second = {1.0, 2.0},
      .alpha_branching = {{0.20, 0.10}, {0.10, 0.20}},
      .beta_per_second = {{0.5, 10.0}, {10.0, 0.5}},
  };

  REQUIRE(lob::stochastic::branching_upper_bound(stable) == Catch::Approx{0.30});
  REQUIRE_NOTHROW(lob::stochastic::validate_stationary(stable));
}

TEST_CASE("non-subcritical conservative branching bound is rejected") {
  const lob::stochastic::HawkesParameters unstable{
      .baseline_events_per_second = {1.0},
      .alpha_branching = {{1.0}},
      .beta_per_second = {{1.0}},
  };
  REQUIRE_THROWS_AS(lob::stochastic::validate_stationary(unstable), std::invalid_argument);
}

TEST_CASE("spectral radius handles periodic nonnegative branching matrices") {
  const lob::stochastic::HawkesParameters stable{
      .baseline_events_per_second = {1.0, 1.0},
      .alpha_branching = {{0.0, 2.0}, {0.18, 0.0}},
      .beta_per_second = {{1.0, 1.0}, {1.0, 1.0}},
  };
  REQUIRE(lob::stochastic::branching_spectral_radius(stable) ==
          Catch::Approx(0.6).epsilon(1.0e-12));
  REQUIRE_NOTHROW(lob::stochastic::validate_stationary(stable));

  const lob::stochastic::HawkesParameters unstable{
      .baseline_events_per_second = {1.0, 1.0},
      .alpha_branching = {{0.0, 3.0}, {0.5, 0.0}},
      .beta_per_second = {{1.0, 1.0}, {1.0, 1.0}},
  };
  REQUIRE(lob::stochastic::branching_spectral_radius(unstable) ==
          Catch::Approx(std::sqrt(1.5)).epsilon(1.0e-12));
  REQUIRE_THROWS_AS(lob::stochastic::validate_stationary(unstable),
                    std::invalid_argument);
}
