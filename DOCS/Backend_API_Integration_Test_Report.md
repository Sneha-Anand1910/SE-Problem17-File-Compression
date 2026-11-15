# Backend API Integration Test Report

## Purpose

This report records automated tests for the Flask API using the repository's real C++ `file-compress` executable. The suite builds the executable with CMake and exercises it through Flask's test client; it does not use the placeholder executable from `backend/test_app.py`.

## Test environment

- Python dependencies: `backend/requirements.txt`
- Native build: CMake, a C++17 compiler, and zlib development headers
- Executable: built during the pytest session in a temporary build directory
- API: Flask test client; no external server or shared files are required
- CI: GitHub Actions workflow on PR #18

## Automated scenarios

| Area | Coverage |
|---|---|
| Compress | Successful text, binary, and empty-file uploads |
| Archive naming | Custom download filename |
| List | List entries from a ZIP created by the real executable |
| Extract | Extract archive and verify byte-for-byte content |
| Directory structure | Preserve nested uploaded paths across extraction |
| Upload validation | Duplicate names, configured per-file size and file-count limits |
| Boundary validation | Exact configured per-file size limit; exact multipart request-size cap and one-byte-over rejection |
| Invalid archives | Reject non-ZIP and zero-byte archive input through List and Extract |
| Temporary storage | Cleanup after successful Compress, List, and Extract; invalid List input; synthetic executable failure; synthetic timeout |

## Results

The latest pull-request CI workflow completed successfully on commit `dcb69234b3a3140162207e2ce338635a76994883`.

- Backend pytest suite: **28 passed, 0 failed**.
- C++ CTest smoke suite: **1 of 1 registered tests passed**.
- The integration tests build and invoke the real C++ executable.
- The suite contains the original backend tests plus the real-executable API integration cases.

The exact-limit tests use controlled small values (3-byte per-file cap and a small multipart request cap) to validate boundary behavior. They do not stress the full default 50 MiB per-file or 100 MiB request limits.

These results apply only to the automated cases listed above. They do not establish that every test planned in the Software Test Plan has been executed, nor do they replace manual frontend/system validation.

## Run locally

From the repository root:

```bash
python -m pip install -r backend/requirements.txt
python -m pytest -q backend
```

The integration fixture builds the real executable itself. A separate manual build or placeholder executable is not used for these integration tests.
