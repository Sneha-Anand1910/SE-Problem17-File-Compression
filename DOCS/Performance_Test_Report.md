# Performance Test Report

## Objective

Validate compression and extraction performance for the 1 MiB, 10 MiB, and 100 MiB test datasets required by the SRS performance target.

## Method

The benchmark generates deterministic binary data locally so no dataset needs to be committed to the repository. For each dataset it:

1. creates the input file;
2. times ZIP compression;
3. records the generated archive size;
4. times extraction;
5. compares the extracted file byte-for-byte with the original;
6. reports the archive/input size ratio.

The benchmark is implemented in `tests/performance_test.cpp` and is executed by GitHub Actions through `.github/workflows/performance.yml`.

## Acceptance

The 100 MiB case must complete successfully without timeout or failure, and the integrity check must pass. Timing is recorded from the CI runner and should be interpreted as an environment-specific measurement rather than a universal machine-time guarantee.

## Results

Measured on GitHub Actions ubuntu-24.04 runner. The benchmark completed successfully for all three sizes and verified each extracted file byte-for-byte against its source.

| Dataset | Compression (ms) | Extraction (ms) | Archive size | Integrity |
|---|---:|---:|---:|---|
| 1 MiB | 25 | 0 | 1,048,708 bytes | PASS |
| 10 MiB | 256 | 10 | 10,485,894 bytes | PASS |
| 100 MiB | 2492 | 122 | 104,857,736 bytes | PASS |

## Evidence

Performance workflow:
- `.github/workflows/performance.yml`

Benchmark source:
- `tests/performance_test.cpp`

### CI Evidence
The successful benchmark run is GitHub Actions run #8 for the Performance benchmark workflow.
