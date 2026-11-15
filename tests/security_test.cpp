#include "zip_archive.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

bool temporary_directory_cleanup_failed = false;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& name) {
        static std::atomic<unsigned long long> sequence{0};
        std::random_device random;
        const auto parent = fs::temp_directory_path();
        for (unsigned int attempt = 0; attempt < 128; ++attempt) {
            const auto nonce = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
            const auto candidate = parent /
                (name + "-" + std::to_string(nonce) + "-" +
                 std::to_string(random()) + "-" +
                 std::to_string(sequence.fetch_add(1)));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path_ = candidate;
                return;
            }
            if (!ec || ec == std::errc::file_exists) continue;
            throw fs::filesystem_error(
                "Could not create temporary test directory",
                candidate, ec);
        }
        throw std::runtime_error(
            "Could not allocate a unique temporary test directory.");
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
        if (ec) {
            temporary_directory_cleanup_failed = true;
            std::cerr << "Could not clean temporary test directory "
                      << path_ << ": " << ec.message() << "\n";
            return;
        }

        const bool remains = fs::exists(path_, ec);
        if (ec || remains) {
            temporary_directory_cleanup_failed = true;
            std::cerr << "Temporary test directory remains after cleanup "
                      << path_;
            if (ec) std::cerr << ": " << ec.message();
            std::cerr << "\n";
        }
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

int skip(const std::string& reason) {
    std::cout << "SKIP: " << reason << "\n";
    return 77;
}

void write_text(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Could not create test file.");
    out << contents;
    if (!out) throw std::runtime_error("Could not write test file.");
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
}

bool create_test_symlink(const fs::path& target,
                         const fs::path& link,
                         bool directory,
                         std::string& reason) {
#ifdef _WIN32
    constexpr DWORD allow_unprivileged_create = 0x2;
    const DWORD type_flag = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(
            link.c_str(), target.c_str(),
            type_flag | allow_unprivileged_create)) {
        return true;
    }

    auto error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER &&
        CreateSymbolicLinkW(link.c_str(), target.c_str(), type_flag)) {
        return true;
    }
    if (error == ERROR_INVALID_PARAMETER) error = GetLastError();
    reason = "CreateSymbolicLinkW failed with Windows error " +
             std::to_string(error) + ": " +
             std::system_category().message(static_cast<int>(error));
    return false;
#else
    try {
        if (directory) {
            fs::create_directory_symlink(target, link);
        } else {
            fs::create_symlink(target, link);
        }
        return true;
    } catch (const fs::filesystem_error& error) {
        reason = error.what();
        return false;
    }
#endif
}

int test_file_symlink() {
    TemporaryDirectory temp("sec-comp-symlink-file");
    const auto input = temp.path() / "input";
    const auto outside = temp.path() / "outside.txt";
    const auto link = input / "external.txt";
    const auto archive = temp.path() / "file-link.zip";
    fs::create_directories(input);
    write_text(outside, "synthetic external file contents");

    std::string link_error;
    if (!create_test_symlink(outside, link, false, link_error)) {
        return skip("file symlink creation unavailable: " + link_error);
    }

    bool rejected = false;
    try {
        ziptool::create_archive(archive, {{input}}, {true});
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("Symbolic links") !=
                   std::string::npos;
    }
    if (!rejected || fs::exists(archive)) {
        std::cerr << "File symlink compression was not rejected safely.\n";
        return 1;
    }
    std::cout << "PASS: file symlink rejected during compression.\n";
    return 0;
}

int test_directory_symlink() {
    TemporaryDirectory temp("sec-comp-symlink-dir");
    const auto input = temp.path() / "input";
    const auto outside = temp.path() / "outside";
    const auto archive = temp.path() / "directory-link.zip";
    fs::create_directories(input);
    write_text(outside / "external.txt", "synthetic external directory contents");

    std::string link_error;
    if (!create_test_symlink(outside, input / "external", true, link_error)) {
        return skip("directory symlink creation unavailable: " + link_error);
    }

    bool rejected = false;
    try {
        ziptool::create_archive(archive, {{input}}, {true});
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("Symbolic links") !=
                   std::string::npos;
    }
    if (!rejected || fs::exists(archive)) {
        std::cerr << "Directory symlink compression was not rejected safely.\n";
        return 1;
    }
    std::cout << "PASS: directory symlink rejected during compression.\n";
    return 0;
}

fs::path expected_race_parent;
fs::path race_outside;
fs::path detached_parent;
bool race_hook_called = false;
fs::path rollback_second_entry;
fs::path rollback_external;
bool rollback_hook_called = false;

void replace_parent_with_symlink(const fs::path& root,
                                 const fs::path& relative) {
    if (race_hook_called || relative.filename() != "payload.txt") return;
    race_hook_called = true;
    expected_race_parent = root / relative.parent_path();
    fs::rename(expected_race_parent, detached_parent);
    std::string link_error;
    if (!create_test_symlink(
            race_outside, expected_race_parent, true, link_error)) {
        throw std::runtime_error(
            "Could not create replacement symlink: " + link_error);
    }
}

