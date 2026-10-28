# Hawkes-Driven Limit Order Book Simulator

- Treat `docs/design.md` as the accepted system specification. Keep exchange
  mechanics independent from stochastic generation, historical replay,
  estimation, analytics, and strategy evaluation.
- Prefer correctness and explicit invariants before optimization. Add unit tests
  for matching behavior and stochastic-model invariants with every relevant
  change.
- Do not fabricate empirical results, data availability, calibration, or
  performance results. Do not claim empirical calibration unless it has actually
  been run against suitable data.
- Do not silently alter mathematical definitions. Document all model changes,
  assumptions, parameter units, time units, price ticks, and quantity lots.
- Store prices as integer ticks, quantities as integer lots/shares, and time as
  explicitly documented integer units. Keep random behavior deterministic and
  seedable.
- Strategies may consume only current or historical information available at the
  decision time; never evaluate a strategy using future information.
- Keep dependencies minimal, pinned, and reproducible. The core runtime must not
  require Python or network access.
- Use C++20, explicit `lob` namespaces, no global mutable state, and small,
  typed interfaces. Avoid premature templates and abstractions.
- Verify all relevant changes: configure, build, run tests, and run affected
  executables/benchmarks. Record environmental limitations honestly.
