#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rinha/refs.hpp"
#include "rinha/types.hpp"
#include "simdjson.h"

namespace {

using rinha::QueryVector;
using rinha::ReferenceSet;

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

struct Group {
    std::array<std::vector<float>, rinha::kDimensions> dims{};
    std::vector<std::uint8_t> labels{};
    std::array<float, rinha::kDimensions> min_values{};
    std::array<float, rinha::kDimensions> max_values{};
    std::array<std::size_t, rinha::kDimensions> dimension_order{};
};

struct GroupedRefs {
    std::vector<Group> groups;
    std::array<std::size_t, rinha::kDimensions> dimension_order{};
};

struct GroupingStrategy {
    const char* name = "";
    bool include_risk = true;
    std::uint32_t amount_buckets = 0;
    bool include_hour = false;
    bool include_day = false;
};

enum class TraversalMode {
    Sort,
    SelectMin,
    Unsorted,
};

const char* traversal_name(TraversalMode mode) noexcept {
    switch (mode) {
        case TraversalMode::Sort:
            return "sort";
        case TraversalMode::SelectMin:
            return "select_min";
        case TraversalMode::Unsorted:
            return "unsorted";
    }
    return "unknown";
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("falha ao abrir " + path);
    }
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string contents(static_cast<std::size_t>(length), '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) {
        throw std::runtime_error("falha ao ler " + path);
    }
    return contents;
}

std::vector<QueryVector> load_queries(const std::string& path, std::size_t limit) {
    std::string contents = read_text_file(path);
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

std::uint32_t normalized_bucket(float value, std::uint32_t buckets) {
    if (buckets == 0) {
        return 0;
    }
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 1.0f) {
        return buckets - 1U;
    }
    return std::min(static_cast<std::uint32_t>(value * static_cast<float>(buckets)), buckets - 1U);
}

std::uint32_t bits_for_buckets(std::uint32_t buckets) {
    std::uint32_t bits = 0;
    std::uint32_t values = 1;
    while (values < buckets) {
        values <<= 1U;
        ++bits;
    }
    return bits;
}

std::uint32_t group_key(const ReferenceSet& refs, std::size_t row, const GroupingStrategy& strategy) {
    const bool no_last_transaction = refs.dim(5)[row] < -0.5f && refs.dim(6)[row] < -0.5f;
    const auto bool_bit = [&refs, row](std::size_t dim) -> std::uint32_t {
        return refs.dim(dim)[row] > 0.5f ? 1U : 0U;
    };

    std::uint32_t key = (no_last_transaction ? 1U : 0U) |
                        (bool_bit(9) << 1U) |
                        (bool_bit(10) << 2U) |
                        (bool_bit(11) << 3U);
    std::uint32_t shift = 4;

    if (strategy.include_risk) {
        const float risk = refs.dim(12)[row];
        const auto risk_bucket = static_cast<std::uint32_t>(std::lround(risk * 20.0f));
        key |= risk_bucket << shift;
        shift += 5U;
    }

    if (strategy.amount_buckets > 0) {
        key |= normalized_bucket(refs.dim(0)[row], strategy.amount_buckets) << shift;
        shift += bits_for_buckets(strategy.amount_buckets);
    }

    if (strategy.include_hour) {
        key |= normalized_bucket(refs.dim(3)[row], 24U) << shift;
        shift += 5U;
    }

    if (strategy.include_day) {
        key |= normalized_bucket(refs.dim(4)[row], 7U) << shift;
    }

    return key;
}

