#include "rinha/ivf.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace rinha {

namespace {

constexpr std::array<char, 4> kMagic{'I', 'V', 'F', '8'};
constexpr float kQuantScale = 10'000.0f;
constexpr std::uint32_t kBlockLanes = 8;

struct RawReferences {
    std::vector<float> vectors;
    std::vector<std::uint8_t> labels;

    [[nodiscard]] std::size_t len() const noexcept {
        return labels.size();
    }
};

struct Top5 {
    std::array<std::uint64_t, 5> distances{};
    std::array<std::uint8_t, 5> labels{};
    std::array<std::uint32_t, 5> ids{};
    std::size_t worst = 0;

    Top5() {
        distances.fill(std::numeric_limits<std::uint64_t>::max());
        labels.fill(0);
        ids.fill(std::numeric_limits<std::uint32_t>::max());
    }

    [[nodiscard]] bool better(std::uint64_t distance, std::uint32_t id, std::size_t pos) const noexcept {
        return distance < distances[pos] || (distance == distances[pos] && id < ids[pos]);
    }

    void refresh_worst() noexcept {
        worst = 0;
        for (std::size_t pos = 1; pos < distances.size(); ++pos) {
            if (distances[pos] > distances[worst] ||
                (distances[pos] == distances[worst] && ids[pos] > ids[worst])) {
                worst = pos;
            }
        }
    }

    void insert(std::uint64_t distance, std::uint8_t label, std::uint32_t id) noexcept {
        if (!better(distance, id, worst)) {
            return;
        }
        distances[worst] = distance;
        labels[worst] = label;
        ids[worst] = id;
        refresh_worst();
    }

    [[nodiscard]] std::uint64_t worst_distance() const noexcept {
        return distances[worst];
    }

