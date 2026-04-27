#include "rinha/classifier.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "rinha/vectorize.hpp"

namespace rinha {

namespace {

constexpr std::size_t kMaxReferenceGroups = 512;

float lower_bound_distance(const ReferenceGroup& group, const QueryVector& query) noexcept {
    float sum = 0.0f;
    for (std::size_t dim_index = 0; dim_index < kDimensions; ++dim_index) {
        float delta = 0.0f;
        if (query[dim_index] < group.min_values[dim_index]) {
            delta = group.min_values[dim_index] - query[dim_index];
        } else if (query[dim_index] > group.max_values[dim_index]) {
            delta = query[dim_index] - group.max_values[dim_index];
        }
        sum += delta * delta;
    }
    return sum;
}

}  // namespace

Classifier::Classifier(ReferenceSet refs) : refs_(std::move(refs)) {}

bool Classifier::classify(const Payload& payload, Classification& classification, std::string& error) const {
    if (refs_.len() < 5) {
        error = "conjunto de referências insuficiente para top-5";
        return false;
    }

    QueryVector query{};
    if (!vectorize(payload, query, error)) {
        return false;
    }

    const Top5 top =
#if defined(__x86_64__) || defined(_M_X64)
        supports_avx2() ? top5_avx2(query) :
#endif
                          top5_scalar(query);

    std::size_t fraud_count = 0;
    for (const auto& entry : top) {
        fraud_count += entry.second ? 1U : 0U;
    }

    classification.fraud_score = static_cast<float>(fraud_count) * 0.2f;
    classification.approved = fraud_count < 3U;
    return true;
}

bool Classifier::supports_avx2() noexcept {
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

Classifier::Top5 Classifier::top5_scalar(const QueryVector& query) const noexcept {
    Top5 top{};
    top.fill({std::numeric_limits<float>::infinity(), false});

    for (std::size_t index = 0; index < refs_.len(); ++index) {
        float distance = 0.0f;
        if (refs_.distance_squared_if_below(query, index, top[4].first, distance)) {
            insert_top5(top, distance, refs_.is_fraud(index));
        }
    }

    return top;
}

#if defined(__x86_64__) || defined(_M_X64)
Classifier::Top5 Classifier::top5_avx2(const QueryVector& query) const noexcept {
    Top5 top{};
    top.fill({std::numeric_limits<float>::infinity(), false});

    const auto& groups = refs_.groups();
    if (groups.empty() || groups.size() > kMaxReferenceGroups) {
        const std::size_t rows = refs_.len();
        const std::size_t chunks = rows / 8U;
        const auto& labels = refs_.labels();
        std::array<const float*, kDimensions> dims{};
        for (std::size_t index = 0; index < kDimensions; ++index) {
            dims[index] = refs_.dim(index).data();
        }

        __m256 query_lanes[kDimensions];
        for (std::size_t index = 0; index < kDimensions; ++index) {
            query_lanes[index] = _mm256_set1_ps(query[index]);
        }

        const auto& ordered_dims = refs_.dimension_order();

        for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
            const std::size_t offset = chunk * 8U;
            const float threshold = top[4].first;
            const __m256 threshold_lanes = _mm256_set1_ps(threshold);
            __m256 accum = _mm256_setzero_ps();
            bool pruned = false;

            for (const std::size_t dim_index : ordered_dims) {
                const __m256 refs_lane = _mm256_loadu_ps(dims[dim_index] + offset);
                const __m256 diff = _mm256_sub_ps(query_lanes[dim_index], refs_lane);
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
                insert_top5(top, distances[lane], labels[offset + lane] != 0);
            }
        }

        for (std::size_t index = chunks * 8U; index < rows; ++index) {
            float distance = 0.0f;
            if (refs_.distance_squared_if_below(query, index, top[4].first, distance)) {
                insert_top5(top, distance, labels[index] != 0);
            }
        }

        return top;
    }

    __m256 query_lanes[kDimensions];
    for (std::size_t index = 0; index < kDimensions; ++index) {
        query_lanes[index] = _mm256_set1_ps(query[index]);
    }

    const auto& ordered_dims = refs_.dimension_order();
    std::array<std::pair<float, std::size_t>, kMaxReferenceGroups> group_order{};
    for (std::size_t index = 0; index < groups.size(); ++index) {
        group_order[index] = {lower_bound_distance(groups[index], query), index};
    }
    std::sort(group_order.begin(), group_order.begin() + static_cast<std::ptrdiff_t>(groups.size()));

    for (std::size_t group_position = 0; group_position < groups.size(); ++group_position) {
        const auto [lower_bound, group_index] = group_order[group_position];
        if (lower_bound >= top[4].first) {
            break;
        }

        const ReferenceGroup& group = groups[group_index];
        const auto& labels = group.labels;
        const std::size_t rows = labels.size();
        const std::size_t chunks = rows / 8U;

        std::array<const float*, kDimensions> dims{};
        for (std::size_t index = 0; index < kDimensions; ++index) {
            dims[index] = group.dims[index].data();
        }

        for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
            const std::size_t offset = chunk * 8U;
            const float threshold = top[4].first;
            const __m256 threshold_lanes = _mm256_set1_ps(threshold);
            __m256 accum = _mm256_setzero_ps();
            bool pruned = false;

            for (const std::size_t dim_index : ordered_dims) {
                const __m256 refs_lane = _mm256_loadu_ps(dims[dim_index] + offset);
                const __m256 diff = _mm256_sub_ps(query_lanes[dim_index], refs_lane);
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
                insert_top5(top, distances[lane], labels[offset + lane] != 0);
            }
        }

        for (std::size_t row = chunks * 8U; row < rows; ++row) {
            float distance = 0.0f;
            bool below = true;
            for (const std::size_t dim_index : ordered_dims) {
                const float delta = query[dim_index] - group.dims[dim_index][row];
                distance += delta * delta;
                if (distance >= top[4].first) {
                    below = false;
                    break;
                }
            }
            if (below) {
                insert_top5(top, distance, labels[row] != 0);
            }
        }
    }

    return top;
}
#endif

void Classifier::insert_top5(Top5& top, float distance, bool is_fraud) noexcept {
    if (distance >= top[4].first) {
        return;
    }

    std::size_t position = 4;
    while (position > 0 && top[position - 1].first > distance) {
        top[position] = top[position - 1];
        --position;
    }
    top[position] = {distance, is_fraud};
}

}  // namespace rinha
