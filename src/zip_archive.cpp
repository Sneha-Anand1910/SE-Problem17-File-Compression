#include "zip_archive.hpp"
#include "huffman.hpp"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ziptool {
namespace {

constexpr std::uint32_t LOCAL_SIG = 0x04034b50;
constexpr std::uint32_t CENTRAL_SIG = 0x02014b50;
constexpr std::uint32_t END_SIG = 0x06054b50;
constexpr std::uint16_t ZIP_VERSION = 20;
constexpr std::uint16_t METHOD_STORE = 0;
constexpr std::uint16_t METHOD_DEFLATE = 8;

#ifdef ZIPTOOL_ENABLE_TEST_HOOKS
ExtractionTestHook extraction_test_hook = nullptr;
#endif

std::filesystem::file_status no_follow_status(
    const std::filesystem::path& path);

std::filesystem::path unique_sibling_name(const char* prefix) {
    static const std::uint64_t nonce = [] {
        std::random_device random;
        return (static_cast<std::uint64_t>(random()) << 32) ^ random();
    }();
    static std::atomic<unsigned long long> sequence{0};
    return std::filesystem::path(
        std::string(prefix) + std::to_string(nonce) + "-" +
        std::to_string(sequence.fetch_add(1)) + ".tmp");
}

void write_u16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff)
    };
    out.write(bytes, 2);
}

void write_u32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff)
    };
    out.write(bytes, 4);
}

std::uint16_t read_u16(std::istream& in) {
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    if (!in) throw std::runtime_error("Unexpected end of archive.");
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

std::uint32_t read_u32(std::istream& in) {
    unsigned char b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    if (!in) throw std::runtime_error("Unexpected end of archive.");
    return static_cast<std::uint32_t>(b[0] | (b[1] << 8) |
                                      (b[2] << 16) | (b[3] << 24));
}

void ensure_u32(std::uint64_t value, const char* label) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(label) +
                                 " exceeds the basic ZIP 32-bit format limit.");
    }
}

#ifdef _WIN32
class NativeHandle {
public:
    explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~NativeHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = other.value_;
            other.value_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

std::string windows_error(const std::string& operation,
                          const std::filesystem::path& path) {
    return operation + ": " + path.string() + " (Windows error " +
           std::to_string(GetLastError()) + ")";
}

void validate_windows_directory(HANDLE handle,
                                const std::filesystem::path& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        throw std::runtime_error(windows_error("Cannot inspect directory", path));
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error(
            "Reparse points are not allowed in filesystem paths: " +
            path.string());
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        throw std::runtime_error(
            "Filesystem path component is not a directory: " + path.string());
    }
}

std::vector<NativeHandle> open_windows_directories(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    bool create_missing) {
    constexpr DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    constexpr DWORD open_flags =
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;

    std::vector<NativeHandle> handles;
    auto current = root.root_path();
    if (current.empty()) {
        throw std::runtime_error("Filesystem root is not absolute: " + root.string());
    }

    auto open_directory = [&](const std::filesystem::path& path,
                              bool allow_create) {
        if (create_missing && allow_create &&
            !CreateDirectoryW(path.c_str(), nullptr)) {
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                throw std::runtime_error(windows_error(
                    "Cannot create extraction directory", path));
            }
        }

        NativeHandle handle(CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES, share_mode, nullptr,
            OPEN_EXISTING, open_flags, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                windows_error("Cannot securely open directory", path));
        }
        validate_windows_directory(handle.get(), path);
        handles.push_back(std::move(handle));
    };

    open_directory(current, false);
    for (const auto& part : root.relative_path()) {
        if (part == L".") continue;
        current /= part;
        open_directory(current, true);
    }
    for (const auto& part : relative) {
        if (part == L".") continue;
        current /= part;
        open_directory(current, true);
    }
    return handles;
}

