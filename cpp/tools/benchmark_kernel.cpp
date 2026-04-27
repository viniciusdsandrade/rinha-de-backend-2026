#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <immintrin.h>

#include "rinha/refs.hpp"
#include "rinha/types.hpp"
#include "simdjson.h"

namespace {

using rinha::QueryVector;
using rinha::ReferenceGroup;
using rinha::ReferenceSet;

constexpr std::size_t kMaxReferenceGroups = 512;

struct Top5 {
    std::array<std::pair<float, bool>, 5> entries{};

    Top5() {
        entries.fill({std::numeric_limits<float>::infinity(), false});
    }

    void insert(float distance, bool is_fraud) noexcept {
        if (distance >= entries[4].first) {
            return;
        }

        std::size_t position = 4;
        while (position > 0 && entries[position - 1].first > distance) {
            entries[position] = entries[position - 1];
            --position;
        }
        entries[position] = {distance, is_fraud};
    }

    [[nodiscard]] std::size_t fraud_count() const noexcept {
        std::size_t count = 0;
        for (const auto& entry : entries) {
            count += entry.second ? 1U : 0U;
        }
        return count;
    }
};

struct ScanStats {
    std::uint64_t rows_scanned = 0;
    std::uint64_t groups_visited = 0;
};

struct PreparedGroup {
    std::array<const float*, rinha::kDimensions> dims{};
    const std::uint8_t* labels = nullptr;
    std::size_t rows = 0;
    const std::array<float, rinha::kDimensions>* min_values = nullptr;
    const std::array<float, rinha::kDimensions>* max_values = nullptr;
    const std::array<std::size_t, rinha::kDimensions>* dimension_order = nullptr;
};

struct PreparedRefs {
    std::vector<PreparedGroup> groups;
};

bool supports_avx2() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return true;
#endif
#else
    return false;
#endif
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("falha ao abrir " + path);
    }
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    file.seekg(0, std::ios::beg);
    if (length < 0) {
        throw std::runtime_error("falha ao medir " + path);
    }

    std::string contents(static_cast<std::size_t>(length), '\0');
    if (!contents.empty()) {
        file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) {
            throw std::runtime_error("falha ao ler " + path);
        }
    }
    return contents;
}

std::vector<QueryVector> load_queries(const std::string& path, std::size_t limit) {
    const std::string contents = read_text_file(path);
    simdjson::dom::parser parser;
    simdjson::padded_string json(contents);
    simdjson::dom::element root;
    if (parser.parse(json).get(root) != simdjson::SUCCESS) {
        throw std::runtime_error("falha ao decodificar test-data");
    }

    simdjson::dom::array entries;
    if (root["entries"].get(entries) != simdjson::SUCCESS) {
        throw std::runtime_error("test-data sem entries");
    }

    std::vector<QueryVector> queries;
    for (const auto entry : entries) {
        simdjson::dom::array vector_values;
        if (entry["info"]["vector"].get(vector_values) != simdjson::SUCCESS) {
            throw std::runtime_error("entrada sem info.vector");
        }

        QueryVector query{};
        std::size_t dim = 0;
        for (const auto value : vector_values) {
            double number = 0.0;
            if (value.get(number) != simdjson::SUCCESS || dim >= rinha::kDimensions) {
                throw std::runtime_error("info.vector inválido");
            }
            query[dim++] = static_cast<float>(number);
        }
        if (dim != rinha::kDimensions) {
            throw std::runtime_error("info.vector sem 14 dimensões");
        }

        queries.push_back(query);
        if (limit != 0 && queries.size() >= limit) {
            break;
        }
    }

    return queries;
}

PreparedRefs prepare_refs(const ReferenceSet& refs) {
    PreparedRefs prepared;
    prepared.groups.reserve(refs.groups().size());
    for (const ReferenceGroup& group : refs.groups()) {
        PreparedGroup prepared_group;
        for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
            prepared_group.dims[dim] = group.dims[dim].data();
        }
        prepared_group.labels = group.labels.data();
        prepared_group.rows = group.labels.size();
        prepared_group.min_values = &group.min_values;
        prepared_group.max_values = &group.max_values;
        prepared_group.dimension_order = &group.dimension_order;
        prepared.groups.push_back(prepared_group);
    }
    return prepared;
}

float lower_bound_distance(const ReferenceGroup& group, const QueryVector& query) noexcept {
    float sum = 0.0f;
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        float delta = 0.0f;
        if (query[dim] < group.min_values[dim]) {
            delta = group.min_values[dim] - query[dim];
        } else if (query[dim] > group.max_values[dim]) {
            delta = query[dim] - group.max_values[dim];
        }
        sum += delta * delta;
    }
    return sum;
}

