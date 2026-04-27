#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rinha/types.hpp"

namespace rinha {

struct ReferenceGroup {
    std::array<std::vector<float>, kDimensions> dims{};
    std::vector<std::uint8_t> labels{};
    std::array<float, kDimensions> min_values{};
    std::array<float, kDimensions> max_values{};
};

class ReferenceSet {
public:
    static bool load_gzip_json(const std::string& path, ReferenceSet& refs, std::string& error);
    static bool load_binary(
        const std::string& references_path,
        const std::string& labels_path,
        ReferenceSet& refs,
        std::string& error
    );
    bool write_binary(
        const std::string& references_path,
        const std::string& labels_path,
        std::string& error
    ) const;

    [[nodiscard]] std::size_t len() const noexcept;
    [[nodiscard]] bool is_empty() const noexcept;
    [[nodiscard]] bool is_fraud(std::size_t index) const noexcept;
    [[nodiscard]] const std::vector<float>& dim(std::size_t index) const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& labels() const noexcept;
    [[nodiscard]] const std::vector<ReferenceGroup>& groups() const noexcept;
    [[nodiscard]] const std::array<std::size_t, kDimensions>& dimension_order() const noexcept;

    bool distance_squared_if_below(
        const QueryVector& query,
        std::size_t row,
        float limit,
        float& distance
    ) const noexcept;

private:
    std::array<std::vector<float>, kDimensions> dims_{};
    std::vector<std::uint8_t> labels_{};
    std::vector<ReferenceGroup> groups_{};
    std::array<std::size_t, kDimensions> dimension_order_{};

    void finalize_dimension_order() noexcept;
    void build_groups();
};

}  // namespace rinha
