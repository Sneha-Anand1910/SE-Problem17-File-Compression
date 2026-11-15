"""Real Flask-to-C++ API integration tests.

These tests build the repository's actual C++ command-line executable and
exercise it through Flask's test client. They intentionally do not use the
placeholder executable from test_app.py.
"""
from __future__ import annotations

from io import BytesIO
import os
from pathlib import Path
import subprocess
import zipfile

import pytest

from backend.app import create_app


@pytest.fixture(scope="session")
def real_client(tmp_path_factory):
    repository_root = Path(__file__).resolve().parents[1]
    build_dir = tmp_path_factory.mktemp("real-compression-build")

    subprocess.run(
        ["cmake", "-S", str(repository_root), "-B", str(build_dir),
         "-DCMAKE_BUILD_TYPE=Release"],
        check=True, capture_output=True, text=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True, capture_output=True, text=True,
    )

    executable_name = "file-compress.exe" if os.name == "nt" else "file-compress"
    executable = build_dir / executable_name
    assert executable.is_file(), f"Expected built executable at {executable}"

    app = create_app({
        "TESTING": True,
        "COMPRESSION_BINARY": str(executable),
        "MAX_FILE_SIZE": 50 * 1024 * 1024,
        "MAX_CONTENT_LENGTH": 100 * 1024 * 1024,
    })
    return app.test_client()


def compress_files(client, files, archive_name="archive.zip"):
    data = {
        "archive_name": archive_name,
        "files": [(BytesIO(contents), name) for name, contents in files],
    }
    return client.post("/api/compress", data=data, content_type="multipart/form-data")


def zip_bytes(response):
    assert response.status_code == 200, response.get_data(as_text=True)
    assert response.mimetype == "application/zip"
    return response.get_data()


def test_real_executable_compresses_a_text_file(real_client):
    original = b"real executable integration test\n"
    response = compress_files(real_client, [("hello.txt", original)])

    with zipfile.ZipFile(BytesIO(zip_bytes(response))) as archive:
        assert archive.namelist() == ["hello.txt"]
        assert archive.read("hello.txt") == original


def test_real_executable_lists_compressed_files(real_client):
    response = compress_files(
        real_client, [("first.txt", b"one"), ("second.txt", b"two")]
    )
    archive_data = zip_bytes(response)

    listed = real_client.post(
        "/api/list",
        data={"file": (BytesIO(archive_data), "input.zip")},
        content_type="multipart/form-data",
    )
    assert listed.status_code == 200, listed.get_data(as_text=True)
    assert set(listed.json["files"]) == {"first.txt", "second.txt"}
    assert len(listed.json["files"]) == 2


def test_real_executable_extracts_and_preserves_file_bytes(real_client):
    original = b"round-trip through Flask and the real C++ executable"
    compressed = compress_files(real_client, [("roundtrip.txt", original)])

    extracted = real_client.post(
        "/api/extract",
        data={"file": (BytesIO(zip_bytes(compressed)), "input.zip")},
        content_type="multipart/form-data",
    )
    with zipfile.ZipFile(BytesIO(zip_bytes(extracted))) as archive:
        assert archive.namelist() == ["roundtrip.txt"]
        assert archive.read("roundtrip.txt") == original


def test_real_executable_preserves_binary_bytes(real_client):
    original = bytes([0, 1, 2, 127, 128, 254, 255, 0, 42])
    compressed = compress_files(real_client, [("payload.bin", original)])

    extracted = real_client.post(
        "/api/extract",
        data={"file": (BytesIO(zip_bytes(compressed)), "binary.zip")},
        content_type="multipart/form-data",
    )
    with zipfile.ZipFile(BytesIO(zip_bytes(extracted))) as archive:
        assert archive.read("payload.bin") == original


def test_real_executable_accepts_an_empty_file(real_client):
    compressed = compress_files(real_client, [("empty.txt", b"")])
    extracted = real_client.post(
        "/api/extract",
        data={"file": (BytesIO(zip_bytes(compressed)), "empty.zip")},
        content_type="multipart/form-data",
    )
    with zipfile.ZipFile(BytesIO(zip_bytes(extracted))) as archive:
        assert archive.namelist() == ["empty.txt"]
        assert archive.read("empty.txt") == b""


def test_real_executable_uses_requested_archive_name(real_client):
    response = compress_files(
        real_client, [("named.txt", b"custom archive")], archive_name="custom-output.zip"
    )
    zip_bytes(response)
    assert 'filename="custom-output.zip"' in response.headers["Content-Disposition"]



def test_real_executable_round_trips_nested_upload_paths(real_client):
    original = b"nested directory content"
    compressed = compress_files(real_client, [("reports/2026/summary.txt", original)])

    extracted = real_client.post(
        "/api/extract",
        data={"archive": (BytesIO(zip_bytes(compressed)), "nested.zip")},
        content_type="multipart/form-data",
    )
    with zipfile.ZipFile(BytesIO(zip_bytes(extracted))) as archive:
        assert "reports/2026/summary.txt" in archive.namelist()
        assert archive.read("reports/2026/summary.txt") == original



def test_real_api_rejects_duplicate_uploaded_names(real_client):
    response = compress_files(
        real_client, [("same.txt", b"first"), ("same.txt", b"second")]
    )
    assert response.status_code == 400
    assert response.json == {"error": "Duplicate uploaded filename"}


def test_real_api_rejects_non_zip_content_for_list_and_extract(real_client):
    listed = real_client.post(
        "/api/list",
        data={"file": (BytesIO(b"not a ZIP archive"), "invalid.zip")},
        content_type="multipart/form-data",
    )
    extracted = real_client.post(
        "/api/extract",
        data={"file": (BytesIO(b"not a ZIP archive"), "invalid.zip")},
        content_type="multipart/form-data",
    )
    assert listed.status_code == 400
    assert extracted.status_code == 400
    assert listed.json == {"error": "Invalid ZIP file"}
    assert extracted.json == {"error": "Invalid ZIP file"}