float lower_bound_distance(const PreparedGroup& group, const QueryVector& query) noexcept {
    float sum = 0.0f;
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        float delta = 0.0f;
        if (query[dim] < (*group.min_values)[dim]) {
            delta = (*group.min_values)[dim] - query[dim];
        } else if (query[dim] > (*group.max_values)[dim]) {
            delta = query[dim] - (*group.max_values)[dim];
        }
        sum += delta * delta;
    }
    return sum;
}

void scan_tail(
    Top5& top,
    const std::array<const float*, rinha::kDimensions>& dims,
    const std::uint8_t* labels,
    const std::array<std::size_t, rinha::kDimensions>& ordered_dims,
    const QueryVector& query,
    std::size_t begin,
    std::size_t end,
    ScanStats& stats
) noexcept {
    for (std::size_t row = begin; row < end; ++row) {
        float distance = 0.0f;
        bool below = true;
        ++stats.rows_scanned;
        for (const std::size_t dim : ordered_dims) {
            const float delta = query[dim] - dims[dim][row];
            distance += delta * delta;
            if (distance >= top.entries[4].first) {
                below = false;
                break;
            }
        }
        if (below) {
            top.insert(distance, labels[row] != 0);
        }
    }
}

void scan_group_baseline(
    Top5& top,
    const ReferenceGroup& group,
    const QueryVector& query,
    const __m256* query_lanes,
    ScanStats& stats
) noexcept {
    ++stats.groups_visited;

    std::array<const float*, rinha::kDimensions> dims{};
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        dims[dim] = group.dims[dim].data();
    }

    const auto& ordered_dims = group.dimension_order;
    const std::size_t rows = group.labels.size();
    const std::size_t chunks = rows / 8U;

    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        const std::size_t offset = chunk * 8U;
        const float threshold = top.entries[4].first;
        const __m256 threshold_lanes = _mm256_set1_ps(threshold);
        __m256 accum = _mm256_setzero_ps();
        bool pruned = false;
        stats.rows_scanned += 8U;

        for (const std::size_t dim : ordered_dims) {
            const __m256 refs_lane = _mm256_loadu_ps(dims[dim] + offset);
            const __m256 diff = _mm256_sub_ps(query_lanes[dim], refs_lane);
            accum = _mm256_fmadd_ps(diff, diff, accum);

            if (std::isfinite(threshold)) {
                const __m256 cmp = _mm256_cmp_ps(accum, threshold_lanes, _CMP_GE_OQ);
                if (_mm256_movemask_ps(cmp) == 0xFF) {
                    pruned = true;
                    break;
                }
            }
        }

        if (pruned) {
            continue;
        }

        alignas(32) std::array<float, 8> distances{};
        _mm256_store_ps(distances.data(), accum);
        for (std::size_t lane = 0; lane < 8U; ++lane) {
            top.insert(distances[lane], group.labels[offset + lane] != 0);
        }
    }

    scan_tail(top, dims, group.labels.data(), ordered_dims, query, chunks * 8U, rows, stats);
}

void scan_group_cached(
    Top5& top,
    const PreparedGroup& group,
    const QueryVector& query,
    const __m256* query_lanes,
    ScanStats& stats,
    bool finite_once_per_chunk
) noexcept {
    ++stats.groups_visited;

    const auto& ordered_dims = *group.dimension_order;
    const std::size_t chunks = group.rows / 8U;

    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        const std::size_t offset = chunk * 8U;
        const float threshold = top.entries[4].first;
        const bool threshold_is_finite = std::isfinite(threshold);
        const __m256 threshold_lanes = _mm256_set1_ps(threshold);
        __m256 accum = _mm256_setzero_ps();
        bool pruned = false;
        stats.rows_scanned += 8U;

        for (const std::size_t dim : ordered_dims) {
            const __m256 refs_lane = _mm256_loadu_ps(group.dims[dim] + offset);
            const __m256 diff = _mm256_sub_ps(query_lanes[dim], refs_lane);
            accum = _mm256_fmadd_ps(diff, diff, accum);

            if (finite_once_per_chunk ? threshold_is_finite : std::isfinite(threshold)) {
                const __m256 cmp = _mm256_cmp_ps(accum, threshold_lanes, _CMP_GE_OQ);
                if (_mm256_movemask_ps(cmp) == 0xFF) {
                    pruned = true;
                    break;
                }
            }
        }

        if (pruned) {
            continue;
        }

        alignas(32) std::array<float, 8> distances{};
        _mm256_store_ps(distances.data(), accum);
        for (std::size_t lane = 0; lane < 8U; ++lane) {
            top.insert(distances[lane], group.labels[offset + lane] != 0);
        }
    }

    scan_tail(top, group.dims, group.labels, ordered_dims, query, chunks * 8U, group.rows, stats);
}

