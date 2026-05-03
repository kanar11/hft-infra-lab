# HFT Infra Lab — Refactor Session Summary

`46ea0dd` → `07a6da8` &mdash; **29 commits** across 7 planned phases plus 5 follow-up rounds.

## What got done

- **Phase 1** — Risk Manager pending exposure
- **Phase 2** — Realistic simulator fill latency
- **Phase 3** — Trade Logger hot-path review
- **Phase 4** — SPSC queue audit + stress test
- **Phase 5** — Tighten cppcheck suppressions
- **Phase 6** — Strategy edge cases
- **Phase 7** — Parser robustness
- **Round R1** — O(1) perf hotspots
- **Round R2** — common/ shared utilities
- **Round R3** — CI: matrix + sanitizers
- **Round R4** — Test coverage gaps
- **Round R5** — Silent-failure warnings

## Commits by category

### Features (3)

- [`33ab400`](https://github.com/kanar11/hft-infra-lab/commit/33ab400) feat(oms): track pending exposure for proper HFT position limits
- [`c21088f`](https://github.com/kanar11/hft-infra-lab/commit/c21088f) feat(risk): track pending exposure across submit/fill/cancel
- [`cbad0ce`](https://github.com/kanar11/hft-infra-lab/commit/cbad0ce) feat(simulator): realistic fill latency with in-flight queue

### Performance (1)

- [`043e509`](https://github.com/kanar11/hft-infra-lab/commit/043e509) perf: O(1) hotspots in RiskManager + Strategy SMA

### Refactor (5)

- [`05c8c3a`](https://github.com/kanar11/hft-infra-lab/commit/05c8c3a) refactor(build): use build/ dir with auto header-dep tracking
- [`c58938a`](https://github.com/kanar11/hft-infra-lab/commit/c58938a) refactor(logger): overflow-safe counters + batched flush + DRY exports
- [`f90563c`](https://github.com/kanar11/hft-infra-lab/commit/f90563c) refactor(lockfree): extract SPSCQueue to header + add stress test
- [`7edeec8`](https://github.com/kanar11/hft-infra-lab/commit/7edeec8) refactor: replace cppcheck arrayIndexOutOfBounds suppresses with strnlen
- [`681359f`](https://github.com/kanar11/hft-infra-lab/commit/681359f) refactor(arch): consolidate Side enum, sym_to_key, time helpers in common/

### Fixes (11)

- [`46ea0dd`](https://github.com/kanar11/hft-infra-lab/commit/46ea0dd) fix: cppcheck narrowingConversion вЂ” size_t to int in latency benchmarks
- [`64fcd07`](https://github.com/kanar11/hft-infra-lab/commit/64fcd07) fix(benchmarks): cast vector indices to size_t
- [`4675832`](https://github.com/kanar11/hft-infra-lab/commit/4675832) fix(cppcheck): resolve warning/performance/error issues from extended scan
- [`359cea1`](https://github.com/kanar11/hft-infra-lab/commit/359cea1) fix(cppcheck): suppress arrayIndexOutOfBoundsCond in multicast symbol copy
- [`b25c21c`](https://github.com/kanar11/hft-infra-lab/commit/b25c21c) fix(cppcheck): suppress uninitvar on SPSCQueue in concurrent stress test
- [`4a63a6e`](https://github.com/kanar11/hft-infra-lab/commit/4a63a6e) fix: use std::memchr instead of strnlen (POSIX-only) for symbol length
- [`95ddf2e`](https://github.com/kanar11/hft-infra-lab/commit/95ddf2e) fix(strategy): guard window<=0, NaN/Inf prices, sma=0 division
- [`d8400d2`](https://github.com/kanar11/hft-infra-lab/commit/d8400d2) fix: three latent bugs from post-session review
- [`6b886bb`](https://github.com/kanar11/hft-infra-lab/commit/6b886bb) fix(risk): replace stale now_ns() reference with mono_ns()
- [`2498695`](https://github.com/kanar11/hft-infra-lab/commit/2498695) fix: real bug surfaced by ASAN + clang -Wunused-private-field
- [`07a6da8`](https://github.com/kanar11/hft-infra-lab/commit/07a6da8) fix(test): use 'rejected' counter so clang -Wunused-but-set-variable passes

### Hardening (1)

- [`c0e4e2e`](https://github.com/kanar11/hft-infra-lab/commit/c0e4e2e) hardening(parsers): bound FIX strlen + add 7 negative tests

### Tests (3)

- [`f6eb309`](https://github.com/kanar11/hft-infra-lab/commit/f6eb309) test: unify exit code on tests_failed across demos
- [`3326eff`](https://github.com/kanar11/hft-infra-lab/commit/3326eff) test(integration): fill order before testing position limit rejection
- [`c2a9844`](https://github.com/kanar11/hft-infra-lab/commit/c2a9844) test: cover async logger flush, rate limiter, drawdown peak=0, protocol interleave

### CI / tooling (3)

- [`a5accd9`](https://github.com/kanar11/hft-infra-lab/commit/a5accd9) ci(tooling): static-analysis + compiler matrix + sanitizers + .clang-format
- [`2a5a725`](https://github.com/kanar11/hft-infra-lab/commit/2a5a725) ci: temporarily revert clang++ matrix and sanitizers job
- [`3980462`](https://github.com/kanar11/hft-infra-lab/commit/3980462) ci: mark clang++ build as continue-on-error pending warning fixes

### Chores (1)

- [`a2c1f21`](https://github.com/kanar11/hft-infra-lab/commit/a2c1f21) chore: warn on OMS double-cancel and Strategy MAX_STOCKS overflow

### Other (1)

- [`7a37feb`](https://github.com/kanar11/hft-infra-lab/commit/7a37feb) docs: scripts/session_summary.py + generated SESSION_SUMMARY.md

## Known follow-ups

- Bilingual (PL+ENG) comment style is inconsistent across modules — ad-hoc cleanup, not blocking.
- `cppcheck-suppress uninitvar` on `SPSCQueue<T,SIZE>{}` in `tests/test_all.cpp` is a known cppcheck template-modeling limitation, intentionally kept.
- CI matrix could be extended with macOS / `-fsanitize=memory` / `clang-tidy` once we want stricter coverage.