GroupedRefs build_grouped_refs(const ReferenceSet& refs, const GroupingStrategy& strategy) {
    std::unordered_map<std::uint32_t, std::size_t> group_by_key;
    GroupedRefs grouped;
    grouped.dimension_order = refs.dimension_order();

    for (std::size_t row = 0; row < refs.len(); ++row) {
        const std::uint32_t key = group_key(refs, row, strategy);
        const auto [position, inserted] = group_by_key.emplace(key, grouped.groups.size());
        if (inserted) {
            grouped.groups.emplace_back();
        }
        Group& group = grouped.groups[position->second];
        for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
            group.dims[dim].push_back(refs.dim(dim)[row]);
        }
        group.labels.push_back(refs.is_fraud(row) ? 1U : 0U);
    }

    for (Group& group : grouped.groups) {
        std::array<double, rinha::kDimensions> variances{};
        for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
            const auto [min_it, max_it] = std::minmax_element(group.dims[dim].begin(), group.dims[dim].end());
            group.min_values[dim] = *min_it;
            group.max_values[dim] = *max_it;

            double mean = 0.0;
            for (const float value : group.dims[dim]) {
                mean += static_cast<double>(value);
            }
            mean /= static_cast<double>(group.dims[dim].size());

            double variance = 0.0;
            for (const float value : group.dims[dim]) {
                const double delta = static_cast<double>(value) - mean;
                variance += delta * delta;
            }
            variances[dim] = variance / static_cast<double>(group.dims[dim].size());

            if (group.dims[dim].capacity() > group.dims[dim].size()) {
                group.dims[dim].shrink_to_fit();
            }
        }

        for (std::size_t dim = 0; dim < rinha::kDimensions; ++dim) {
            group.dimension_order[dim] = dim;
        }
        std::sort(
            group.dimension_order.begin(),
            group.dimension_order.end(),
            [&variances](std::size_t left, std::size_t right) {
                return variances[left] > variances[right];
            }
        );

        if (group.labels.capacity() > group.labels.size()) {
            group.labels.shrink_to_fit();
        }
    }

    return grouped;
}