Top5 top5_baseline(const ReferenceSet& refs, const QueryVector& query, ScanStats& stats) {
    Top5 top;

    __m256 query_lanes[rinha::kDimensions];
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        query_lanes[dim] = _mm256_set1_ps(query[dim]);
    }

    const auto& groups = refs.groups();
    std::array<std::pair<float, std::size_t>, kMaxReferenceGroups> group_order{};
    for (std::size_t index = 0; index < groups.size(); ++index) {
        group_order[index] = {lower_bound_distance(groups[index], query), index};
    }
    std::sort(group_order.begin(), group_order.begin() + static_cast<std::ptrdiff_t>(groups.size()));

    for (std::size_t position = 0; position < groups.size(); ++position) {
        const auto [lower_bound, group_index] = group_order[position];
        if (lower_bound >= top.entries[4].first) {
            break;
        }
        scan_group_baseline(top, groups[group_index], query, query_lanes, stats);
    }

    return top;
}

Top5 top5_baseline_select_min(const ReferenceSet& refs, const QueryVector& query, ScanStats& stats) {
    Top5 top;

    __m256 query_lanes[rinha::kDimensions];
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        query_lanes[dim] = _mm256_set1_ps(query[dim]);
    }

    const auto& groups = refs.groups();
    std::array<float, kMaxReferenceGroups> bounds{};
    for (std::size_t index = 0; index < groups.size(); ++index) {
        bounds[index] = lower_bound_distance(groups[index], query);
    }

    for (;;) {
        float best = std::numeric_limits<float>::infinity();
        std::size_t group_index = groups.size();
        for (std::size_t index = 0; index < groups.size(); ++index) {
            if (bounds[index] < best) {
                best = bounds[index];
                group_index = index;
            }
        }
        if (group_index == groups.size() || best >= top.entries[4].first) {
            break;
        }

        bounds[group_index] = std::numeric_limits<float>::infinity();
        scan_group_baseline(top, groups[group_index], query, query_lanes, stats);
    }

    return top;
}

Top5 top5_cached_sort(
    const PreparedRefs& refs,
    const QueryVector& query,
    ScanStats& stats,
    bool finite_once_per_chunk
) {
    Top5 top;

    __m256 query_lanes[rinha::kDimensions];
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        query_lanes[dim] = _mm256_set1_ps(query[dim]);
    }

    std::array<std::pair<float, std::size_t>, kMaxReferenceGroups> group_order{};
    for (std::size_t index = 0; index < refs.groups.size(); ++index) {
        group_order[index] = {lower_bound_distance(refs.groups[index], query), index};
    }
    std::sort(group_order.begin(), group_order.begin() + static_cast<std::ptrdiff_t>(refs.groups.size()));

    for (std::size_t position = 0; position < refs.groups.size(); ++position) {
        const auto [lower_bound, group_index] = group_order[position];
        if (lower_bound >= top.entries[4].first) {
            break;
        }
        scan_group_cached(top, refs.groups[group_index], query, query_lanes, stats, finite_once_per_chunk);
    }

    return top;
}

Top5 top5_cached_select_min(const PreparedRefs& refs, const QueryVector& query, ScanStats& stats) {
    Top5 top;

    __m256 query_lanes[rinha::kDimensions];
    for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
        query_lanes[dim] = _mm256_set1_ps(query[dim]);
    }

    std::array<float, kMaxReferenceGroups> bounds{};
    for (std::size_t index = 0; index < refs.groups.size(); ++index) {
        bounds[index] = lower_bound_distance(refs.groups[index], query);
    }

    for (;;) {
        float best = std::numeric_limits<float>::infinity();
        std::size_t group_index = refs.groups.size();
        for (std::size_t index = 0; index < refs.groups.size(); ++index) {
            if (bounds[index] < best) {
                best = bounds[index];
                group_index = index;
            }
        }
        if (group_index == refs.groups.size() || best >= top.entries[4].first) {
            break;
        }

        bounds[group_index] = std::numeric_limits<float>::infinity();
        scan_group_cached(top, refs.groups[group_index], query, query_lanes, stats, true);
    }

    return top;
}

template <typename Fn>
std::pair<std::chrono::nanoseconds, std::uint64_t> time_queries(
    const std::vector<QueryVector>& queries,
    std::size_t repeat,
    Fn&& fn
) {
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < repeat; ++iteration) {
        for (const QueryVector& query : queries) {
            checksum += fn(query).fraud_count();
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(end - start), checksum};
}

