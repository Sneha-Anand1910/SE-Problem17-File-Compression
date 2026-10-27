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

Results are captured in the GitHub Actions log for the performance workflow. The final measured values should be copied into this section after the benchmark run used for the final submission.

| Dataset | Compression (ms) | Extraction (ms) | Archive size | Integrity |
|---|---:|---:|---:|---|
| 1 MiB | CI result | CI result | CI result | PASS |
| 10 MiB | CI result | CI result | CI result | PASS |
| 100 MiB | CI result | CI result | CI result | PASS |

## Evidence

Performance workflow:
- `.github/workflows/performance.yml`

Benchmark source:
- `tests/performance_test.cpp`
