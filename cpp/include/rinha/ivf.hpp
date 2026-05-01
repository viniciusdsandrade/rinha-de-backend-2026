#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rinha/types.hpp"

namespace rinha {

struct IvfBuildOptions {
    std::uint32_t clusters = 256;
    std::uint32_t train_sample = 65'536;
    std::uint32_t iterations = 6;
    std::size_t max_references = 0;
};

struct IvfSearchConfig {
    std::uint32_t fast_nprobe = 1;
    std::uint32_t full_nprobe = 1;
    bool boundary_full = false;
    bool bbox_repair = true;
    std::uint8_t repair_min_frauds = 2;
    std::uint8_t repair_max_frauds = 3;
};

class IvfIndex {
public:
    static bool build_from_gzip_json(
        const std::string& path,
        const IvfBuildOptions& options,
        IvfIndex& index,
        std::string& error
    );

    bool load_binary(const std::string& path, std::string& error);
    bool write_binary(const std::string& path, std::string& error) const;

    [[nodiscard]] std::uint8_t fraud_count(
        const QueryVector& query,
        const IvfSearchConfig& config = {}
    ) const noexcept;

    [[nodiscard]] std::size_t len() const noexcept;
    [[nodiscard]] std::uint32_t clusters() const noexcept;
    [[nodiscard]] std::size_t padded_len() const noexcept;
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

private:
    std::uint32_t n_ = 0;
    std::uint32_t clusters_ = 0;
    std::uint32_t total_blocks_ = 0;
    std::vector<float> centroids_{};
    std::vector<std::int16_t> bbox_min_{};
    std::vector<std::int16_t> bbox_max_{};
    std::vector<std::uint32_t> offsets_{};
    std::vector<std::uint8_t> labels_{};
    std::vector<std::uint32_t> orig_ids_{};
    std::vector<std::int16_t> blocks_{};

    std::uint8_t fraud_count_once(
        const std::array<std::int16_t, kDimensions>& query_i16,
        const QueryVector& query_float,
        std::uint32_t nprobe,
        bool bbox_repair
    ) const noexcept;

    template <std::size_t MaxNprobe>
    std::uint8_t fraud_count_once_fixed(
        const std::array<std::int16_t, kDimensions>& query_i16,
        const QueryVector& query_float,
        std::uint32_t nprobe,
        bool bbox_repair
    ) const noexcept;
};

}  // namespace rinha