std::vector<std::uint8_t> read_file_no_follow(
    const std::filesystem::path& root,
    const std::filesystem::path& relative) {
    auto directories = open_windows_directories(
        root, relative.parent_path(), false);
    const auto path = root / relative;
    NativeHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(windows_error("Cannot securely open input file", path));
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        throw std::runtime_error(windows_error("Cannot inspect input file", path));
    }
    if ((info.dwFileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY |
          FILE_ATTRIBUTE_DEVICE)) != 0 ||
        GetFileType(file.get()) != FILE_TYPE_DISK) {
        throw std::runtime_error(
            "Input is not a regular file or is a reparse point: " + path.string());
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Cannot determine input file size: " + path.string());
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = static_cast<DWORD>(
            (std::min)(data.size() - offset,
                       static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD read = 0;
        if (!ReadFile(file.get(), data.data() + offset, count, &read, nullptr) ||
            read == 0) {
            throw std::runtime_error("Failed while reading: " + path.string());
        }
        offset += read;
    }
    return data;
}

std::filesystem::path stage_file_no_follow(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const std::vector<std::uint8_t>& data) {
    auto directories = open_windows_directories(
        root, relative.parent_path(), true);
    const auto path = root / relative;
    const auto temp_name = unique_sibling_name(".extract-");
    const auto temp = path.parent_path() / temp_name;
    NativeHandle file(CreateFileW(
        temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(windows_error(
            "Cannot create temporary extraction file", temp));
    }

    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count = static_cast<DWORD>(
                (std::min)(data.size() - offset,
                           static_cast<std::size_t>(
                               std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteFile(file.get(), data.data() + offset, count,
                           &written, nullptr) || written == 0) {
                throw std::runtime_error(
                    "Failed while staging extracted file: " + path.string());
            }
            offset += written;
        }
        file = NativeHandle();
        return temp_name;
    } catch (...) {
        file = NativeHandle();
        DeleteFileW(temp.c_str());
        throw;
    }
}

std::filesystem::path backup_existing_file(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    bool overwrite) {
    auto directories = open_windows_directories(
        root, relative.parent_path(), false);
    const auto path = root / relative;
    const auto existing = no_follow_status(path);
    if (existing.type() == std::filesystem::file_type::symlink) {
        throw std::runtime_error(
            "Refusing to overwrite a symbolic link: " + path.string());
    }
    if (existing.type() != std::filesystem::file_type::not_found) {
        if (!std::filesystem::is_regular_file(existing)) {
            throw std::runtime_error(
                "Extraction target is not a regular file: " + path.string());
        }
        if (!overwrite) {
            throw std::runtime_error(
                "File already exists. Use --yes to overwrite: " + path.string());
        }
        {
            NativeHandle target(CreateFileW(
                path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (target.get() == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandle(target.get(), &info)) {
                throw std::runtime_error(windows_error(
                    "Cannot inspect extraction target", path));
            }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                info.nNumberOfLinks > 1) {
                throw std::runtime_error(
                    "Refusing to overwrite a reparse point or hard-linked file: " +
                    path.string());
            }
        }

        const auto backup_name = unique_sibling_name(".backup-");
        const auto backup = path.parent_path() / backup_name;
        if (!MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(
                windows_error("Cannot preserve existing extraction target", path));
        }
        return backup_name;
    }
    return {};
}

void publish_file_no_follow(const std::filesystem::path& root,
                            const std::filesystem::path& relative,
                            const std::filesystem::path& temp_name,
                            bool& published) {
    auto directories = open_windows_directories(
        root, relative.parent_path(), false);
    const auto path = root / relative;
    const auto temp = path.parent_path() / temp_name;
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error(
            windows_error("Cannot publish extracted file", path));
    }
    published = true;
}

bool remove_published_file(const std::filesystem::path& root,
                           const std::filesystem::path& relative) noexcept {
    try {
        auto directories = open_windows_directories(
            root.root_path(),
            root.relative_path() / relative.parent_path(), false);
        (void)directories;
        return DeleteFileW((root / relative).c_str()) != 0 ||
               GetLastError() == ERROR_FILE_NOT_FOUND;
    } catch (...) {
        return false;
    }
}

bool restore_backup_file(const std::filesystem::path& root,
                         const std::filesystem::path& relative,
                         const std::filesystem::path& backup_name) noexcept {
    try {
        auto directories = open_windows_directories(
            root.root_path(),
            root.relative_path() / relative.parent_path(), false);
        (void)directories;
        const auto target = root / relative;
        const auto backup = target.parent_path() / backup_name;
        return MoveFileExW(
            backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
    } catch (...) {
        return false;
    }
}

bool discard_backup_file(const std::filesystem::path& root,
                         const std::filesystem::path& relative,
                         const std::filesystem::path& backup_name) noexcept {
    try {
        auto directories = open_windows_directories(
            root.root_path(),
            root.relative_path() / relative.parent_path(), false);
        (void)directories;
        const auto backup = (root / relative).parent_path() / backup_name;
        return DeleteFileW(backup.c_str()) != 0 ||
               GetLastError() == ERROR_FILE_NOT_FOUND;
    } catch (...) {
        return false;
    }
}

bool remove_staged_file(const std::filesystem::path& root,
                        const std::filesystem::path& relative,
                        const std::filesystem::path& temp_name) noexcept {
    try {
        auto directories = open_windows_directories(
            root.root_path(),
            root.relative_path() / relative.parent_path(), false);
        (void)directories;
        const auto temp = (root / relative).parent_path() / temp_name;
        return DeleteFileW(temp.c_str()) != 0 ||
               GetLastError() == ERROR_FILE_NOT_FOUND;
    } catch (...) {
        return false;
    }
}
#else
class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) ::close(value_);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_(other.value_) {
        other.value_ = -1;
    }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = other.value_;
            other.value_ = -1;
        }
        return *this;
    }

    int get() const { return value_; }

private:
    int value_;
};

std::string posix_error(const std::string& operation,
                        const std::filesystem::path& path) {
    return operation + ": " + path.string() + " (" + std::strerror(errno) + ")";
}

std::vector<FileDescriptor> open_posix_directories(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    bool create_missing) {
    std::vector<FileDescriptor> descriptors;
    int root_fd = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        throw std::runtime_error(posix_error("Cannot securely open filesystem root", root));
    }
    descriptors.emplace_back(root_fd);

    auto open_component = [&](const std::filesystem::path& part) {
        if (part.empty() || part == "." || part == "/") return;
        const auto& parent = descriptors.back();
        int next = ::openat(parent.get(), part.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create_missing) {
            if (::mkdirat(parent.get(), part.c_str(), 0777) < 0 && errno != EEXIST) {
                throw std::runtime_error(posix_error(
                    "Cannot create extraction directory", root / relative));
            }
            next = ::openat(parent.get(), part.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            throw std::runtime_error(posix_error(
                "Cannot securely open directory", root / relative));
        }
        descriptors.emplace_back(next);
    };

    for (const auto& part : root.relative_path()) {
        open_component(part);
    }
    for (const auto& part : relative) {
        open_component(part);
    }
    return descriptors;
}

