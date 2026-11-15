# System Validation Report — File Compression Tool

## 1. Objective

Validate the File Compression Tool's user-facing compress, list, and extract workflow through the React web interface, Flask API, and C++ compression engine. This report records four manually performed checks, including one SHA-256 file-integrity comparison. It does not treat automated CTest or pytest results as evidence for these manual checks.

## 2. System architecture and test environment

The tested workflow ran on Windows. The React + Vite frontend was served at `http://localhost:5173`, and the Flask backend at `http://127.0.0.1:5000`. The backend invokes a C++ compression binary built with MSYS2 UCRT64. Browser file selections were submitted through the interface to the Flask API; the backend passed ZIP operations to the C++ engine and returned downloadable ZIP files or archive listings.

The merged repository now includes `frontend/src/App.tsx` and `frontend/vite.config.ts`. Source review confirms the interface exposes Compress, Extract, and List ZIP Contents actions and proxies `/api` requests to the Flask backend at `http://127.0.0.1:5000`. The manual outcomes below remain based on the supplied execution record; source inspection is not a substitute for browser execution. The existing Software Test Plan (STP) was reviewed for requirement IDs.

## 3. Test methodology

One JPEG, one DOCX, and one PDF were selected in the browser and compressed with the entered archive name `Test1_Compression`. The resulting archive was then submitted through the List and Extract features. The downloaded extraction result was unpacked with Windows **Extract All**. Finally, PowerShell `Get-FileHash` was used to compare the original DOCX with its extracted copy using SHA-256. The outcomes in this report are the supplied observed results; no additional tests were run for this report.

## 4. Manual system test cases

The IDs in this table are report identifiers for the four performed checks, not IDs taken from the STP.

| ID | Steps | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| SV-01 | Select one JPEG, one DOCX, and one PDF; enter `Test1_Compression`; run Compress. | Compression completes and downloads `Test1_Compression.zip`. | Compression succeeded and `Test1_Compression.zip` downloaded. | **PASS** |
| SV-02 | Submit `Test1_Compression.zip` through List ZIP Contents. | The interface displays the three original filenames. | All three original filenames were displayed. | **PASS** |
| SV-03 | Submit `Test1_Compression.zip` through Extract; download `extracted.zip`; unpack it with Windows Extract All. | A downloadable extraction result contains all three original files. | `extracted.zip` downloaded; all three original files were present after unpacking. | **PASS** |
| SV-04 | Compare the original DOCX and its extracted copy with PowerShell `Get-FileHash` using SHA-256. | The two SHA-256 values match. | The hashes matched exactly, as recorded below. | **PASS** |

## 5. SHA-256 integrity verification evidence

| File compared | Original SHA-256 | Extracted SHA-256 | Result |
| --- | --- | --- | --- |
| DOCX | `E7A88F7EE204F130FEEEEB50A9BE291D0606B08F5145F424B9BA20E2342D50D5` | `E7A88F7EE204F130FEEEEB50A9BE291D0606B08F5145F424B9BA20E2342D50D5` | Match |

This verifies byte-for-byte identity for the tested DOCX. The JPEG and PDF were observed to be present after extraction, but no SHA-256 comparison for either file was reported.

## 6. Observed behavior and limitations

The three-file archive could be downloaded, listed, and extracted through the browser workflow. The extraction endpoint in `backend/app.py` invokes the C++ extractor, then **repackages the extracted files into `extracted.zip`** for a convenient browser download. Accordingly, `extracted.zip` is the transport artifact; Windows Extract All was used to inspect its contents.

This validation used one three-file archive containing the stated file types and one hash comparison. It does not establish behavior for other formats, file sizes, invalid archives, security cases, or other platforms. File presence alone does not prove byte-for-byte integrity of the JPEG or PDF. Source inspection confirms the frontend action components and development API proxy, but no new manual browser execution was performed as part of this report update.

## 7. Requirement traceability

The STP identifies `FCT-F-008` for creating a compressed output file, `FCT-F-014` for restoring original file contents, `FCT-NF-001` for lossless compression/decompression, and `FCT-NF-003` for data integrity. The following links describe the relationship between those requirements and the observed checks; they do not claim complete verification of each requirement.

| STP requirement ID | Relevant manual evidence | Scope of evidence |
| --- | --- | --- |
| `FCT-F-008` | SV-01 | Named ZIP output downloaded after compressing three files. |
| `FCT-F-014` | SV-03 | Three original files were present after extracting the returned ZIP. |
| `FCT-NF-001` | SV-04 | Lossless round trip verified by SHA-256 for the DOCX only. |
| `FCT-NF-003` | SV-04 | Data integrity verified by SHA-256 for the DOCX only. |

SV-02 validates the observed List feature, but this report does not assign it an STP requirement ID without a verified matching requirement. The STP's planned test cases and their statuses are separate from these performed manual checks.

## 8. Additional automated API integration evidence (PR #18)

A separate automated integration suite, `backend/test_integration.py`, builds the real C++ executable and exercises the Flask Compress, List, and Extract routes through Flask's test client. The associated GitHub Actions run **43687260485** completed successfully with **28 backend tests passed** and **1/1 registered C++ CTest tests passed**. The integration suite verifies text, binary, and empty-file behavior, byte-for-byte round trips, nested paths, custom archive naming, invalid and zero-byte archives, upload limits, configured exact-boundary behavior, and temporary-workspace cleanup for the tested success and failure paths.

This is automated API evidence and is separate from the four manual browser checks in Section 4. The controlled small-limit tests do not constitute full default 50 MiB/100 MiB stress tests. This report update does not claim that additional manual frontend tests were performed.

## 9. Overall validation summary and conclusion

All **four performed manual system checks passed** in the stated Windows environment. The interface successfully produced a named ZIP, displayed its three filenames, and returned an extraction ZIP containing the three files. The original and extracted DOCX had identical SHA-256 hashes. These results validate the exercised end-to-end workflow and DOCX integrity only; automated CTest and pytest execution are reported separately in the project's unit testing documentation.

