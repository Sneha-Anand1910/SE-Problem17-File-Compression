#include "zip_archive.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t MiB = 1024u * 1024u;

void generate_dataset(const fs::path& path, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create benchmark input: " + path.string());

    std::vector<std::uint8_t> buffer(1u << 20);
    std::uint32_t state = 0x13579BDFu;

    std::size_t remaining = bytes;
    while (remaining > 0) {
        const auto chunk = std::min<std::size_t>(remaining, buffer.size());
        for (std::size_t i = 0; i < chunk; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            buffer[i] = static_cast<std::uint8_t>(state >> 24);
        }
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk));
        if (!out) throw std::runtime_error("Failed while generating benchmark input.");
        remaining -= chunk;
    }
}

bool files_equal(const fs::path& a, const fs::path& b) {
    std::ifstream left(a, std::ios::binary);
    std::ifstream right(b, std::ios::binary);
    if (!left || !right || fs::file_size(a) != fs::file_size(b)) return false;

    std::vector<char> lhs(1u << 20);
    std::vector<char> rhs(1u << 20);

    while (left && right) {
        left.read(lhs.data(), static_cast<std::streamsize>(lhs.size()));
        right.read(rhs.data(), static_cast<std::streamsize>(rhs.size()));
        const auto ln = left.gcount();
        const auto rn = right.gcount();
        if (ln != rn) return false;
        if (!std::equal(lhs.begin(), lhs.begin() + ln, rhs.begin())) return false;
        if (ln == 0) break;
    }
    return true;
}

void run_case(const fs::path& root, std::size_t input_bytes) {
    const auto label = std::to_string(input_bytes / MiB) + " MiB";
    const auto input = root / ("input_" + std::to_string(input_bytes) + ".bin");
    const auto archive = root / ("archive_" + std::to_string(input_bytes) + ".zip");
    const auto output = root / ("output_" + std::to_string(input_bytes));

    generate_dataset(input, input_bytes);

    const auto compress_start = Clock::now();
    ziptool::create_archive(archive, {{input}}, {true});
    const auto compress_end = Clock::now();

    const auto extract_start = Clock::now();
    ziptool::extract_archive(archive, output, {true});
    const auto extract_end = Clock::now();

    const auto compress_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            compress_end - compress_start).count();
    const auto extract_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            extract_end - extract_start).count();

    const auto archive_bytes = fs::file_size(archive);
    const double ratio =
        static_cast<double>(archive_bytes) / static_cast<double>(input_bytes);

    if (!files_equal(input, output / input.filename())) {
        throw std::runtime_error("Round-trip integrity check failed for " + label);
    }

    std::cout << "PERF " << label
              << " | compress_ms=" << compress_ms
              << " | extract_ms=" << extract_ms
              << " | input_bytes=" << input_bytes
              << " | archive_bytes=" << archive_bytes
              << " | archive/input=" << ratio
              << " | integrity=PASS\n";
}

} // namespace

int main() {
    const fs::path root = fs::current_path() / "performance_workspace";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) {
        std::cerr << "Cannot create performance workspace.\n";
        return 1;
    }

    try {
        run_case(root, 1u * MiB);
        run_case(root, 10u * MiB);
        run_case(root, 100u * MiB);
    } catch (const std::exception& ex) {
        std::cerr << "Performance test failed: " << ex.what() << '\n';
        fs::remove_all(root, ec);
        return 1;
    }

    fs::remove_all(root, ec);
    std::cout << "Performance benchmark completed successfully.\n";
    return 0;
}