    [[nodiscard]] std::uint8_t frauds() const noexcept {
        std::uint8_t count = 0;
        for (const std::uint8_t label : labels) {
            count = static_cast<std::uint8_t>(count + (label != 0 ? 1U : 0U));
        }
        return count;
    }
};

std::int16_t quantize(float value) noexcept {
    if (value < -1.0f) {
        value = -1.0f;
    } else if (value > 1.0f) {
        value = 1.0f;
    }
    const float scaled = value * kQuantScale;
    const long rounded = std::lround(scaled);
    if (rounded < std::numeric_limits<std::int16_t>::min()) {
        return std::numeric_limits<std::int16_t>::min();
    }
    if (rounded > std::numeric_limits<std::int16_t>::max()) {
        return std::numeric_limits<std::int16_t>::max();
    }
    return static_cast<std::int16_t>(rounded);
}

std::uint64_t sqdiff_i16(std::int16_t left, std::int16_t right) noexcept {
    const std::int64_t delta = static_cast<std::int64_t>(left) - static_cast<std::int64_t>(right);
    return static_cast<std::uint64_t>(delta * delta);
}

bool has_suffix(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool read_gzip_chunk(gzFile file, std::string& buffer, std::string& error) {
    std::array<char, 256 * 1024> chunk{};
    const int read = gzread(file, chunk.data(), static_cast<unsigned int>(chunk.size()));
    if (read > 0) {
        buffer.append(chunk.data(), static_cast<std::size_t>(read));
        return true;
    }
    if (read == 0) {
        return false;
    }
    int zlib_error = Z_OK;
    const char* message = gzerror(file, &zlib_error);
    error = "falha ao ler gzip: ";
    error += message == nullptr ? "erro desconhecido" : message;
    return false;
}

char* find_bytes(char* begin, char* end, std::string_view needle) noexcept {
    if (needle.empty() || static_cast<std::size_t>(end - begin) < needle.size()) {
        return nullptr;
    }
    const char* found = std::search(begin, end, needle.begin(), needle.end());
    return found == end ? nullptr : const_cast<char*>(found);
}

bool parse_references_stream(
    const std::string& path,
    std::size_t max_references,
    RawReferences& refs,
    std::string& error
) {
    if (!has_suffix(path, ".gz")) {
        error = "prepare IVF espera references.json.gz";
        return false;
    }

    gzFile file = gzopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "falha ao abrir gzip: " + path;
        return false;
    }

    refs = RawReferences{};
    refs.vectors.reserve(max_references == 0 ? 3'000'000ULL * kDimensions : max_references * kDimensions);
    refs.labels.reserve(max_references == 0 ? 3'000'000ULL : max_references);

    std::string buffer;
    buffer.reserve(2 * 1024 * 1024);
    bool eof = false;

    while (!eof) {
        if (!read_gzip_chunk(file, buffer, error)) {
            if (!error.empty()) {
                gzclose(file);
                return false;
            }
            eof = true;
        }

        char* begin = buffer.data();
        char* end = begin + buffer.size();
        char* cursor = begin;
        char* keep_from = end;

        while (cursor < end) {
            char* vector_key = find_bytes(cursor, end, "\"vector\"");
            if (vector_key == nullptr) {
                const std::size_t tail = std::min<std::size_t>(buffer.size(), 64);
                keep_from = end - static_cast<std::ptrdiff_t>(tail);
                break;
            }

            char* left_bracket = static_cast<char*>(std::memchr(vector_key, '[', static_cast<std::size_t>(end - vector_key)));
            if (left_bracket == nullptr) {
                keep_from = vector_key;
                break;
            }
            char* right_bracket = static_cast<char*>(std::memchr(left_bracket, ']', static_cast<std::size_t>(end - left_bracket)));
            if (right_bracket == nullptr) {
                keep_from = vector_key;
                break;
            }

            std::array<float, kDimensions> vector{};
            char* number_cursor = left_bracket + 1;
            for (std::size_t dim = 0; dim < kDimensions; ++dim) {
                while (number_cursor < right_bracket &&
                       (*number_cursor == ' ' || *number_cursor == '\n' || *number_cursor == '\r' ||
                        *number_cursor == '\t' || *number_cursor == ',')) {
                    ++number_cursor;
                }
                char* number_end = number_cursor;
                errno = 0;
                const float value = std::strtof(number_cursor, &number_end);
                if (number_end == number_cursor || number_end > right_bracket || errno == ERANGE) {
                    error = "falha ao ler dimensão em references.json.gz";
                    gzclose(file);
                    return false;
                }
                vector[dim] = value;
                number_cursor = number_end;
            }

            char* label_key = find_bytes(right_bracket, end, "\"label\"");
            if (label_key == nullptr) {
                keep_from = vector_key;
                break;
            }
            char* colon = static_cast<char*>(std::memchr(label_key, ':', static_cast<std::size_t>(end - label_key)));
            if (colon == nullptr) {
                keep_from = vector_key;
                break;
            }
            char* quote = static_cast<char*>(std::memchr(colon, '"', static_cast<std::size_t>(end - colon)));
            if (quote == nullptr) {
                keep_from = vector_key;
                break;
            }
            ++quote;
            char* quote_end = static_cast<char*>(std::memchr(quote, '"', static_cast<std::size_t>(end - quote)));
            if (quote_end == nullptr) {
                keep_from = vector_key;
                break;
            }

            refs.vectors.insert(refs.vectors.end(), vector.begin(), vector.end());
            refs.labels.push_back(
                quote_end - quote >= 5 && std::strncmp(quote, "fraud", 5) == 0 ? 1U : 0U
            );

            cursor = quote_end + 1;
            keep_from = cursor;

            if (max_references != 0 && refs.len() >= max_references) {
                gzclose(file);
                return true;
            }
        }

        if (keep_from < end) {
            const std::size_t keep = static_cast<std::size_t>(end - keep_from);
            if (keep > buffer.capacity() / 2U) {
                error = "objeto JSON incompleto grande demais no parser streaming";
                gzclose(file);
                return false;
            }
            std::memmove(buffer.data(), keep_from, keep);
            buffer.resize(keep);
        } else {
            buffer.clear();
        }
    }

    gzclose(file);
    if (refs.len() == 0) {
        error = "nenhuma referência carregada";
        return false;
    }
    return true;
}

float distance_sq_to_centroid(const float* vector, const float* centroids, std::uint32_t cluster) noexcept {
    const float* centroid = centroids + static_cast<std::size_t>(cluster) * kDimensions;
    float distance = 0.0f;
    for (std::size_t dim = 0; dim < kDimensions; ++dim) {
        const float delta = vector[dim] - centroid[dim];
        distance += delta * delta;
    }
    return distance;
}

std::uint32_t nearest_centroid(const float* vector, const std::vector<float>& centroids, std::uint32_t clusters) noexcept {
    std::uint32_t best = 0;
    float best_distance = std::numeric_limits<float>::infinity();
    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        const float distance = distance_sq_to_centroid(vector, centroids.data(), cluster);
        if (distance < best_distance) {
            best_distance = distance;
            best = cluster;
        }
    }
    return best;
}

