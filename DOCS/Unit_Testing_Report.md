# Unit Testing Report — File Compression Tool

## 1. Objective and scope

This report records the automated checks found in `tests/smoke_test.cpp` and `backend/test_app.py`, and separates function-level checks from component, API, and smoke checks. The report IDs below identify checks for this report only; they are not test case IDs from the Software Test Plan (STP) or Software Requirements Specification (SRS). Execution status is stated only where supported by the supplied CTest result or repository evidence.

## 2. Testing environment and tools

The C++ project uses CMake to build `compression_core` and the `smoke_test` executable. `CMakeLists.txt` registers **one** CTest test, named `smoke_test`; it also builds `performance_test` but does not register it with CTest. The supplied coverage-enabled C++ run used GCC 16.2.0, CMake 4.4.4, a Debug build with `--coverage -O0 -g`, and CTest. CTest reported **100% of registered tests passing**. The Python tests use pytest, Flask's test client, temporary paths, and Werkzeug `FileStorage`. The earlier focused backend run used Windows, Python 3.13.0, and pytest 8.4.2. The later PR #18 CI run also executed the expanded backend suite on GitHub Actions.

## 3. Test cases found in the code

**Status key:** *Passed via CTest* means the check is on the unconditional path of the single passing `smoke_test` executable. *Conditional* means the code may skip the check depending on platform support. *Passed via pytest* means the named Python test passed in the verified pytest run. The C++ checks below are mostly component or smoke tests because they call the ZIP API and use real filesystem artifacts, rather than isolating one function.

### C++ component and smoke checks

