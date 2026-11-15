# Backend API Integration Test Report

## Purpose

This report records automated tests for the Flask API using the repository's
real C++ `file-compress` executable. The suite builds the executable with
CMake and exercises it through Flask's test client; it does not use the
placeholder executable from `backend/test_app.py`.

## Test environment

- Python dependencies: `backend/requirements.txt`
- Native build: CMake, a C++17 compiler, and zlib development headers
- Executable: built during the pytest session in a temporary build directory
- API: Flask test client; no external server or shared files are required
- CI: GitHub Actions workflow on this pull request

## Automated scenarios

| Area | Coverage |
|---|---|
| Compress | Successful text, binary, and empty-file uploads |
| Archive naming | Custom download filename |
| List | List entries from a ZIP created by the real executable |
| Extract | Extract archive and verify byte-for-byte content |
| Directory structure | Preserve nested uploaded paths across extraction |
| Upload validation | Duplicate names, configured per-file size limit, and configured file-count limit |
| Invalid archives | Reject non-ZIP input through list and extract routes |
| Temporary storage | Verify request workspaces are removed after successful compression and invalid list input |

## Results

The pull-request CI workflow completed successfully.

- Backend pytest suite: **21 passed, 0 failed**.
- C++ CTest smoke suite: **1 of 1 registered tests passed**.
- The integration tests build and invoke the real C++ executable.
- The backend suite includes the pre-existing eight tests plus the real-executable integration tests.

These results apply to the automated cases listed above. They do not establish
that every test planned in the Software Test Plan has been executed, nor do
they replace manual frontend/system validation.

## Run locally

From the repository root:

```bash
python -m pip install -r backend/requirements.txt
python -m pytest -q backend
```

The integration fixture builds the real executable itself. A separate manual
build or placeholder executable is not used for these integration tests.