void train_kmeans(
    const RawReferences& refs,
    std::uint32_t clusters,
    std::uint32_t train_sample,
    std::uint32_t iterations,
    std::vector<float>& centroids
) {
    const std::size_t rows = refs.len();
    const std::uint32_t sample = std::max<std::uint32_t>(clusters, std::min<std::uint32_t>(train_sample, static_cast<std::uint32_t>(rows)));
    std::vector<std::uint32_t> sample_rows(sample);
    for (std::uint32_t index = 0; index < sample; ++index) {
        sample_rows[index] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(index) * rows) / sample);
    }

    centroids.assign(static_cast<std::size_t>(clusters) * kDimensions, 0.0f);
    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        const std::uint32_t sample_index = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(cluster) * sample) / clusters
        );
        const float* source = refs.vectors.data() + static_cast<std::size_t>(sample_rows[sample_index]) * kDimensions;
        std::copy(source, source + kDimensions, centroids.begin() + static_cast<std::ptrdiff_t>(cluster * kDimensions));
    }

    std::vector<double> sums(static_cast<std::size_t>(clusters) * kDimensions);
    std::vector<std::uint32_t> counts(clusters);
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        std::fill(sums.begin(), sums.end(), 0.0);
        std::fill(counts.begin(), counts.end(), 0);

        for (const std::uint32_t row : sample_rows) {
            const float* vector = refs.vectors.data() + static_cast<std::size_t>(row) * kDimensions;
            const std::uint32_t cluster = nearest_centroid(vector, centroids, clusters);
            ++counts[cluster];
            double* sum = sums.data() + static_cast<std::size_t>(cluster) * kDimensions;
            for (std::size_t dim = 0; dim < kDimensions; ++dim) {
                sum[dim] += vector[dim];
            }
        }

        for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
            if (counts[cluster] == 0) {
                continue;
            }
            float* centroid = centroids.data() + static_cast<std::size_t>(cluster) * kDimensions;
            const double* sum = sums.data() + static_cast<std::size_t>(cluster) * kDimensions;
            const double inv_count = 1.0 / static_cast<double>(counts[cluster]);
            for (std::size_t dim = 0; dim < kDimensions; ++dim) {
                centroid[dim] = static_cast<float>(sum[dim] * inv_count);
            }
        }
    }
}

bool write_exact(std::ofstream& out, const void* data, std::size_t bytes) {
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(out);
}

bool read_exact(std::ifstream& in, void* data, std::size_t bytes) {
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(in);
}