void create_hardlink_commit_conflict(const fs::path& root,
                                     const fs::path&) {
    if (rollback_hook_called) return;
    rollback_hook_called = true;
    fs::create_hard_link(rollback_external, root / rollback_second_entry);
}

int test_extraction_race() {
    TemporaryDirectory temp("sec-extract-race");
    const auto source = temp.path() / "source";
    const auto archive = temp.path() / "race.zip";
    const auto extraction = temp.path() / "extracted";
    race_outside = temp.path() / "outside";
    detached_parent = temp.path() / "detached-parent";
    write_text(source / "parent" / "payload.txt", "synthetic archive payload");
    write_text(race_outside / "sentinel.txt", "outside sentinel unchanged");

    const auto probe = temp.path() / "symlink-probe";
    std::string link_error;
    const bool symlink_available =
        create_test_symlink(race_outside, probe, true, link_error);
    if (symlink_available) {
        fs::remove(probe);
        ziptool::create_archive(archive, {{source}}, {true});
        race_hook_called = false;
        ziptool::set_extraction_test_hook(replace_parent_with_symlink);
        bool rejected = false;
        std::string rejection_reason;
        try {
            ziptool::extract_archive(archive, extraction, {true});
        } catch (const std::runtime_error& error) {
            rejection_reason = error.what();
            rejected = rejection_reason.find("reparse point") != std::string::npos ||
                       rejection_reason.find("symbolic") != std::string::npos ||
                       rejection_reason.find("securely open directory") !=
                           std::string::npos;
        }
        ziptool::set_extraction_test_hook(nullptr);

        if (!race_hook_called || !rejected ||
            fs::exists(race_outside / "payload.txt") ||
            fs::exists(expected_race_parent / "payload.txt") ||
            fs::exists(extraction / "parent" / "payload.txt")) {
            std::cerr << "Extraction replacement race escaped the extraction root.\n";
            return 1;
        }
        if (read_text(race_outside / "sentinel.txt") !=
            "outside sentinel unchanged") {
            std::cerr << "Extraction race changed the outside sentinel.\n";
            return 1;
        }
    } else {
        std::cout << "SKIP: directory symlink unavailable for race test: "
                  << link_error << "\n";
    }

    const auto rollback_source = temp.path() / "rollback-source";
    const auto first_source = rollback_source / "first.txt";
    const auto second_source = rollback_source / "second.txt";
    const auto rollback_archive = temp.path() / "rollback.zip";
    const auto rollback_output = temp.path() / "rollback-output";
    rollback_external = temp.path() / "rollback-external.txt";
    write_text(first_source, "first archive output");
    write_text(second_source, "second archive output");
    write_text(rollback_external, "linked sentinel");
    ziptool::create_archive(
        rollback_archive, {{first_source}, {second_source}}, {true});
    const auto rollback_entries = ziptool::list_archive(rollback_archive);
    if (rollback_entries.size() != 2) {
        std::cerr << "Rollback test archive did not contain two files.\n";
        return 1;
    }
    const auto first_entry = fs::path(rollback_entries[0]);
    rollback_second_entry = fs::path(rollback_entries[1]);
    write_text(rollback_output / first_entry, "original output");
    rollback_hook_called = false;
    ziptool::set_extraction_test_hook(create_hardlink_commit_conflict);
    bool rollback_rejected = false;
    try {
        ziptool::extract_archive(rollback_archive, rollback_output, {true});
    } catch (const std::runtime_error&) {
        rollback_rejected = true;
    }
    ziptool::set_extraction_test_hook(nullptr);

    std::ifstream linked_file(rollback_external, std::ios::binary);
    const std::string linked_contents(
        (std::istreambuf_iterator<char>(linked_file)),
        std::istreambuf_iterator<char>());
    if (!rollback_hook_called || !rollback_rejected ||
        read_text(rollback_output / first_entry) != "original output" ||
        read_text(rollback_output / rollback_second_entry) !=
            "linked sentinel" ||
        linked_contents != "linked sentinel") {
        std::cerr << "Failed extraction did not roll back published files safely.\n";
        return 1;
    }
    std::cout << "PASS: failed publish rolled back earlier files safely.\n";
    if (!symlink_available) {
        return skip(
            "directory symlink creation unavailable for race test: " +
            link_error);
    }
    std::cout << "PASS: replaced extraction parent was rejected safely.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int result = 1;
    try {
        if (argc != 2) {
            std::cerr << "Expected one security test case name.\n";
            return 2;
        }
        const std::string test = argv[1];
        if (test == "file-symlink") {
            result = test_file_symlink();
        } else if (test == "directory-symlink") {
            result = test_directory_symlink();
        } else if (test == "extract-race") {
            result = test_extraction_race();
        } else {
            std::cerr << "Unknown security test case: " << test << "\n";
            result = 2;
        }
    } catch (const std::exception& error) {
        ziptool::set_extraction_test_hook(nullptr);
        std::cerr << "Security test failed: " << error.what() << "\n";
        result = 1;
    }
    return temporary_directory_cleanup_failed ? 1 : result;
}
