# Flask backend

The backend exposes the frontend contract:

- `POST /api/compress`: multipart field `files` (one or more files) and optional `archive_name` (defaults to `archive.zip`).
- `POST /api/extract`: multipart field `file` (or `archive`) containing one ZIP. Returns `extracted.zip`.
- `POST /api/list`: multipart field `file` (or `archive`) containing one ZIP. Returns `{ "files": [...] }`.

Errors use `{ "error": "..." }`; malformed or unsafe ZIPs return `400` with `Invalid ZIP file`, while executable and processing failures return `500` with their specific message. Empty files are valid ZIP inputs. Uploads are limited to 100 MiB per request and 50 MiB per file by default. Override `MAX_CONTENT_LENGTH`, `MAX_FILE_SIZE`, `MAX_FILES`, and `COMPRESSION_BINARY` with environment variables.

## Run

Build the C++ executable first from the repository root:

```bash
cmake -S . -B build
cmake --build build --parallel
python -m pip install -r backend/requirements.txt
python -m backend.app
```

The frontend can use `http://127.0.0.1:5000`; CORS is enabled for development and can be restricted with `CORS_ORIGIN`.


## Backend API tests

Run the Flask tests from the repository root:

```bash
python -m pip install -r backend/requirements.txt
python -m pytest -q backend
```

The tests in `test_app.py` cover request validation and helper behavior with a
stub executable. The tests in `test_integration.py` are separate: they
configure and build the real C++ `file-compress` executable with CMake, then
exercise the Flask compress, list, and extract endpoints through Flask's test
client. They verify ZIP contents, round-trip bytes, nested upload paths,
invalid archive handling, upload limits, and temporary-workspace cleanup.
A working CMake toolchain, C++17 compiler, and zlib development package are
required for the real-executable integration suite.