void insert_probe(
    std::uint32_t cluster,
    float distance,
    std::uint32_t* best_clusters,
    float* best_distances,
    std::uint32_t nprobe
) noexcept {
    if (distance >= best_distances[nprobe - 1U]) {
        return;
    }
    std::uint32_t pos = nprobe - 1U;
    while (pos > 0 && distance < best_distances[pos - 1U]) {
        best_distances[pos] = best_distances[pos - 1U];
        best_clusters[pos] = best_clusters[pos - 1U];
        --pos;
    }
    best_distances[pos] = distance;
    best_clusters[pos] = cluster;
}

std::uint64_t bbox_lower_bound(
    const std::vector<std::int16_t>& bbox_min,
    const std::vector<std::int16_t>& bbox_max,
    std::uint32_t cluster,
    const std::array<std::int16_t, kDimensions>& query,
    std::uint64_t stop_after
) noexcept {
    const std::size_t base = static_cast<std::size_t>(cluster) * kDimensions;
    std::uint64_t sum = 0;
    for (std::size_t dim = 0; dim < kDimensions; ++dim) {
        std::int16_t target = query[dim];
        if (target < bbox_min[base + dim]) {
            sum += sqdiff_i16(target, bbox_min[base + dim]);
        } else if (target > bbox_max[base + dim]) {
            sum += sqdiff_i16(target, bbox_max[base + dim]);
        }
        if (sum > stop_after) {
            return sum;
        }
    }
    return sum;
}

void scan_blocks_scalar(
    Top5& top,
    const std::vector<std::int16_t>& blocks,
    const std::vector<std::uint8_t>& labels,
    const std::vector<std::uint32_t>& orig_ids,
    std::uint32_t start_block,
    std::uint32_t end_block,
    const std::array<std::int16_t, kDimensions>& query
) noexcept {
    for (std::uint32_t block = start_block; block < end_block; ++block) {
        const std::size_t block_base = static_cast<std::size_t>(block) * kDimensions * kBlockLanes;
        const std::size_t label_base = static_cast<std::size_t>(block) * kBlockLanes;
        for (std::uint32_t lane = 0; lane < kBlockLanes; ++lane) {
            const std::uint32_t id = orig_ids[label_base + lane];
            if (id == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            std::uint64_t distance = 0;
            for (std::size_t dim = 0; dim < kDimensions; ++dim) {
                distance += sqdiff_i16(query[dim], blocks[block_base + (dim * kBlockLanes) + lane]);
                if (distance > top.worst_distance()) {
                    break;
                }
            }
            top.insert(distance, labels[label_base + lane], id);
        }
    }
}

#if defined(__x86_64__) || defined(_M_X64)
void acc_dim_i64(
    __m256i& lo,
    __m256i& hi,
    __m256i q32,
    const std::int16_t* ptr
) noexcept {
    const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
    const __m256i value = _mm256_cvtepi16_epi32(raw);
    const __m256i diff = _mm256_sub_epi32(value, q32);
    const __m256i square32 = _mm256_mullo_epi32(diff, diff);
    const __m128i square_lo = _mm256_castsi256_si128(square32);
    const __m128i square_hi = _mm256_extracti128_si256(square32, 1);
    lo = _mm256_add_epi64(lo, _mm256_cvtepi32_epi64(square_lo));
    hi = _mm256_add_epi64(hi, _mm256_cvtepi32_epi64(square_hi));
}

__attribute__((target("avx2")))
void scan_blocks_avx2(
    Top5& top,
    const std::vector<std::int16_t>& blocks,
    const std::vector<std::uint8_t>& labels,
    const std::vector<std::uint32_t>& orig_ids,
    std::uint32_t start_block,
    std::uint32_t end_block,
    const std::array<std::int16_t, kDimensions>& query
) noexcept {
    const std::int16_t* blocks_ptr = blocks.data();
    const std::uint8_t* labels_ptr = labels.data();
    const std::uint32_t* ids_ptr = orig_ids.data();

    __m256i q[kDimensions];
    for (std::size_t dim = 0; dim < kDimensions; ++dim) {
        q[dim] = _mm256_set1_epi32(static_cast<int>(query[dim]));
    }

    alignas(32) std::array<std::uint64_t, 4> lo_values{};
    alignas(32) std::array<std::uint64_t, 4> hi_values{};

    for (std::uint32_t block = start_block; block < end_block; ++block) {
        const std::size_t block_base = static_cast<std::size_t>(block) * kDimensions * kBlockLanes;
        __m256i lo = _mm256_setzero_si256();
        __m256i hi = _mm256_setzero_si256();
        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
            acc_dim_i64(lo, hi, q[dim], blocks_ptr + block_base + (dim * kBlockLanes));
        }

        _mm256_store_si256(reinterpret_cast<__m256i*>(lo_values.data()), lo);
        _mm256_store_si256(reinterpret_cast<__m256i*>(hi_values.data()), hi);

        const std::size_t label_base = static_cast<std::size_t>(block) * kBlockLanes;
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const std::uint32_t id = ids_ptr[label_base + lane];
            if (id != std::numeric_limits<std::uint32_t>::max()) {
                top.insert(lo_values[lane], labels_ptr[label_base + lane], id);
            }
        }
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const std::uint32_t id = ids_ptr[label_base + 4U + lane];
            if (id != std::numeric_limits<std::uint32_t>::max()) {
                top.insert(hi_values[lane], labels_ptr[label_base + 4U + lane], id);
            }
        }
    }
}
#endif

