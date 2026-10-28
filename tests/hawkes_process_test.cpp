#include "lob/stochastic/hawkes_process.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numeric>

namespace { lob::stochastic::HawkesParameters one(double mu,double alpha,double beta){return {{mu},{{alpha}},{{beta}}};} }
TEST_CASE("zero excitation is a seeded Poisson reference") {
  lob::stochastic::ExponentialHawkesProcess a(one(2.0,0.0,1.0),7),b(one(2.0,0.0,5.0),7);
  const auto ea=a.simulate_until(10.0), eb=b.simulate_until(10.0); REQUIRE(ea.size()==eb.size()); REQUIRE(ea[0].time_seconds==eb[0].time_seconds);
}
TEST_CASE("recursive state decays exactly and event jump is alpha beta") {
  lob::stochastic::ExponentialHawkesProcess p(one(1.0,0.5,2.0),1); p.observe_event(0,1.0); REQUIRE(p.intensities()[0]==Catch::Approx(2.0)); p.advance_to(2.0); REQUIRE(p.intensities()[0]==Catch::Approx(1.0+std::exp(-2.0))); p.observe_event(0,2.0); REQUIRE(p.intensities()[0]==Catch::Approx(2.0+std::exp(-2.0)));
}
TEST_CASE("validation stability and stationary mean use integrated alpha") {
  const auto p=one(2.0,0.25,20.0); REQUIRE(lob::stochastic::branching_spectral_radius(p)==Catch::Approx(0.25)); REQUIRE(lob::stochastic::stationary_mean_intensity(p)[0]==Catch::Approx(2.0/0.75));
  REQUIRE_THROWS_AS(lob::stochastic::ExponentialHawkesProcess(one(1.0,1.0,1.0),1),std::invalid_argument);
}
TEST_CASE("fixed seed reproduces, different seed changes a path, and intensities remain nonnegative") {
  const auto p=one(1.5,0.4,3.0); lob::stochastic::ExponentialHawkesProcess a(p,42),b(p,42),c(p,43);const auto ea=a.simulate_until(20.0),eb=b.simulate_until(20.0),ec=c.simulate_until(20.0);REQUIRE(ea.size()==eb.size());REQUIRE(ea[0].time_seconds==eb[0].time_seconds);REQUIRE(ea[0].time_seconds!=ec[0].time_seconds);for(const auto& e:ea)REQUIRE(e.pre_event_intensities[0]>=0.0);
}
TEST_CASE("stable long-run mean and clustering exceed Poisson dispersion") {
  const auto p=one(1.0,0.3,4.0); lob::stochastic::ExponentialHawkesProcess h(p,9); const auto e=h.simulate_until(2000.0); const double mean=static_cast<double>(e.size())/2000.0; REQUIRE(mean==Catch::Approx(1.0/0.7).epsilon(0.20));
  lob::stochastic::ExponentialHawkesProcess q(one(1.0,0.0,1.0),9); const auto poisson=q.simulate_until(2000.0); REQUIRE(e.size()>poisson.size());
}

TEST_CASE("multivariate recursive excitation matches direct historical summation") {
  const lob::stochastic::HawkesParameters p{
      .baseline_events_per_second = {0.5, 0.7},
      .alpha_branching = {{0.2, 0.1}, {0.15, 0.25}},
      .beta_per_second = {{2.0, 3.0}, {4.0, 1.5}},
  };
  lob::stochastic::ExponentialHawkesProcess process(p, 21U);
  process.observe_event(0U, 1.0);
  process.observe_event(1U, 1.5);
  process.advance_to(2.0);

  const auto actual = process.intensities();
  for (std::size_t child = 0; child < 2U; ++child) {
    double direct = p.baseline_events_per_second[child];
    direct += p.alpha_branching[child][0] * p.beta_per_second[child][0] *
              std::exp(-p.beta_per_second[child][0] * (2.0 - 1.0));
    direct += p.alpha_branching[child][1] * p.beta_per_second[child][1] *
              std::exp(-p.beta_per_second[child][1] * (2.0 - 1.5));
    REQUIRE(actual[child] == Catch::Approx(direct).epsilon(1.0e-12));
  }
}

TEST_CASE("independent seeded Hawkes runs report stationary-rate uncertainty") {
  const auto parameters = one(1.0, 0.3, 4.0);
  constexpr std::size_t run_count = 24U;
  constexpr double horizon = 1000.0;
  std::vector<double> rates;
  rates.reserve(run_count);

  for (std::uint64_t seed = 1U; seed <= run_count; ++seed) {
    lob::stochastic::ExponentialHawkesProcess process(parameters, seed);
    const auto events = process.simulate_until(horizon);
    rates.push_back(static_cast<double>(events.size()) / horizon);
  }

  const double mean =
      std::accumulate(rates.begin(), rates.end(), 0.0) /
      static_cast<double>(rates.size());
  double squared_deviation = 0.0;
  for (const double rate : rates) {
    const double deviation = rate - mean;
    squared_deviation += deviation * deviation;
  }
  const double standard_error =
      std::sqrt(squared_deviation / static_cast<double>(run_count - 1U)) /
      std::sqrt(static_cast<double>(run_count));
  const double theoretical_mean = 1.0 / (1.0 - 0.3);

  REQUIRE(std::abs(mean - theoretical_mean) <= 3.0 * standard_error);
}
