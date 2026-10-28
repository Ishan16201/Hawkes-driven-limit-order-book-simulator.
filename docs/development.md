# Development guide

Use CMake presets from the repository root:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/lob_simulator
./build/dev/lob_benchmarks
```

`release` selects optimized compilation. `sanitize` enables AddressSanitizer and
UndefinedBehaviorSanitizer on Clang or GCC. Sanitizers should be used for local
debugging rather than performance measurements.

Tests use Catch2 v3.8.0 (`914aeecfe23b1e16af6ea675a4fb5dbd5a5b8d0a`), acquired
with CMake `FetchContent` at configure time. The core library and executables
make no runtime network requests. Format with
`clang-format` and analyze with `clang-tidy` when installed; this environment may
not provide either tool. Do not reformat generated build output.

The benchmark target is intentionally a lightweight, dependency-free timing
harness. It is for regression observation, not for published performance claims.