bool supports_avx2() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return true;
#endif
#else
    return false;
#endif
}

void scan_blocks(
    Top5& top,
    const std::vector<std::int16_t>& blocks,
    const std::vector<std::uint8_t>& labels,
    const std::vector<std::uint32_t>& orig_ids,
    std::uint32_t start_block,
    std::uint32_t end_block,
    const std::array<std::int16_t, kDimensions>& query
) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    static const bool avx2_available = supports_avx2();
    if (avx2_available) {
        scan_blocks_avx2(top, blocks, labels, orig_ids, start_block, end_block, query);
        return;
    }
#endif
    scan_blocks_scalar(top, blocks, labels, orig_ids, start_block, end_block, query);
}

}  // namespace

bool IvfIndex::build_from_gzip_json(
    const std::string& path,
    const IvfBuildOptions& options,
    IvfIndex& index,
    std::string& error
) {
    if (options.clusters < 1) {
        error = "clusters deve ser >= 1";
        return false;
    }
    if (options.clusters > std::numeric_limits<std::uint16_t>::max()) {
        error = "clusters excede uint16";
        return false;
    }

    RawReferences refs;
    if (!parse_references_stream(path, options.max_references, refs, error)) {
        return false;
    }

    const std::uint32_t clusters = std::min<std::uint32_t>(options.clusters, static_cast<std::uint32_t>(refs.len()));
    std::vector<float> centroids;
    train_kmeans(refs, clusters, options.train_sample, options.iterations, centroids);

    std::vector<std::uint16_t> assignments(refs.len());
    std::vector<std::uint32_t> counts(clusters, 0);
    for (std::size_t row = 0; row < refs.len(); ++row) {
        const float* vector = refs.vectors.data() + row * kDimensions;
        const std::uint32_t cluster = nearest_centroid(vector, centroids, clusters);
        assignments[row] = static_cast<std::uint16_t>(cluster);
        ++counts[cluster];
    }

    IvfIndex built;
    built.n_ = static_cast<std::uint32_t>(refs.len());
    built.clusters_ = clusters;
    built.offsets_.assign(static_cast<std::size_t>(clusters) + 1U, 0);
    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        built.offsets_[cluster + 1U] = built.offsets_[cluster] + ((counts[cluster] + kBlockLanes - 1U) / kBlockLanes);
    }
    built.total_blocks_ = built.offsets_.back();
    const std::size_t padded_rows = static_cast<std::size_t>(built.total_blocks_) * kBlockLanes;

    built.centroids_.assign(static_cast<std::size_t>(clusters) * kDimensions, 0.0f);
    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
            built.centroids_[dim * clusters + cluster] = centroids[static_cast<std::size_t>(cluster) * kDimensions + dim];
        }
    }

    built.bbox_min_.assign(static_cast<std::size_t>(clusters) * kDimensions, std::numeric_limits<std::int16_t>::max());
    built.bbox_max_.assign(static_cast<std::size_t>(clusters) * kDimensions, std::numeric_limits<std::int16_t>::min());
    built.labels_.assign(padded_rows, 0);
    built.orig_ids_.assign(padded_rows, std::numeric_limits<std::uint32_t>::max());
    built.blocks_.assign(static_cast<std::size_t>(built.total_blocks_) * kDimensions * kBlockLanes, std::numeric_limits<std::int16_t>::max());

    std::vector<std::uint32_t> cluster_positions(clusters, 0);
    for (std::size_t row = 0; row < refs.len(); ++row) {
        const std::uint32_t cluster = assignments[row];
        const std::uint32_t position = cluster_positions[cluster]++;
        const std::uint32_t block = built.offsets_[cluster] + (position / kBlockLanes);
        const std::uint32_t lane = position % kBlockLanes;
        const std::size_t block_base = static_cast<std::size_t>(block) * kDimensions * kBlockLanes;
        const std::size_t label_base = static_cast<std::size_t>(block) * kBlockLanes;
        const float* vector = refs.vectors.data() + row * kDimensions;

        built.labels_[label_base + lane] = refs.labels[row];
        built.orig_ids_[label_base + lane] = static_cast<std::uint32_t>(row);

        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
            const std::int16_t value = quantize(vector[dim]);
            built.blocks_[block_base + (dim * kBlockLanes) + lane] = value;
            const std::size_t bbox_index = static_cast<std::size_t>(cluster) * kDimensions + dim;
            built.bbox_min_[bbox_index] = std::min(built.bbox_min_[bbox_index], value);
            built.bbox_max_[bbox_index] = std::max(built.bbox_max_[bbox_index], value);
        }
    }

    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        if (counts[cluster] != 0) {
            continue;
        }
        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
            built.bbox_min_[static_cast<std::size_t>(cluster) * kDimensions + dim] = 0;
            built.bbox_max_[static_cast<std::size_t>(cluster) * kDimensions + dim] = 0;
        }
    }

    index = std::move(built);
    return true;
}