| Report ID | Check in `tests/smoke_test.cpp` | Expected result | Level | Execution status |
| --- | --- | --- | --- | --- |
| UT-CPP-01 | Create an archive from a missing input path or an empty input list | Both calls reject the input | Component, negative | Passed via CTest |
| UT-CPP-02 | List a missing ZIP, plain text disguised as ZIP, a truncated ZIP, or a directory used as an archive | Each invalid archive is rejected | Component, negative | Passed via CTest |
| UT-CPP-03 | Extract plain text disguised as a ZIP | Extraction is rejected | Component, negative | Passed via CTest |
| UT-CPP-04 | Alter central-directory CRC metadata and extract | Corrupted metadata is rejected | Component, integrity | Passed via CTest |
| UT-CPP-05 | Corrupt file data in a generated ZIP and extract | Extraction raises an error containing `CRC check failed` | Component, integrity | Passed via CTest |
| UT-CPP-06 | Compress a directory, list it, and extract it | Relative file and directory paths are present; text, binary, empty-file, and nested-directory results are restored | Smoke and integration | Passed via CTest |
| UT-CPP-07 | Attempt archive creation and extraction over existing data with overwrite disabled | Both are rejected and existing bytes remain unchanged | Component, filesystem | Passed via CTest |
| UT-CPP-08 | Extract archive entries with `../`, Windows-style `..\`, absolute POSIX, or drive-qualified paths | Unsafe paths are rejected and traversal targets are not created | Component, security | Passed via CTest |
| UT-CPP-09 | Extract over an existing file with overwrite disabled, then enabled | Existing bytes remain unchanged when disabled; explicit overwrite replaces them | Component, filesystem | Passed via CTest |
| UT-CPP-10 | Extract through directory and file symlinks in the destination | Symlink paths are rejected and outside data remains unchanged | Component, security | Conditional; symlink creation may be unavailable, and no per-check execution evidence was supplied |

### Python function and API checks

| Report ID | pytest function in `backend/test_app.py` | Expected result | Level | Execution status |
| --- | --- | --- | --- | --- |
| UT-PY-01 | `test_archive_name_defaults_to_archive_zip` | Missing or empty name becomes `archive.zip` | Function unit | Passed via pytest |
| UT-PY-02 | `test_empty_upload_is_allowed` | Saving a zero-byte upload creates an empty file | Function unit with filesystem | Passed via pytest |
| UT-PY-03 | `test_only_archive_validation_errors_are_invalid_zip` | Corrupt-archive command errors become `ValueError("Invalid ZIP file")`; other processing errors remain runtime errors | Function unit | Passed via pytest |
| UT-PY-04 | `test_compress_requires_files` | `/api/compress` returns HTTP 400 and the required-files JSON error | API component | Passed via pytest |
| UT-PY-05 | `test_oversized_request_returns_json_error` | Over-limit multipart request returns HTTP 413 and a JSON error | API component | Passed via pytest |
| UT-PY-06 | `test_rejects_traversal_filename` | Unsafe uploaded filename returns HTTP 400 with an invalid-filename error | API component | Passed via pytest |
| UT-PY-07 | `test_rejects_invalid_archive_name` | Traversal archive name returns HTTP 400 and `Invalid ZIP filename` | API component | Passed via pytest |
| UT-PY-08 | `test_missing_zip_is_json_error` | `/api/list` without an archive returns HTTP 400 and the required-ZIP JSON error | API component | Passed via pytest |

## 4. C++ automated testing results

The supplied coverage-enabled CTest run reported **100% of registered tests passing**. Since CMake registers only `smoke_test`, this establishes a pass for that executable as a whole, not a separate CTest result for every row above. The executable returns a failure code when its unconditional assertions fail. Its symlink section runs only if the host permits creating a directory symlink; the overall CTest pass does not prove that section ran. `performance_test` is built but is not part of this CTest result.

## 5. Python backend testing coverage

The command `python -m pytest backend/test_app.py -v` collected **8 tests** and completed with **8 passed, 0 failed, in 1.07 seconds**. All eight named tests in the Python table passed. PR #18 subsequently added `backend/test_integration.py`, a separate suite that builds the real C++ executable with CMake and exercises the Flask API.

The original eight tests cover helper behaviors and API validation/error responses using a placeholder executable. The new real-executable integration suite covers successful Compress, List, and Extract requests, text/binary/empty-file round trips, nested paths, custom archive naming, duplicate names, invalid and zero-byte archives, configured upload limits, exact configured boundaries, and temporary-workspace cleanup. No Python code coverage measurement was provided, so this report records **no Python coverage percentage**.


## 6A. Real-executable Flask API integration tests (PR #18)

PR #18 adds `backend/test_integration.py`. Its session fixture configures and builds the actual C++ executable with CMake, then supplies its path through `COMPRESSION_BINARY` to the Flask application. These tests use Flask's test client and do not use the placeholder executable used by the original helper/API tests.

The GitHub Actions run associated with PR #18 (run **43687260485**) completed successfully. Its build job reported successful CMake configure/build, C++ CTest, dependency installation, and Flask API test steps. The backend suite reported **28 passed, 0 failed**, and CTest reported **1/1 registered C++ tests passed**.

| Integration area | Evidence in `backend/test_integration.py` |
| --- | --- |
| Compress, List, Extract with real executable | Successful route requests and archive entry verification |
| Data integrity | Byte-for-byte text and binary round trips; empty-file preservation |
| Archive paths and naming | Nested upload path preservation and custom archive download filename |
| Upload validation | Duplicate filenames, configured file-count and per-file size limits |
| Boundary behavior | Exact configured per-file cap and exact multipart request cap, then one byte above |
| Invalid archives | Non-ZIP and zero-byte archives rejected through List and Extract |
| Temporary storage | Cleanup after successful Compress/List/Extract, invalid List input, synthetic executable failure, and synthetic timeout |

The exact-boundary tests use controlled small limits (3 bytes for the per-file limit and a small multipart request cap). They verify boundary logic but do not stress the default 50 MiB per-file and 100 MiB request limits. Validation-failure cleanup is not separately asserted. The integration results do not establish that every STP case has been executed.

## 7. Requirement traceability

The STP identifies `FCT-F-015` for invalid/corrupted-file handling, `FCT-NF-001` for lossless compression/decompression, and `FCT-NF-003` for data integrity. Based on the behavior checked in code, UT-CPP-02 through UT-CPP-05 and UT-PY-03 relate to `FCT-F-015`; UT-CPP-06 relates to `FCT-NF-001` and `FCT-NF-003`; UT-CPP-04, UT-CPP-05, and UT-CPP-07 through UT-CPP-10 relate to `FCT-NF-003`. The STP also lists requirement `FCT-NF-004` for clear success/error messages; UT-PY-04 through UT-PY-08 check specific JSON error responses related to it. These are evidence-based associations, not claims that the STP's separately planned test case IDs have been executed or that each requirement is fully verified.

## 8. Defects and limitations

No defect log or individual failing test result was supplied. The passing CTest result does not establish the absence of defects. The C++ executable combines many checks into one registered test, so CTest does not provide per-case timing or independent pass records. Some C++ checks depend on prior archive creation and filesystem state; the symlink checks are platform-dependent. The Python tests passed but cover selected validation paths rather than all API outcomes. The STP contains additional planned cases marked Not Run; this report does not convert those plans into executed results. React frontend testing is outside the evidence reviewed here.

## 9. Conclusion

The coverage-enabled C++ run passed **100% of the single registered CTest test**, which exercises ZIP component behavior and a compress/list/extract smoke flow. The eight focused Python pytest functions, including function-level and API component checks, all passed in the verified run. This evidence supports a scoped testing result, not a claim that all planned tests or the entire application passed.