float lower_bound_distance(const Group& group, const QueryVector& query) noexcept {
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

Top5 baseline_top5(const ReferenceSet& refs, const QueryVector& query, ScanStats& stats) {
    Top5 top;
    for (std::size_t row = 0; row < refs.len(); ++row) {
        float distance = 0.0f;
        ++stats.rows_scanned;
        if (refs.distance_squared_if_below(query, row, top.entries[4].first, distance)) {
            top.insert(distance, refs.is_fraud(row));
        }
    }
    return top;
}

void scan_group(
    Top5& top,
    const Group& group,
    const QueryVector& query,
    ScanStats& stats,
    const std::array<std::size_t, rinha::kDimensions>& dimension_order
) {
    ++stats.groups_visited;
    const std::size_t rows = group.labels.size();
    for (std::size_t row = 0; row < rows; ++row) {
        float distance = 0.0f;
        bool below = true;
        ++stats.rows_scanned;
        for (const std::size_t dim : dimension_order) {
            const float delta = query[dim] - group.dims[dim][row];
            distance += delta * delta;
            if (distance >= top.entries[4].first) {
                below = false;
                break;
            }
        }
        if (below) {
            top.insert(distance, group.labels[row] != 0);
        }
    }
}

Top5 grouped_top5(
    const GroupedRefs& refs,
    const QueryVector& query,
    ScanStats& stats,
    bool use_group_order,
    TraversalMode traversal
) {
    Top5 top;
    const auto dimensions_for = [&refs, use_group_order](const Group& group)
        -> const std::array<std::size_t, rinha::kDimensions>& {
        return use_group_order ? group.dimension_order : refs.dimension_order;
    };

    if (traversal == TraversalMode::Unsorted) {
        for (const Group& group : refs.groups) {
            const float lower_bound = lower_bound_distance(group, query);
            if (lower_bound < top.entries[4].first) {
                scan_group(top, group, query, stats, dimensions_for(group));
            }
        }
        return top;
    }

    if (traversal == TraversalMode::SelectMin) {
        std::vector<float> bounds;
        bounds.reserve(refs.groups.size());
        for (const Group& group : refs.groups) {
            bounds.push_back(lower_bound_distance(group, query));
        }

        for (;;) {
            float best = std::numeric_limits<float>::infinity();
            std::size_t group_index = refs.groups.size();
            for (std::size_t index = 0; index < bounds.size(); ++index) {
                if (bounds[index] < best) {
                    best = bounds[index];
                    group_index = index;
                }
            }
            if (group_index == refs.groups.size() || best >= top.entries[4].first) {
                break;
            }

            bounds[group_index] = std::numeric_limits<float>::infinity();
            const Group& group = refs.groups[group_index];
            scan_group(top, group, query, stats, dimensions_for(group));
        }
        return top;
    }

    std::vector<std::pair<float, std::size_t>> order;
    order.reserve(refs.groups.size());
    for (std::size_t index = 0; index < refs.groups.size(); ++index) {
        order.emplace_back(lower_bound_distance(refs.groups[index], query), index);
    }
    std::sort(order.begin(), order.end());

    for (const auto& [lower_bound, group_index] : order) {
        if (lower_bound >= top.entries[4].first) {
            break;
        }

        const Group& group = refs.groups[group_index];
        scan_group(top, group, query, stats, dimensions_for(group));
    }
    return top;
}

Top5 grouped_budget_top5(
    const GroupedRefs& refs,
    const QueryVector& query,
    ScanStats& stats,
    bool use_group_order,
    std::size_t group_budget
) {
    Top5 top;
    const auto dimensions_for = [&refs, use_group_order](const Group& group)
        -> const std::array<std::size_t, rinha::kDimensions>& {
        return use_group_order ? group.dimension_order : refs.dimension_order;
    };

    std::vector<std::pair<float, std::size_t>> order;
    order.reserve(refs.groups.size());
    for (std::size_t index = 0; index < refs.groups.size(); ++index) {
        order.emplace_back(lower_bound_distance(refs.groups[index], query), index);
    }
    std::sort(order.begin(), order.end());

    const std::size_t limit = std::min(group_budget, order.size());
    for (std::size_t position = 0; position < limit; ++position) {
        const auto [lower_bound, group_index] = order[position];
        if (lower_bound >= top.entries[4].first) {
            break;
        }

        const Group& group = refs.groups[group_index];
        scan_group(top, group, query, stats, dimensions_for(group));
    }
    return top;
}

template <typename Fn>
std::pair<std::chrono::nanoseconds, std::uint64_t> time_queries(
    const std::vector<QueryVector>& queries,
    std::size_t repeat,
    Fn&& fn
) {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t checksum = 0;
    for (std::size_t iteration = 0; iteration < repeat; ++iteration) {
        for (const QueryVector& query : queries) {
            const Top5 top = fn(query);
            checksum += top.fraud_count();
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(end - start), checksum};
}

double ns_per_query(std::chrono::nanoseconds elapsed, std::size_t query_count, std::size_t repeat) {
    const auto total = static_cast<double>(query_count * repeat);
    return static_cast<double>(elapsed.count()) / total;
}

std::vector<GroupingStrategy> benchmark_strategies() {
    return {
        {"base", true, 0, false, false},
        {"no_risk", false, 0, false, false},
        {"amount4", true, 4, false, false},
        {"amount8", true, 8, false, false},
        {"hour", true, 0, true, false},
        {"amount4_hour", true, 4, true, false},
        {"amount4_hour_day", true, 4, true, true},
    };
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string references_path = argc > 1 ? argv[1] : "resources/references.json.gz";
        const std::string test_data_path = argc > 2 ? argv[2] : "test/test-data.json";
        const std::size_t repeat = argc > 3 ? static_cast<std::size_t>(std::stoull(argv[3])) : 1U;
        const std::size_t limit = argc > 4 ? static_cast<std::size_t>(std::stoull(argv[4])) : 0U;
        const bool sweep = argc > 5 && std::string(argv[5]) == "sweep";
        const bool traversal = argc > 5 && std::string(argv[5]) == "traversal";
        const bool budget = argc > 5 && std::string(argv[5]) == "budget";

        ReferenceSet refs;
        std::string error;
        if (!ReferenceSet::load_gzip_json(references_path, refs, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const std::vector<QueryVector> queries = load_queries(test_data_path, limit);

        if (sweep) {
            ScanStats baseline_stats;
            const auto [baseline_elapsed, baseline_checksum] = time_queries(
                queries,
                repeat,
                [&refs, &baseline_stats](const QueryVector& query) {
                    return baseline_top5(refs, query, baseline_stats);
                }
            );
            std::cout << "queries=" << queries.size()
                      << " repeat=" << repeat
                      << " refs=" << refs.len()
                      << " baseline_ns_per_query=" << ns_per_query(baseline_elapsed, queries.size(), repeat)
                      << " rows_per_query=" << static_cast<double>(baseline_stats.rows_scanned) /
                             static_cast<double>(queries.size() * repeat)
                      << " checksum=" << baseline_checksum
                      << '\n';

            for (const GroupingStrategy& strategy : benchmark_strategies()) {
                const GroupedRefs grouped = build_grouped_refs(refs, strategy);
                std::size_t mismatches = 0;
                ScanStats validation_baseline_stats;
                ScanStats validation_grouped_stats;
                for (const QueryVector& query : queries) {
                    const Top5 baseline = baseline_top5(refs, query, validation_baseline_stats);
                    const Top5 grouped_top = grouped_top5(
                        grouped,
                        query,
                        validation_grouped_stats,
                        false,
                        TraversalMode::Sort
                    );
                    if (baseline.fraud_count() != grouped_top.fraud_count()) {
                        ++mismatches;
                    }
                }

                std::size_t local_mismatches = 0;
                ScanStats local_validation_stats;
                for (const QueryVector& query : queries) {
                    const Top5 baseline = baseline_top5(refs, query, validation_baseline_stats);
                    const Top5 grouped_top = grouped_top5(
                        grouped,
                        query,
                        local_validation_stats,
                        true,
                        TraversalMode::Sort
                    );
                    if (baseline.fraud_count() != grouped_top.fraud_count()) {
                        ++local_mismatches;
                    }
                }

                ScanStats grouped_stats;
                const auto [grouped_elapsed, grouped_checksum] = time_queries(
                    queries,
                    repeat,
                    [&grouped, &grouped_stats](const QueryVector& query) {
                        return grouped_top5(grouped, query, grouped_stats, false, TraversalMode::Sort);
                    }
                );

                ScanStats grouped_local_stats;
                const auto [grouped_local_elapsed, grouped_local_checksum] = time_queries(
                    queries,
                    repeat,
                    [&grouped, &grouped_local_stats](const QueryVector& query) {
                        return grouped_top5(grouped, query, grouped_local_stats, true, TraversalMode::Sort);
                    }
                );

                std::cout << "strategy=" << strategy.name
                          << " order=global"
                          << " groups=" << grouped.groups.size()
                          << " mismatches=" << mismatches
                          << " grouped_ns_per_query=" << ns_per_query(grouped_elapsed, queries.size(), repeat)
                          << " rows_per_query=" << static_cast<double>(grouped_stats.rows_scanned) /
                                 static_cast<double>(queries.size() * repeat)
                          << " groups_per_query=" << static_cast<double>(grouped_stats.groups_visited) /
                                 static_cast<double>(queries.size() * repeat)
                          << " checksum=" << grouped_checksum
                          << '\n';
                std::cout << "strategy=" << strategy.name
                          << " order=group_local"
                          << " groups=" << grouped.groups.size()
                          << " mismatches=" << local_mismatches
                          << " grouped_ns_per_query=" << ns_per_query(grouped_local_elapsed, queries.size(), repeat)
                          << " rows_per_query=" << static_cast<double>(grouped_local_stats.rows_scanned) /
                                 static_cast<double>(queries.size() * repeat)
                          << " groups_per_query=" << static_cast<double>(grouped_local_stats.groups_visited) /
                                 static_cast<double>(queries.size() * repeat)
                          << " checksum=" << grouped_local_checksum
                          << '\n';
            }
            return 0;
        }

        if (traversal) {
            const GroupingStrategy strategy{"base", true, 0, false, false};
            const GroupedRefs grouped = build_grouped_refs(refs, strategy);

            std::vector<std::size_t> baseline_counts;
            baseline_counts.reserve(queries.size());
            ScanStats validation_baseline_stats;
            for (const QueryVector& query : queries) {
                baseline_counts.push_back(baseline_top5(refs, query, validation_baseline_stats).fraud_count());
            }

            std::cout << "queries=" << queries.size()
                      << " repeat=" << repeat
                      << " refs=" << refs.len()
                      << " groups=" << grouped.groups.size()
                      << '\n';

            const std::array<TraversalMode, 3> traversal_modes{
                TraversalMode::Sort,
                TraversalMode::SelectMin,
                TraversalMode::Unsorted,
            };

            for (const bool use_group_order : {false, true}) {
                for (const TraversalMode mode : traversal_modes) {
                    std::size_t mismatches = 0;
                    ScanStats validation_stats;
                    for (std::size_t index = 0; index < queries.size(); ++index) {
                        const Top5 grouped_top = grouped_top5(
                            grouped,
                            queries[index],
                            validation_stats,
                            use_group_order,
                            mode
                        );
                        if (baseline_counts[index] != grouped_top.fraud_count()) {
                            ++mismatches;
                        }
                    }

                    ScanStats grouped_stats;
                    const auto [elapsed, checksum] = time_queries(
                        queries,
                        repeat,
                        [&grouped, &grouped_stats, use_group_order, mode](const QueryVector& query) {
                            return grouped_top5(grouped, query, grouped_stats, use_group_order, mode);
                        }
                    );

                    std::cout << "order=" << (use_group_order ? "group_local" : "global")
                              << " traversal=" << traversal_name(mode)
                              << " mismatches=" << mismatches
                              << " ns_per_query=" << ns_per_query(elapsed, queries.size(), repeat)
                              << " rows_per_query=" << static_cast<double>(grouped_stats.rows_scanned) /
                                     static_cast<double>(queries.size() * repeat)
                              << " groups_per_query=" << static_cast<double>(grouped_stats.groups_visited) /
                                     static_cast<double>(queries.size() * repeat)
                              << " checksum=" << checksum
                              << '\n';
                }
            }
            return 0;
        }

        if (budget) {
            const GroupingStrategy strategy{"base", true, 0, false, false};
            const GroupedRefs grouped = build_grouped_refs(refs, strategy);

            std::vector<std::size_t> exact_counts;
            exact_counts.reserve(queries.size());
            ScanStats exact_stats;
            const auto [exact_elapsed, exact_checksum] = time_queries(
                queries,
                repeat,
                [&grouped, &exact_stats, &exact_counts, repeat](const QueryVector& query) {
                    const Top5 top = grouped_top5(grouped, query, exact_stats, true, TraversalMode::Sort);
                    if (repeat == 1) {
                        exact_counts.push_back(top.fraud_count());
                    }
                    return top;
                }
            );

            if (repeat != 1) {
                ScanStats baseline_once_stats;
                for (const QueryVector& query : queries) {
                    exact_counts.push_back(
                        grouped_top5(grouped, query, baseline_once_stats, true, TraversalMode::Sort).fraud_count()
                    );
                }
            }

            std::cout << "queries=" << queries.size()
                      << " repeat=" << repeat
                      << " refs=" << refs.len()
                      << " groups=" << grouped.groups.size()
                      << " exact_ns_per_query=" << ns_per_query(exact_elapsed, queries.size(), repeat)
                      << " exact_rows_per_query=" << static_cast<double>(exact_stats.rows_scanned) /
                             static_cast<double>(queries.size() * repeat)
                      << " exact_groups_per_query=" << static_cast<double>(exact_stats.groups_visited) /
                             static_cast<double>(queries.size() * repeat)
                      << " checksum=" << exact_checksum
                      << '\n';

            for (const std::size_t group_budget : {1U, 2U, 3U, 4U, 5U, 6U, 8U, 10U}) {
                std::size_t fraud_count_mismatches = 0;
                std::size_t decision_errors = 0;
                ScanStats validation_stats;
                for (std::size_t index = 0; index < queries.size(); ++index) {
                    const Top5 top = grouped_budget_top5(
                        grouped,
                        queries[index],
                        validation_stats,
                        true,
                        group_budget
                    );
                    const std::size_t fraud_count = top.fraud_count();
                    fraud_count_mismatches += fraud_count != exact_counts[index] ? 1U : 0U;
                    decision_errors += (fraud_count < 3U) != (exact_counts[index] < 3U) ? 1U : 0U;
                }

                ScanStats budget_stats;
                const auto [elapsed, checksum] = time_queries(
                    queries,
                    repeat,
                    [&grouped, &budget_stats, group_budget](const QueryVector& query) {
                        return grouped_budget_top5(grouped, query, budget_stats, true, group_budget);
                    }
                );

                std::cout << "budget_groups=" << group_budget
                          << " fraud_count_mismatches=" << fraud_count_mismatches
                          << " decision_errors=" << decision_errors
                          << " ns_per_query=" << ns_per_query(elapsed, queries.size(), repeat)
                          << " rows_per_query=" << static_cast<double>(budget_stats.rows_scanned) /
                                 static_cast<double>(queries.size() * repeat)
                          << " groups_per_query=" << static_cast<double>(budget_stats.groups_visited) /
                                 static_cast<double>(queries.size() * repeat)
                          << " checksum=" << checksum
                          << '\n';
            }
            return 0;
        }

        const GroupingStrategy strategy{"base", true, 0, false, false};
        const GroupedRefs grouped = build_grouped_refs(refs, strategy);

        std::size_t mismatches = 0;
        ScanStats baseline_validation_stats;
        ScanStats grouped_validation_stats;
        for (const QueryVector& query : queries) {
            const Top5 baseline = baseline_top5(refs, query, baseline_validation_stats);
            const Top5 grouped_top = grouped_top5(
                grouped,
                query,
                grouped_validation_stats,
                false,
                TraversalMode::Sort
            );
            if (baseline.fraud_count() != grouped_top.fraud_count()) {
                ++mismatches;
            }
        }

        ScanStats baseline_stats;
        const auto [baseline_elapsed, baseline_checksum] = time_queries(
            queries,
            repeat,
            [&refs, &baseline_stats](const QueryVector& query) {
                return baseline_top5(refs, query, baseline_stats);
            }
        );

        ScanStats grouped_stats;
        const auto [grouped_elapsed, grouped_checksum] = time_queries(
            queries,
            repeat,
            [&grouped, &grouped_stats](const QueryVector& query) {
                return grouped_top5(grouped, query, grouped_stats, false, TraversalMode::Sort);
            }
        );

        std::cout << "queries=" << queries.size()
                  << " repeat=" << repeat
                  << " refs=" << refs.len()
                  << " groups=" << grouped.groups.size()
                  << " mismatches=" << mismatches
                  << '\n';
        std::cout << "baseline_ns_per_query=" << ns_per_query(baseline_elapsed, queries.size(), repeat)
                  << " rows_per_query=" << static_cast<double>(baseline_stats.rows_scanned) /
                         static_cast<double>(queries.size() * repeat)
                  << " checksum=" << baseline_checksum
                  << '\n';
        std::cout << "grouped_ns_per_query=" << ns_per_query(grouped_elapsed, queries.size(), repeat)
                  << " rows_per_query=" << static_cast<double>(grouped_stats.rows_scanned) /
                         static_cast<double>(queries.size() * repeat)
                  << " groups_per_query=" << static_cast<double>(grouped_stats.groups_visited) /
                         static_cast<double>(queries.size() * repeat)
                  << " checksum=" << grouped_checksum
                  << '\n';
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