bool IvfIndex::load_binary(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "falha ao abrir índice IVF: " + path;
        return false;
    }

    std::array<char, 4> magic{};
    std::uint32_t dim = 0;
    std::uint32_t scale = 0;
    if (!read_exact(in, magic.data(), magic.size()) ||
        !read_exact(in, &n_, sizeof(n_)) ||
        !read_exact(in, &clusters_, sizeof(clusters_)) ||
        !read_exact(in, &dim, sizeof(dim)) ||
        !read_exact(in, &scale, sizeof(scale)) ||
        !read_exact(in, &total_blocks_, sizeof(total_blocks_))) {
        error = "índice IVF truncado";
        return false;
    }
    if (magic != kMagic || dim != kDimensions || scale != static_cast<std::uint32_t>(kQuantScale) || clusters_ == 0) {
        error = "índice IVF incompatível";
        return false;
    }

    const std::size_t padded_rows = static_cast<std::size_t>(total_blocks_) * kBlockLanes;
    centroids_.resize(static_cast<std::size_t>(clusters_) * kDimensions);
    bbox_min_.resize(static_cast<std::size_t>(clusters_) * kDimensions);
    bbox_max_.resize(static_cast<std::size_t>(clusters_) * kDimensions);
    offsets_.resize(static_cast<std::size_t>(clusters_) + 1U);
    labels_.resize(padded_rows);
    orig_ids_.resize(padded_rows);
    blocks_.resize(static_cast<std::size_t>(total_blocks_) * kDimensions * kBlockLanes);

    if (!read_exact(in, centroids_.data(), centroids_.size() * sizeof(float)) ||
        !read_exact(in, bbox_min_.data(), bbox_min_.size() * sizeof(std::int16_t)) ||
        !read_exact(in, bbox_max_.data(), bbox_max_.size() * sizeof(std::int16_t)) ||
        !read_exact(in, offsets_.data(), offsets_.size() * sizeof(std::uint32_t)) ||
        !read_exact(in, labels_.data(), labels_.size() * sizeof(std::uint8_t)) ||
        !read_exact(in, orig_ids_.data(), orig_ids_.size() * sizeof(std::uint32_t)) ||
        !read_exact(in, blocks_.data(), blocks_.size() * sizeof(std::int16_t))) {
        error = "índice IVF truncado";
        return false;
    }

    if (offsets_.empty() || offsets_.front() != 0 || offsets_.back() != total_blocks_) {
        error = "offsets inválidos no índice IVF";
        return false;
    }
    return true;
}