double ns_per_query(std::chrono::nanoseconds elapsed, std::size_t query_count, std::size_t repeat) {
    const auto total = static_cast<double>(query_count * repeat);
    return static_cast<double>(elapsed.count()) / total;
}

template <typename Fn>
void run_variant(
    const char* name,
    const std::vector<QueryVector>& queries,
    const std::vector<std::size_t>& expected_counts,
    std::size_t repeat,
    Fn&& fn
) {
    std::size_t fraud_count_mismatches = 0;
    std::size_t decision_errors = 0;
    ScanStats validation_stats;
    for (std::size_t index = 0; index < queries.size(); ++index) {
        const Top5 top = fn(queries[index], validation_stats);
        const std::size_t fraud_count = top.fraud_count();
        fraud_count_mismatches += fraud_count != expected_counts[index] ? 1U : 0U;
        decision_errors += (fraud_count < 3U) != (expected_counts[index] < 3U) ? 1U : 0U;
    }

    ScanStats stats;
    const auto [elapsed, checksum] = time_queries(
        queries,
        repeat,
        [&fn, &stats](const QueryVector& query) {
            return fn(query, stats);
        }
    );

    std::cout << "variant=" << name
              << " fraud_count_mismatches=" << fraud_count_mismatches
              << " decision_errors=" << decision_errors
              << " ns_per_query=" << ns_per_query(elapsed, queries.size(), repeat)
              << " rows_per_query=" << static_cast<double>(stats.rows_scanned) /
                     static_cast<double>(queries.size() * repeat)
              << " groups_per_query=" << static_cast<double>(stats.groups_visited) /
                     static_cast<double>(queries.size() * repeat)
              << " validation_rows_per_query=" << static_cast<double>(validation_stats.rows_scanned) /
                     static_cast<double>(queries.size())
              << " checksum=" << checksum
              << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (!supports_avx2()) {
            std::cerr << "CPU sem AVX2/FMA\n";
            return 1;
        }

        const std::string references_path = argc > 1 ? argv[1] : "resources/references.json.gz";
        const std::string test_data_path = argc > 2 ? argv[2] : "test/test-data.json";
        const std::size_t repeat = argc > 3 ? static_cast<std::size_t>(std::stoull(argv[3])) : 3U;
        const std::size_t limit = argc > 4 ? static_cast<std::size_t>(std::stoull(argv[4])) : 0U;

        ReferenceSet refs;
        std::string error;
        if (!ReferenceSet::load_gzip_json(references_path, refs, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        if (refs.groups().empty() || refs.groups().size() > kMaxReferenceGroups) {
            std::cerr << "quantidade de grupos inválida para benchmark: " << refs.groups().size() << '\n';
            return 1;
        }

        const PreparedRefs prepared = prepare_refs(refs);
        const std::vector<QueryVector> queries = load_queries(test_data_path, limit);

        std::vector<std::size_t> expected_counts;
        expected_counts.reserve(queries.size());
        ScanStats expected_stats;
        for (const QueryVector& query : queries) {
            expected_counts.push_back(top5_baseline(refs, query, expected_stats).fraud_count());
        }

        std::cout << "queries=" << queries.size()
                  << " repeat=" << repeat
                  << " refs=" << refs.len()
                  << " groups=" << refs.groups().size()
                  << " expected_rows_per_query=" << static_cast<double>(expected_stats.rows_scanned) /
                         static_cast<double>(queries.size())
                  << " expected_groups_per_query=" << static_cast<double>(expected_stats.groups_visited) /
                         static_cast<double>(queries.size())
                  << '\n';

        run_variant(
            "baseline_production",
            queries,
            expected_counts,
            repeat,
            [&refs](const QueryVector& query, ScanStats& stats) {
                return top5_baseline(refs, query, stats);
            }
        );
        run_variant(
            "baseline_select_min",
            queries,
            expected_counts,
            repeat,
            [&refs](const QueryVector& query, ScanStats& stats) {
                return top5_baseline_select_min(refs, query, stats);
            }
        );
        run_variant(
            "cached_group_ptrs",
            queries,
            expected_counts,
            repeat,
            [&prepared](const QueryVector& query, ScanStats& stats) {
                return top5_cached_sort(prepared, query, stats, false);
            }
        );
        run_variant(
            "cached_group_ptrs_finite_once",
            queries,
            expected_counts,
            repeat,
            [&prepared](const QueryVector& query, ScanStats& stats) {
                return top5_cached_sort(prepared, query, stats, true);
            }
        );
        run_variant(
            "cached_select_min_finite_once",
            queries,
            expected_counts,
            repeat,
            [&prepared](const QueryVector& query, ScanStats& stats) {
                return top5_cached_select_min(prepared, query, stats);
            }
        );
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
