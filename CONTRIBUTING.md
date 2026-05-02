# Contributing

## Before Opening A PR

Run the local precheck when the needed MariaDB/Stoolap dependencies are
available:

```sh
scripts/precheck.sh
```

At minimum, build the plugin and run the targeted test case for the behavior
you changed. Before pushing a release-facing change, run the full suite.

## Regression Discipline

- Every bug fix lands with a regression test unless the failure cannot be
  reproduced in the local harness.
- Every non-obvious architecture change updates the matching file under
  `docs/` or the memory pointer that now refers to it.
- `Stoolap_unmapped_errors` is a drift counter, not an activity counter. Do
  not allowlist it in tests.
- `clang-format-18 --dry-run --Werror src/*.{cc,h}` must pass. CI pins
  clang-format-18 because different major versions disagree on edge
  cases. Install via apt (`apt-get install clang-format-18`), Homebrew
  (`brew install llvm@18` then use `$(brew --prefix llvm@18)/bin/clang-format`),
  or `docker run --rm -v $PWD:/work -w /work ubuntu:24.04 sh -c 'apt update && apt install -y clang-format-18 && clang-format-18 --dry-run --Werror src/*.{cc,h}'`.

## Benchmark Baselines

Intentional performance changes should update `benchmarks/baseline.json` on the
canonical machine and commit the new baseline with the same PR:

```sh
python3 benchmarks/bench.py \
  --runs 3 \
  --write-baseline benchmarks/baseline.json \
  --update-baseline-i-promise
```

Do not regenerate the committed baseline from an arbitrary noisy local run.

## Upstream Stoolap Coordination

The plugin carries workarounds that disappear if Stoolap exposes a few small
FFI surfaces. Draft of the upstream issue lives at
[`docs/upstream/stoolap_ffi_issue.md`](docs/upstream/stoolap_ffi_issue.md).
Update it before opening or amending the upstream issue.