bool IvfIndex::write_binary(const std::string& path, std::string& error) const {
    if (n_ == 0 || clusters_ == 0 || total_blocks_ == 0) {
        error = "índice IVF vazio";
        return false;
    }

    const std::filesystem::path out_path(path);
    if (out_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            error = "falha ao criar diretório do índice IVF: " + ec.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "falha ao criar índice IVF: " + path;
        return false;
    }

    const std::uint32_t dim = static_cast<std::uint32_t>(kDimensions);
    const std::uint32_t scale = static_cast<std::uint32_t>(kQuantScale);
    if (!write_exact(out, kMagic.data(), kMagic.size()) ||
        !write_exact(out, &n_, sizeof(n_)) ||
        !write_exact(out, &clusters_, sizeof(clusters_)) ||
        !write_exact(out, &dim, sizeof(dim)) ||
        !write_exact(out, &scale, sizeof(scale)) ||
        !write_exact(out, &total_blocks_, sizeof(total_blocks_)) ||
        !write_exact(out, centroids_.data(), centroids_.size() * sizeof(float)) ||
        !write_exact(out, bbox_min_.data(), bbox_min_.size() * sizeof(std::int16_t)) ||
        !write_exact(out, bbox_max_.data(), bbox_max_.size() * sizeof(std::int16_t)) ||
        !write_exact(out, offsets_.data(), offsets_.size() * sizeof(std::uint32_t)) ||
        !write_exact(out, labels_.data(), labels_.size() * sizeof(std::uint8_t)) ||
        !write_exact(out, orig_ids_.data(), orig_ids_.size() * sizeof(std::uint32_t)) ||
        !write_exact(out, blocks_.data(), blocks_.size() * sizeof(std::int16_t))) {
        error = "falha ao escrever índice IVF";
        return false;
    }
    return true;
}

std::uint8_t IvfIndex::fraud_count(const QueryVector& query, const IvfSearchConfig& config) const noexcept {
    std::array<std::int16_t, kDimensions> query_i16{};
    for (std::size_t dim = 0; dim < kDimensions; ++dim) {
        query_i16[dim] = quantize(query[dim]);
    }

    const std::uint32_t fast_nprobe = std::max<std::uint32_t>(1, std::min(config.fast_nprobe, clusters_));
    const bool fast_repair = config.bbox_repair && !config.boundary_full;
    std::uint8_t frauds = fraud_count_once(query_i16, query, fast_nprobe, fast_repair);
    if (config.boundary_full &&
        frauds >= config.repair_min_frauds &&
        frauds <= config.repair_max_frauds) {
        const std::uint32_t full_nprobe = std::max<std::uint32_t>(fast_nprobe, std::min(config.full_nprobe, clusters_));
        frauds = fraud_count_once(query_i16, query, full_nprobe, config.bbox_repair);
    }
    return frauds;
}