std::vector<FileDescriptor> open_posix_relative_directories(
    int root_fd,
    const std::filesystem::path& relative,
    bool create_missing) {
    std::vector<FileDescriptor> descriptors;
    const int first = ::dup(root_fd);
    if (first < 0) {
        throw std::runtime_error(
            posix_error("Cannot duplicate extraction root handle", "."));
    }
    descriptors.emplace_back(first);

    for (const auto& part : relative) {
        if (part.empty() || part == ".") continue;
        const auto& parent = descriptors.back();
        int next = ::openat(parent.get(), part.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create_missing) {
            if (::mkdirat(parent.get(), part.c_str(), 0777) < 0 &&
                errno != EEXIST) {
                throw std::runtime_error(posix_error(
                    "Cannot create extraction directory", relative));
            }
            next = ::openat(parent.get(), part.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            throw std::runtime_error(posix_error(
                "Cannot securely open directory", relative));
        }
        descriptors.emplace_back(next);
    }
    return descriptors;
}

std::vector<std::uint8_t> read_file_no_follow(
    const std::filesystem::path& root,
    const std::filesystem::path& relative) {
    auto directories = open_posix_directories(
        root, relative.parent_path(), false);
    const auto& parent = directories.back();
    const auto path = root / relative;
    FileDescriptor file(::openat(
        parent.get(), relative.filename().c_str(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (file.get() < 0) {
        throw std::runtime_error(posix_error("Cannot securely open input file", path));
    }

    struct stat info{};
    if (::fstat(file.get(), &info) < 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 ||
        static_cast<unsigned long long>(info.st_size) >
            std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Input is not a regular file: " + path.string());
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = (std::min)(
            data.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto read = ::read(file.get(), data.data() + offset, count);
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) {
            throw std::runtime_error("Failed while reading: " + path.string());
        }
        offset += static_cast<std::size_t>(read);
    }
    return data;
}

std::filesystem::path stage_file_no_follow(
    int root_fd,
    const std::filesystem::path& relative,
    const std::vector<std::uint8_t>& data) {
    auto directories = open_posix_relative_directories(
        root_fd, relative.parent_path(), true);
    const auto& parent = directories.back();
    const auto path = relative;
    const auto temp_name = unique_sibling_name(".extract-");
    FileDescriptor file(::openat(
        parent.get(), temp_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) {
        throw std::runtime_error(
            posix_error("Cannot create temporary extraction file", path));
    }

    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count = (std::min)(
                data.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto written = ::write(
                file.get(), data.data() + offset, count);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                throw std::runtime_error(
                    posix_error("Failed while staging extracted file", path));
            }
            offset += static_cast<std::size_t>(written);
        }
        file = FileDescriptor();
        return temp_name;
    } catch (...) {
        ::unlinkat(parent.get(), temp_name.c_str(), 0);
        throw;
    }
}

std::filesystem::path backup_existing_file(
    int root_fd,
    const std::filesystem::path& relative,
    bool overwrite) {
    auto directories = open_posix_relative_directories(
        root_fd, relative.parent_path(), false);
    const auto& parent = directories.back();
    const auto path = relative;
    struct stat existing{};
    if (::fstatat(parent.get(), relative.filename().c_str(), &existing,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(existing.st_mode) || !S_ISREG(existing.st_mode)) {
            throw std::runtime_error(
                "Extraction target is not a regular file: " + path.string());
        }
        if (!overwrite) {
            throw std::runtime_error(
                "File already exists. Use --yes to overwrite: " + path.string());
        }
        if (existing.st_nlink > 1) {
            throw std::runtime_error(
                "Refusing to overwrite a hard-linked file: " + path.string());
        }
        const auto backup_name = unique_sibling_name(".backup-");
        if (::renameat(parent.get(), relative.filename().c_str(),
                       parent.get(), backup_name.c_str()) < 0) {
            throw std::runtime_error(
                posix_error("Cannot preserve existing extraction target", path));
        }
        return backup_name;
    } else if (errno != ENOENT) {
        throw std::runtime_error(posix_error("Cannot inspect extraction target", path));
    }

    return {};
}

void publish_file_no_follow(int root_fd,
                            const std::filesystem::path& relative,
                            const std::filesystem::path& temp_name,
                            bool& published) {
    auto directories = open_posix_relative_directories(
        root_fd, relative.parent_path(), false);
    const auto& parent = directories.back();
    const auto path = relative;
    if (::linkat(parent.get(), temp_name.c_str(),
                 parent.get(), relative.filename().c_str(), 0) < 0) {
        if (errno == EEXIST) {
            throw std::runtime_error(
                "File already exists. Use --yes to overwrite: " + path.string());
        }
        throw std::runtime_error(
            posix_error("Cannot publish extracted file", path));
    }
    published = true;
    if (::unlinkat(parent.get(), temp_name.c_str(), 0) < 0) {
        throw std::runtime_error(
            posix_error("Cannot remove temporary extraction file", path));
    }
}

bool remove_published_file(int root_fd,
                           const std::filesystem::path& relative) noexcept {
    int current = ::dup(root_fd);
    if (current < 0) return false;
    for (const auto& part : relative.parent_path()) {
        const int next = ::openat(
            current, part.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(current);
        if (next < 0) return false;
        current = next;
    }
    const bool removed =
        ::unlinkat(current, relative.filename().c_str(), 0) == 0 ||
        errno == ENOENT;
    ::close(current);
    return removed;
}

bool restore_backup_file(int root_fd,
                         const std::filesystem::path& relative,
                         const std::filesystem::path& backup_name) noexcept {
    int current = ::dup(root_fd);
    if (current < 0) return false;
    for (const auto& part : relative.parent_path()) {
        const int next = ::openat(
            current, part.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(current);
        if (next < 0) return false;
        current = next;
    }
    const bool restored = ::renameat(
        current, backup_name.c_str(), current, relative.filename().c_str()) == 0;
    ::close(current);
    return restored;
}

bool discard_backup_file(int root_fd,
                         const std::filesystem::path& relative,
                         const std::filesystem::path& backup_name) noexcept {
    int current = ::dup(root_fd);
    if (current < 0) return false;
    for (const auto& part : relative.parent_path()) {
        const int next = ::openat(
            current, part.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(current);
        if (next < 0) return false;
        current = next;
    }
    const bool removed =
        ::unlinkat(current, backup_name.c_str(), 0) == 0 || errno == ENOENT;
    ::close(current);
    return removed;
}

bool remove_staged_file(int root_fd,
                        const std::filesystem::path& relative,
                        const std::filesystem::path& temp_name) noexcept {
    int current = ::dup(root_fd);
    if (current < 0) return false;
    for (const auto& part : relative.parent_path()) {
        const int next = ::openat(
            current, part.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(current);
        if (next < 0) return false;
        current = next;
    }
    const bool removed =
        ::unlinkat(current, temp_name.c_str(), 0) == 0 || errno == ENOENT;
    ::close(current);
    return removed;
}
#endif

struct Entry {
    std::string name;
    bool directory{false};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> compressed;
    std::uint32_t crc{0};
    std::uint32_t local_offset{0};
    std::uint16_t method{METHOD_STORE};
};

std::string zip_name(const std::filesystem::path& p) {
    auto s = p.generic_string();
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

bool starts_with_path(const std::filesystem::path& child,
                      const std::filesystem::path& parent) {
    auto child_it = child.begin();
    auto parent_it = parent.begin();
    for (; parent_it != parent.end(); ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it) return false;
    }
    return true;
}

std::filesystem::path canonicalish(const std::filesystem::path& p) {
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(p, ec);
    if (ec) return std::filesystem::absolute(p).lexically_normal();
    return result.lexically_normal();
}


std::filesystem::path safe_entry_path(const std::string& name, bool directory) {
    if (name.empty() || name.find('\0') != std::string::npos) {
        throw std::runtime_error("Archive contains an invalid entry name.");
    }

    std::string normalized = name;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    if (normalized.front() == '/') {
        throw std::runtime_error("Absolute archive paths are not allowed: " + name);
    }

    const bool has_trailing_separator = normalized.back() == '/';
    if (has_trailing_separator != directory) {
        throw std::runtime_error("Archive entry type does not match its path: " + name);
    }
    if (directory) normalized.pop_back();
    if (normalized.empty()) {
        throw std::runtime_error("Archive contains an empty path.");
    }

    std::filesystem::path relative;
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto length = (end == std::string::npos)
            ? normalized.size() - start : end - start;
        const auto part = normalized.substr(start, length);

        if (part.empty() || part == "." || part == ".." ||
            part.find(':') != std::string::npos ||
            part.back() == '.' || part.back() == ' ') {
            throw std::runtime_error("Unsafe archive path rejected: " + name);
        }

        std::string device = part.substr(0, part.find('.'));
        std::transform(device.begin(), device.end(), device.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        const bool numbered_device =
            (device.size() == 4 &&
             (device.compare(0, 3, "COM") == 0 ||
              device.compare(0, 3, "LPT") == 0) &&
             device[3] >= '1' && device[3] <= '9');
        if (device == "CON" || device == "PRN" || device == "AUX" ||
            device == "NUL" || numbered_device) {
            throw std::runtime_error("Unsafe archive path rejected: " + name);
        }

        relative /= part;
        if (end == std::string::npos) break;
        start = end + 1;
    }

    if (relative.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory()) {
        throw std::runtime_error("Unsafe archive path rejected: " + name);
    }
    return relative;
}


std::filesystem::file_status no_follow_status(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory) {
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    if (ec) {
        throw std::runtime_error("Cannot inspect extraction path: " + path.string());
    }
    return status;
}

void check_safe_directory(const std::filesystem::path& root,
                          const std::filesystem::path& relative,
                          bool create_missing) {
    auto current = root;
    for (const auto& part : relative) {
        current /= part;
        auto status = no_follow_status(current);

        if (status.type() == std::filesystem::file_type::symlink) {
            throw std::runtime_error(
                "Symbolic links are not allowed in extraction paths: " +
                current.string());
        }

        if (status.type() == std::filesystem::file_type::not_found) {
            if (!create_missing) continue;

            std::error_code ec;
            std::filesystem::create_directory(current, ec);
            if (ec && ec != std::errc::file_exists) {
                throw std::runtime_error(
                    "Cannot create extraction directory: " + current.string());
            }
            status = no_follow_status(current);
            if (status.type() == std::filesystem::file_type::symlink) {
                throw std::runtime_error(
                    "Symbolic links are not allowed in extraction paths: " +
                    current.string());
            }
        }

        if (!std::filesystem::is_directory(status)) {
            throw std::runtime_error(
                "Extraction path component is not a directory: " +
                current.string());
        }

        if (!starts_with_path(canonicalish(current), root)) {
            throw std::runtime_error(
                "Extraction path resolves outside the destination: " +
                current.string());
        }
    }
}

void check_safe_file_target(const std::filesystem::path& root,
                            const std::filesystem::path& relative,
                            bool overwrite) {
    const auto target = root / relative;
    const auto status = no_follow_status(target);

    if (status.type() == std::filesystem::file_type::symlink) {
        throw std::runtime_error(
            "Refusing to write through a symbolic link: " + target.string());
    }

    if (status.type() != std::filesystem::file_type::not_found) {
        if (!std::filesystem::is_regular_file(status)) {
            throw std::runtime_error(
                "Extraction target is not a regular file: " + target.string());
        }
        if (!overwrite) {
            throw std::runtime_error(
                "File already exists. Use --yes to overwrite: " + target.string());
        }
    }

    if (!starts_with_path(canonicalish(target), root)) {
        throw std::runtime_error(
            "Extraction target resolves outside the destination: " +
            target.string());
    }
}

std::vector<std::uint8_t> deflate_data(const std::vector<std::uint8_t>& input) {
    if (input.empty()) return {};

    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("Deflate initialization failed.");
    }

    const auto bound = deflateBound(&stream, static_cast<uLong>(input.size()));
    std::vector<std::uint8_t> output(bound);

    stream.next_in = const_cast<Bytef*>(
        reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int rc = deflate(&stream, Z_FINISH);
    const bool ok = (rc == Z_STREAM_END);
    const auto produced = stream.total_out;
    deflateEnd(&stream);

    if (!ok) throw std::runtime_error("Deflate compression failed.");
    output.resize(produced);
    return output;
}

std::vector<std::uint8_t> inflate_data(const std::vector<std::uint8_t>& input,
                                       std::size_t expected_size) {
    std::vector<std::uint8_t> output(expected_size);
    if (expected_size == 0) return output;

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Inflate initialization failed.");
    }

    stream.next_in = const_cast<Bytef*>(
        reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int rc = inflate(&stream, Z_FINISH);
    const bool ok = (rc == Z_STREAM_END &&
                     stream.total_out == expected_size);
    inflateEnd(&stream);

    if (!ok) throw std::runtime_error(
        "Archive entry failed DEFLATE decompression.");
    return output;
}

void collect_path(const std::filesystem::path& path,
                  const std::filesystem::path& base,
                  std::vector<Entry>& entries,
                  std::set<std::string>& names) {
    std::error_code ec;
    const auto link_status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::exists(link_status)) {
        throw std::runtime_error(
            "Input does not exist or is inaccessible: " + path.string());
    }

    if (link_status.type() == std::filesystem::file_type::symlink) {
        throw std::runtime_error(
            "Symbolic links are not allowed in compression inputs: " +
            path.string());
    }

    const auto status = link_status;
    if (std::filesystem::is_directory(status)) {
        const auto relative = path.lexically_relative(base);
        if (relative.empty()) {
            throw std::runtime_error(
                "Unable to determine relative path for: " + path.string());
        }

        const auto dir_name = zip_name(relative) + "/";
        if (dir_name != "/" && names.insert(dir_name).second) {
            Entry e;
            e.name = dir_name;
            e.directory = true;
            entries.push_back(std::move(e));
        }

        for (const auto& item :
             std::filesystem::directory_iterator(
                 path,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec)) {
            if (ec) throw std::runtime_error(
                "Unable to enumerate directory: " + path.string());
            collect_path(item.path(), base, entries, names);
        }
        return;
    }

    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("Unsupported input type: " + path.string());
    }

    const auto relative = path.lexically_relative(base);
    if (relative.empty()) {
        throw std::runtime_error(
            "Unable to determine relative path for: " + path.string());
    }

    const auto entry_name = zip_name(relative);
    if (!names.insert(entry_name).second) {
        throw std::runtime_error(
            "Duplicate archive entry name: " + entry_name);
    }

    Entry e;
    e.name = entry_name;
    e.data = read_file_no_follow(base, relative);
    e.crc = ::crc32(0L, Z_NULL, 0);
    if (!e.data.empty()) {
        e.crc = ::crc32(e.crc, e.data.data(),
                        static_cast<uInt>(e.data.size()));
    }

    e.compressed = deflate_data(e.data);
    if (!e.data.empty() && e.compressed.size() < e.data.size()) {
        e.method = METHOD_DEFLATE;
    } else {
        e.method = METHOD_STORE;
        e.compressed = e.data;
    }

    entries.push_back(std::move(e));
}

std::vector<Entry> build_entries(const std::vector<InputPath>& inputs) {
    if (inputs.empty()) {
        throw std::runtime_error("At least one input file or folder is required.");
    }

    std::vector<Entry> entries;
    std::set<std::string> names;
    for (const auto& item : inputs) {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(item.path, ec).lexically_normal();
        if (ec) {
            throw std::runtime_error(
                "Invalid input path: " + item.path.string());
        }

        const auto link_status = std::filesystem::symlink_status(abs, ec);
        if (ec || !std::filesystem::exists(link_status)) {
            throw std::runtime_error(
                "Input does not exist or is inaccessible: " +
                item.path.string());
        }
        if (link_status.type() == std::filesystem::file_type::symlink) {
            throw std::runtime_error(
                "Symbolic links are not allowed in compression inputs: " +
                item.path.string());
        }

        const auto base = abs.parent_path();
        const auto selected = base / abs.filename();
        collect_path(selected, base, entries, names);
    }

    return entries;

}

struct CentralRecord {
    std::string name;
    std::uint16_t method{};
    std::uint32_t crc{};
    std::uint32_t compressed_size{};
    std::uint32_t uncompressed_size{};
    std::uint32_t local_offset{};
    bool directory{false};
};

std::vector<CentralRecord> read_central_directory(std::ifstream& in) {
    in.seekg(0, std::ios::end);
    const auto end_position = in.tellg();
    if (end_position < 0) {
        throw std::runtime_error("Cannot determine archive size.");
    }
    const auto file_size = static_cast<std::uint64_t>(end_position);
    constexpr std::uint64_t EOCD_SIZE = 22;
    if (file_size < EOCD_SIZE) {
        throw std::runtime_error("Not a valid basic ZIP archive: file is too small.");
    }

    const auto search = std::min<std::uint64_t>(file_size, 65557);
    in.seekg(static_cast<std::streamoff>(file_size - search), std::ios::beg);
    std::vector<char> tail(static_cast<std::size_t>(search));
    in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    if (!in) throw std::runtime_error("Cannot read archive tail.");

    auto read16_tail = [&](std::size_t pos) -> std::uint16_t {
        if (pos > tail.size() || tail.size() - pos < 2) {
            throw std::runtime_error("Corrupted archive: incomplete end record.");
        }
        return static_cast<std::uint16_t>(
            static_cast<unsigned char>(tail[pos]) |
            (static_cast<unsigned char>(tail[pos + 1]) << 8));
    };
    auto read32_tail = [&](std::size_t pos) -> std::uint32_t {
        if (pos > tail.size() || tail.size() - pos < 4) {
            throw std::runtime_error("Corrupted archive: incomplete end record.");
        }
        return static_cast<std::uint32_t>(
            static_cast<unsigned char>(tail[pos]) |
            (static_cast<unsigned char>(tail[pos + 1]) << 8) |
            (static_cast<unsigned char>(tail[pos + 2]) << 16) |
            (static_cast<unsigned char>(tail[pos + 3]) << 24));
    };

    std::size_t eocd_pos = std::string::npos;
    for (std::size_t pos = tail.size() - 4 + 1; pos-- > 0;) {
        if (tail[pos] == 'P' && tail[pos + 1] == 'K' &&
            static_cast<unsigned char>(tail[pos + 2]) == 0x05 &&
            static_cast<unsigned char>(tail[pos + 3]) == 0x06) {
            if (pos + EOCD_SIZE > tail.size()) continue;
            const auto comment_len = read16_tail(pos + 20);
            if (pos + EOCD_SIZE + comment_len == tail.size()) {
                eocd_pos = pos;
                break;
            }
        }
    }
    if (eocd_pos == std::string::npos) {
        throw std::runtime_error(
            "Not a valid basic ZIP archive: complete end record not found.");
    }

    const auto disk_number = read16_tail(eocd_pos + 4);
    const auto central_disk = read16_tail(eocd_pos + 6);
    const auto disk_count = read16_tail(eocd_pos + 8);
    const auto count = read16_tail(eocd_pos + 10);
    const auto central_size = read32_tail(eocd_pos + 12);
    const auto central_offset = read32_tail(eocd_pos + 16);
    const auto eocd_absolute = file_size - search + eocd_pos;

    if (disk_number != 0 || central_disk != 0 || disk_count != count) {
        throw std::runtime_error(
            "Archive uses unsupported multi-disk ZIP metadata.");
    }
    if (static_cast<std::uint64_t>(central_offset) + central_size !=
        eocd_absolute) {
        throw std::runtime_error(
            "Corrupted archive: central directory bounds are inconsistent.");
    }

    in.clear();
    in.seekg(static_cast<std::streamoff>(central_offset), std::ios::beg);
    if (!in) throw std::runtime_error("Cannot seek to central directory.");

    std::vector<CentralRecord> records;
    records.reserve(count);
    const auto central_end = static_cast<std::uint64_t>(central_offset) +
                             central_size;

    for (std::uint16_t i = 0; i < count; ++i) {
        const auto entry_start = in.tellg();
        if (entry_start < 0 ||
            static_cast<std::uint64_t>(entry_start) + 46 > central_end) {
            throw std::runtime_error(
                "Corrupted archive: central-directory entry exceeds its bounds.");
        }
        if (read_u32(in) != CENTRAL_SIG) {
            throw std::runtime_error(
                "Corrupted archive: bad central-directory entry.");
        }

        read_u16(in); // version made by
        read_u16(in); // version needed
        const auto flags = read_u16(in);
        if (flags != 0) {
            throw std::runtime_error(
                "Archive uses unsupported flags; only basic ZIP entries are supported.");
        }

        const auto method = read_u16(in);
        read_u16(in); // time
        read_u16(in); // date
        const auto crc = read_u32(in);
        const auto csize = read_u32(in);
        const auto usize = read_u32(in);
        const auto name_len = read_u16(in);
        const auto extra_len = read_u16(in);
        const auto comment_len = read_u16(in);
        const auto disk_start = read_u16(in);
        read_u16(in); // internal attrs
        read_u32(in); // external attrs
        const auto offset = read_u32(in);

        const auto variable_size = static_cast<std::uint64_t>(name_len) +
                                   extra_len + comment_len;
        const auto current = in.tellg();
        if (disk_start != 0 || current < 0 ||
            static_cast<std::uint64_t>(current) + variable_size > central_end) {
            throw std::runtime_error(
                "Corrupted archive: invalid central-directory entry metadata.");
        }

        std::string name(name_len, '\0');
        if (name_len) in.read(name.data(), static_cast<std::streamsize>(name_len));
        in.seekg(static_cast<std::streamoff>(extra_len + comment_len),
                 std::ios::cur);
        if (!in) {
            throw std::runtime_error(
                "Corrupted archive: truncated central-directory entry.");
        }

        records.push_back({
            name, method, crc, csize, usize, offset,
            !name.empty() && name.back() == '/'
        });
    }

    const auto final_position = in.tellg();
    if (final_position < 0 ||
        static_cast<std::uint64_t>(final_position) != central_end) {
        throw std::runtime_error(
            "Corrupted archive: central-directory size does not match its entries.");
    }
    return records;
}

std::vector<std::uint8_t> read_entry_data(
    std::ifstream& in, const CentralRecord& record) {

    in.clear();
    in.seekg(static_cast<std::streamoff>(record.local_offset), std::ios::beg);
    if (read_u32(in) != LOCAL_SIG) {
        throw std::runtime_error("Corrupted archive: invalid local header.");
    }

    read_u16(in); // version
    const auto flags = read_u16(in);
    if (flags != 0) {
        throw std::runtime_error("Archive uses unsupported local-header flags.");
    }

    const auto method = read_u16(in);
    if (method != record.method) {
        throw std::runtime_error(
            "Corrupted archive: compression method mismatch.");
    }

    read_u16(in); // time
    read_u16(in); // date
    const auto local_crc = read_u32(in);
    const auto local_compressed_size = read_u32(in);
    const auto local_uncompressed_size = read_u32(in);
    if (local_crc != record.crc ||
        local_compressed_size != record.compressed_size ||
        local_uncompressed_size != record.uncompressed_size) {
        throw std::runtime_error(
            "Corrupted archive: local and central entry metadata mismatch.");
    }

    const auto name_len = read_u16(in);
    const auto extra_len = read_u16(in);
    in.seekg(static_cast<std::streamoff>(name_len + extra_len),
             std::ios::cur);
    if (!in) {
        throw std::runtime_error(
            "Corrupted archive: invalid local header lengths.");
    }

    const auto data_start = in.tellg();
    in.seekg(0, std::ios::end);
    const auto archive_end = in.tellg();
    if (data_start < 0 || archive_end < 0 ||
        static_cast<std::uint64_t>(data_start) > static_cast<std::uint64_t>(archive_end) ||
        record.compressed_size >
            static_cast<std::uint64_t>(archive_end) -
            static_cast<std::uint64_t>(data_start)) {
        throw std::runtime_error(
            "Corrupted archive: entry data extends beyond the archive.");
    }
    in.clear();
    in.seekg(data_start, std::ios::beg);

    std::vector<std::uint8_t> compressed(record.compressed_size);
    if (!compressed.empty()) {
        in.read(reinterpret_cast<char*>(compressed.data()),
                static_cast<std::streamsize>(compressed.size()));
    }
    if (!in && !compressed.empty()) {
        throw std::runtime_error(
            "Corrupted archive: entry data is truncated.");
    }

    std::vector<std::uint8_t> data;
    if (record.method == METHOD_STORE) {
        if (record.compressed_size != record.uncompressed_size) {
            throw std::runtime_error("Corrupted stored entry size.");
        }
        data = std::move(compressed);
    } else if (record.method == METHOD_DEFLATE) {
        data = inflate_data(compressed, record.uncompressed_size);
    } else {
        throw std::runtime_error(
            "Archive contains an unsupported compression method.");
    }

    auto actual_crc = ::crc32(0L, Z_NULL, 0);
    if (!data.empty()) {
        actual_crc = ::crc32(actual_crc, data.data(),
                             static_cast<uInt>(data.size()));
    }
    if (actual_crc != record.crc) {
        throw std::runtime_error(
            "CRC check failed for archive entry: " + record.name);
    }

    return data;
}

void write_local_entry(std::ofstream& out, Entry& entry) {
    const auto pos = static_cast<std::uint64_t>(out.tellp());
    ensure_u32(pos, "Archive offset");
    entry.local_offset = static_cast<std::uint32_t>(pos);

    write_u32(out, LOCAL_SIG);
    write_u16(out, ZIP_VERSION);
    write_u16(out, 0);
    write_u16(out, entry.directory ? METHOD_STORE : entry.method);
    write_u16(out, 0);
    write_u16(out, 0);
    write_u32(out, entry.crc);
    write_u32(out, static_cast<std::uint32_t>(entry.compressed.size()));
    write_u32(out, static_cast<std::uint32_t>(entry.data.size()));
    write_u16(out, static_cast<std::uint16_t>(entry.name.size()));
    write_u16(out, 0);
    out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    if (!entry.compressed.empty()) {
        out.write(reinterpret_cast<const char*>(entry.compressed.data()),
                  static_cast<std::streamsize>(entry.compressed.size()));
    }
}

void write_central_entry(std::ofstream& out, const Entry& entry) {
    write_u32(out, CENTRAL_SIG);
    write_u16(out, ZIP_VERSION);
    write_u16(out, ZIP_VERSION);
    write_u16(out, 0);
    write_u16(out, entry.directory ? METHOD_STORE : entry.method);
    write_u16(out, 0);
    write_u16(out, 0);
    write_u32(out, entry.crc);
    write_u32(out, static_cast<std::uint32_t>(entry.compressed.size()));
    write_u32(out, static_cast<std::uint32_t>(entry.data.size()));
    write_u16(out, static_cast<std::uint16_t>(entry.name.size()));
    write_u16(out, 0);
    write_u16(out, 0);
    write_u16(out, 0);
    write_u16(out, 0);
    write_u32(out, entry.directory ? 0x10u : 0u);
    write_u32(out, entry.local_offset);
    out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
}

} // namespace

#ifdef ZIPTOOL_ENABLE_TEST_HOOKS
void set_extraction_test_hook(ExtractionTestHook hook) {
    extraction_test_hook = hook;
}
#endif

void create_archive(const std::filesystem::path& output,
                    const std::vector<InputPath>& inputs,
                    const ArchiveOptions& options) {

    std::error_code ec;
    if (std::filesystem::exists(output, ec) && !options.overwrite) {
        throw std::runtime_error(
            "Output archive already exists. Use --yes to overwrite: " +
            output.string());
    }

    std::cout << "Compression started: reading and compressing inputs...\n" << std::flush;
    auto entries = build_entries(inputs);
    ensure_u32(entries.size(), "Entry count");

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error(
        "Cannot create archive: " + output.string());

    std::size_t file_count = 0;
    std::uint64_t total_input = 0;

    std::size_t processed_entries = 0;
    for (auto& entry : entries) {
        total_input += entry.data.size();
        ensure_u32(entry.data.size(), "File size");
        ensure_u32(entry.compressed.size(), "Compressed size");

        write_local_entry(out, entry);
        ++processed_entries;
        std::cout << "  Writing archive entries: " << (100 * processed_entries / entries.size()) << "%\n";

        if (!entry.directory) {
            ++file_count;
            const auto analysis = huffman::analyse(entry.data);
            std::cout << "  " << entry.name << "  ["
                      << entry.data.size() << " bytes, "
                      << analysis.unique_symbols
                      << " symbols, avg Huffman code "
                      << analysis.average_code_length << " bits]\n";
        }
    }

    const auto central_offset64 =
        static_cast<std::uint64_t>(out.tellp());
    ensure_u32(central_offset64, "Central directory offset");
    const auto central_offset =
        static_cast<std::uint32_t>(central_offset64);

    for (const auto& entry : entries) {
        write_central_entry(out, entry);
    }

    const auto central_end =
        static_cast<std::uint64_t>(out.tellp());
    ensure_u32(central_end - central_offset64,
               "Central directory size");
    const auto central_size =
        static_cast<std::uint32_t>(central_end - central_offset64);

    write_u32(out, END_SIG);
    write_u16(out, 0);
    write_u16(out, 0);
    write_u16(out, static_cast<std::uint16_t>(entries.size()));
    write_u16(out, static_cast<std::uint16_t>(entries.size()));
    write_u32(out, central_size);
    write_u32(out, central_offset);
    write_u16(out, 0);

    out.flush();
    if (!out) throw std::runtime_error(
        "Failed while writing archive.");

    std::cout << "Created: " << output << "\n"
              << "Files: " << file_count
              << ", uncompressed bytes: " << total_input << "\n";
}

void extract_archive(const std::filesystem::path& archive,
                     const std::filesystem::path& destination,
                     const ExtractionOptions& options) {

    std::ifstream in(archive, std::ios::binary);
    if (!in) throw std::runtime_error(
        "Cannot open archive: " + archive.string());

    const auto records = read_central_directory(in);

    struct PlannedEntry {
        const CentralRecord* record;
        std::filesystem::path relative;
    };

    std::vector<PlannedEntry> planned;
    planned.reserve(records.size());
    std::map<std::string, bool> entry_types;

    for (const auto& record : records) {
        auto relative = safe_entry_path(record.name, record.directory);
        const auto key = relative.generic_string();
        if (!entry_types.emplace(key, record.directory).second) {
            throw std::runtime_error("Duplicate archive path rejected: " + record.name);
        }
        planned.push_back({&record, std::move(relative)});
    }

    for (const auto& item : planned) {
        const auto key = item.relative.generic_string();
        auto parent = item.relative.parent_path();
        while (!parent.empty()) {
            const auto found = entry_types.find(parent.generic_string());
            if (found != entry_types.end() && !found->second) {
                throw std::runtime_error(
                    "Archive file conflicts with a directory path: " + parent.string());
            }
            parent = parent.parent_path();
        }

        if (!item.record->directory) {
            const auto prefix = key + "/";
            const auto child = entry_types.lower_bound(prefix);
            if (child != entry_types.end() &&
                child->first.compare(0, prefix.size(), prefix) == 0) {
                throw std::runtime_error(
                    "Archive file conflicts with a nested entry: " + key);
            }
        }
    }

    for (const auto& item : planned) {
        if (!item.record->directory) {
            (void)read_entry_data(in, *item.record);
        }
    }

    std::error_code ec;
    const auto safe_root =
        std::filesystem::absolute(destination, ec).lexically_normal();
    if (ec) throw std::runtime_error(
        "Invalid extraction destination: " + destination.string());

#ifdef _WIN32
    auto root_handles = open_windows_directories(
        safe_root.root_path(), safe_root.relative_path(), true);
#else
    auto root_handles = open_posix_directories(
        "/", safe_root.relative_path(), true);
#endif

    // Check all existing paths up front so a conflict cannot leave a partial extraction.
    for (const auto& item : planned) {
        if (item.record->directory) {
            check_safe_directory(safe_root, item.relative, false);
        } else {
            check_safe_directory(safe_root, item.relative.parent_path(), false);
            check_safe_file_target(safe_root, item.relative, options.overwrite);
        }
    }

    const auto total_files = std::count_if(
        records.begin(), records.end(),
        [](const auto& record) { return !record.directory; });
    struct StagedFile {
        std::size_t entry_index;
        std::filesystem::path temporary_name;
        std::filesystem::path backup_name;
        bool published = false;
    };
    std::vector<StagedFile> staged;
    staged.reserve(static_cast<std::size_t>(total_files));

    try {
        for (std::size_t index = 0; index < planned.size(); ++index) {
            const auto& item = planned[index];
            const auto& record = *item.record;
            const auto target = (safe_root / item.relative).lexically_normal();
            if (!starts_with_path(target, safe_root)) {
                throw std::runtime_error(
                    "Unsafe extraction path rejected: " + record.name);
            }

            if (record.directory) {
#ifdef _WIN32
                auto directory_handles = open_windows_directories(
                    safe_root.root_path(),
                    safe_root.relative_path() / item.relative, true);
#else
                auto directory_handles = open_posix_relative_directories(
                    root_handles.back().get(), item.relative, true);
#endif
                (void)directory_handles;
                continue;
            }

            auto data = read_entry_data(in, record);
#ifdef ZIPTOOL_ENABLE_TEST_HOOKS
            if (extraction_test_hook) {
                extraction_test_hook(safe_root, item.relative);
            }
#endif
#ifdef _WIN32
            auto temporary_name =
                stage_file_no_follow(safe_root, item.relative, data);
#else
            auto temporary_name = stage_file_no_follow(
                root_handles.back().get(), item.relative, data);
#endif
            staged.push_back({index, std::move(temporary_name), {}, false});
        }

        for (auto& file : staged) {
            const auto& item = planned[file.entry_index];
#ifdef _WIN32
            file.backup_name = backup_existing_file(
                safe_root, item.relative, options.overwrite);
            publish_file_no_follow(
                safe_root, item.relative, file.temporary_name,
                file.published);
#else
            file.backup_name = backup_existing_file(
                root_handles.back().get(), item.relative, options.overwrite);
            publish_file_no_follow(
                root_handles.back().get(), item.relative,
                file.temporary_name, file.published);
#endif
        }
    } catch (...) {
        for (auto file = staged.rbegin(); file != staged.rend(); ++file) {
            const auto& item = planned[file->entry_index];
            if (file->published) {
#ifdef _WIN32
                const bool removed = remove_published_file(
                    safe_root, item.relative);
#else
                const bool removed = remove_published_file(
                    root_handles.back().get(), item.relative);
#endif
                if (!removed) {
                    std::cerr << "Warning: could not roll back extracted file "
                              << item.record->name << "\n";
                }
            }
            if (!file->backup_name.empty()) {
#ifdef _WIN32
                const bool restored = restore_backup_file(
                    safe_root, item.relative, file->backup_name);
#else
                const bool restored = restore_backup_file(
                    root_handles.back().get(), item.relative,
                    file->backup_name);
#endif
                if (!restored) {
                    std::cerr << "Warning: could not restore previous extraction target for "
                              << item.record->name << "\n";
                }
            }
#ifdef _WIN32
            const bool removed = remove_staged_file(
                safe_root, item.relative, file->temporary_name);
#else
            const bool removed = remove_staged_file(
                root_handles.back().get(), item.relative,
                file->temporary_name);
#endif
            if (!removed) {
                std::cerr << "Warning: could not remove staged extraction file for "
                          << item.record->name << "\n";
            }
        }
        throw;
    }

    for (const auto& file : staged) {
        if (file.backup_name.empty()) continue;
        const auto& item = planned[file.entry_index];
#ifdef _WIN32
        const bool removed = discard_backup_file(
            safe_root, item.relative, file.backup_name);
#else
        const bool removed = discard_backup_file(
            root_handles.back().get(), item.relative, file.backup_name);
#endif
        if (!removed) {
            std::cerr << "Warning: could not remove extraction backup for "
                      << item.record->name << "\n";
        }
    }

    std::size_t extracted = 0;
    for (const auto& file : staged) {
        const auto& item = planned[file.entry_index];
        const auto& record = *item.record;
        ++extracted;
        const double progress =
            total_files == 0 ? 100.0 :
            100.0 * static_cast<double>(extracted) /
            static_cast<double>(total_files);

        std::cout << "  Extracted " << record.name << " ("
                  << static_cast<int>(progress) << "%)\n";
    }

    std::cout << "Extraction completed: " << extracted
              << " files -> " << destination << "\n";
}

std::vector<std::string> list_archive(
    const std::filesystem::path& archive) {

    std::ifstream in(archive, std::ios::binary);
    if (!in) throw std::runtime_error(
        "Cannot open archive: " + archive.string());

    const auto records = read_central_directory(in);
    std::vector<std::string> result;
    result.reserve(records.size());

    for (const auto& record : records) {
        result.push_back(record.name);
    }
    return result;
}

} // namespace
