# Backend API Integration Test Report

## Purpose

This report describes automated tests for the Flask API using the repository's
real C++ `file-compress` executable. Unlike the helper-focused tests in
`backend/test_app.py`, this suite configures and builds the native executable
with CMake and sends requests through Flask's test client.

## Test environment

- Python dependencies: `backend/requirements.txt`
- Native build: CMake, a C++17 compiler, and zlib development headers
- Executable: built during the pytest session in a temporary build directory
- API: Flask test client; no external server or shared files are required

## Automated scenarios

| Area | Coverage |
|---|---|
| Compress | Successful text, binary, and empty-file uploads |
| Archive naming | Custom download filename |
| List | List entries from a ZIP created by the real executable |
| Extract | Extract archive and verify byte-for-byte content |
| Directory structure | Preserve nested uploaded paths across extraction |
| Upload validation | Duplicate names, per-file size limit, and file-count limit |
| Invalid archives | Reject non-ZIP input through list and extract routes |
| Temporary storage | Verify request workspaces are removed after success and invalid input |

## Run locally

From the repository root:

```bash
python -m pip install -r backend/requirements.txt
python -m pytest -q backend
```

The integration fixture builds the real executable itself. A separate manual
build or placeholder executable is not used for these integration tests.

## Results

The test cases are implemented and wired into the pull-request CI workflow.
The final pass/fail result must be taken from the GitHub Actions run for this
pull request. No pass percentage is stated in this report before that run
completes.