std::uint8_t IvfIndex::fraud_count_once(
    const std::array<std::int16_t, kDimensions>& query_i16,
    const QueryVector& query_float,
    std::uint32_t nprobe,
    bool repair
) const noexcept {
    if (nprobe == 1U) {
        return fraud_count_once_fixed<1>(query_i16, query_float, nprobe, repair);
    }
    if (nprobe <= 8U) {
        return fraud_count_once_fixed<8>(query_i16, query_float, nprobe, repair);
    }
    if (nprobe <= 16U) {
        return fraud_count_once_fixed<16>(query_i16, query_float, nprobe, repair);
    }
    if (nprobe <= 32U) {
        return fraud_count_once_fixed<32>(query_i16, query_float, nprobe, repair);
    }
    return fraud_count_once_fixed<64>(query_i16, query_float, std::min<std::uint32_t>(nprobe, 64U), repair);
}

template <std::size_t MaxNprobe>
std::uint8_t IvfIndex::fraud_count_once_fixed(
    const std::array<std::int16_t, kDimensions>& query_i16,
    const QueryVector& query_float,
    std::uint32_t nprobe,
    bool repair
) const noexcept {
    if (n_ < 5 || clusters_ == 0) {
        return 0;
    }

    std::array<std::uint32_t, MaxNprobe> best_clusters{};
    std::array<float, MaxNprobe> best_distances{};
    best_distances.fill(std::numeric_limits<float>::infinity());
    for (std::uint32_t cluster = 0; cluster < clusters_; ++cluster) {
        float distance = 0.0f;
        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
            const float centroid = centroids_[dim * clusters_ + cluster];
            const float delta = query_float[dim] - centroid;
            distance += delta * delta;
        }
        insert_probe(cluster, distance, best_clusters.data(), best_distances.data(), nprobe);
    }

    Top5 top;
    for (std::uint32_t index = 0; index < nprobe; ++index) {
        const std::uint32_t cluster = best_clusters[index];
        scan_blocks(top, blocks_, labels_, orig_ids_, offsets_[cluster], offsets_[cluster + 1U], query_i16);
    }

    if (repair) {
        for (std::uint32_t cluster = 0; cluster < clusters_; ++cluster) {
            if (offsets_[cluster] == offsets_[cluster + 1U]) {
                continue;
            }
            bool already_scanned = false;
            for (std::uint32_t index = 0; index < nprobe; ++index) {
                already_scanned = already_scanned || best_clusters[index] == cluster;
            }
            if (already_scanned) {
                continue;
            }
            const std::uint64_t worst = top.worst_distance();
            if (bbox_lower_bound(bbox_min_, bbox_max_, cluster, query_i16, worst) <= worst) {
                scan_blocks(top, blocks_, labels_, orig_ids_, offsets_[cluster], offsets_[cluster + 1U], query_i16);
            }
        }
    }

    return top.frauds();
}

std::size_t IvfIndex::len() const noexcept {
    return n_;
}

std::uint32_t IvfIndex::clusters() const noexcept {
    return clusters_;
}

std::size_t IvfIndex::padded_len() const noexcept {
    return static_cast<std::size_t>(total_blocks_) * kBlockLanes;
}

std::size_t IvfIndex::memory_bytes() const noexcept {
    return (centroids_.size() * sizeof(float)) +
           (bbox_min_.size() * sizeof(std::int16_t)) +
           (bbox_max_.size() * sizeof(std::int16_t)) +
           (offsets_.size() * sizeof(std::uint32_t)) +
           (labels_.size() * sizeof(std::uint8_t)) +
           (orig_ids_.size() * sizeof(std::uint32_t)) +
           (blocks_.size() * sizeof(std::int16_t));
}

}  // namespace rinha
