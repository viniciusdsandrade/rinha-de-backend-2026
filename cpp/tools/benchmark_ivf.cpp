#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "rinha/ivf.hpp"
#include "rinha/request.hpp"
#include "rinha/types.hpp"
#include "rinha/vectorize.hpp"
#include "simdjson.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
    std::string body;
    bool expected_approved = true;
};

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
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) {
        throw std::runtime_error("falha ao ler " + path);
    }
    return contents;
}

std::vector<Sample> load_samples(const std::string& path, std::size_t limit) {
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

    std::vector<Sample> samples;
    for (const auto entry : entries) {
        simdjson::dom::element request;
        if (entry["request"].get(request) != simdjson::SUCCESS) {
            throw std::runtime_error("entrada sem request");
        }

        bool expected_approved = true;
        if (entry["expected_approved"].get(expected_approved) != simdjson::SUCCESS) {
            throw std::runtime_error("entrada sem expected_approved");
        }

        samples.push_back({simdjson::minify(request), expected_approved});
        if (limit != 0 && samples.size() >= limit) {
            break;
        }
    }
    return samples;
}

std::uint64_t checksum(bool approved, std::uint8_t fraud_count) noexcept {
    return (approved ? 1U : 0U) + (static_cast<std::uint64_t>(fraud_count) << 8U);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 13) {
            std::cerr << "uso: benchmark-ivf-cpp <test-data.json> <index.bin> "
                      << "[repeat=1] [limit=0] [fast_nprobe=1] [full_nprobe=1] "
                      << "[bbox_repair=1] [repair_min=2] [repair_max=3] [stats=0] [disable_extreme=0] [print_errors=0]\n";
            return 1;
        }

        const std::string test_data_path = argv[1];
        const std::string index_path = argv[2];
        const std::size_t repeat = argc > 3 ? static_cast<std::size_t>(std::stoull(argv[3])) : 1U;
        const std::size_t limit = argc > 4 ? static_cast<std::size_t>(std::stoull(argv[4])) : 0U;

        rinha::IvfSearchConfig config{};
        if (argc > 5) {
            config.fast_nprobe = static_cast<std::uint32_t>(std::stoul(argv[5]));
        }
        if (argc > 6) {
            config.full_nprobe = static_cast<std::uint32_t>(std::stoul(argv[6]));
            config.boundary_full = config.full_nprobe > config.fast_nprobe;
        }
        if (argc > 7) {
            config.bbox_repair = std::stoul(argv[7]) != 0;
        }
        if (argc > 8) {
            config.repair_min_frauds = static_cast<std::uint8_t>(std::stoul(argv[8]));
            config.boundary_full = true;
        }
        if (argc > 9) {
            config.repair_max_frauds = static_cast<std::uint8_t>(std::stoul(argv[9]));
            config.boundary_full = true;
        }
#ifdef RINHA_IVF_STATS
        const bool collect_stats = argc > 10 && std::stoul(argv[10]) != 0;
        if (argc > 11) {
            config.disable_extreme_repair = std::stoul(argv[11]) != 0;
        }
        const bool print_errors = argc > 12 && std::stoul(argv[12]) != 0;
        rinha::IvfSearchStats stats{};
#else
        const bool collect_stats = false;
        const bool print_errors = false;
#endif

        const std::vector<Sample> samples = load_samples(test_data_path, limit);
        if (samples.empty()) {
            throw std::runtime_error("nenhum sample carregado");
        }

        std::string error;
        rinha::IvfIndex index;
        if (!index.load_binary(index_path, error)) {
            throw std::runtime_error(error);
        }

        std::uint64_t checksum_value = 0;
        std::uint64_t fp = 0;
        std::uint64_t fn = 0;
        std::uint64_t parse_errors = 0;

        const auto started_at = Clock::now();
        for (std::size_t pass = 0; pass < repeat; ++pass) {
            for (const Sample& sample : samples) {
                rinha::Payload payload;
                if (!rinha::parse_payload(std::string_view(sample.body), payload, error)) {
                    ++parse_errors;
                    continue;
                }
                rinha::QueryVector query{};
                if (!rinha::vectorize(payload, query, error)) {
                    ++parse_errors;
                    continue;
                }
                const std::uint8_t fraud_count =
#ifdef RINHA_IVF_STATS
                    collect_stats ? index.fraud_count_with_stats(query, config, stats) :
#endif
                    index.fraud_count(query, config);
                const bool approved = fraud_count < 3;
                checksum_value += checksum(approved, fraud_count);
                if (approved != sample.expected_approved) {
                    if (approved) {
                        ++fn;
                    } else {
                        ++fp;
                    }
                    if (print_errors && (fp + fn) <= 20U) {
                        std::cerr << "classification_error expected_approved=" << (sample.expected_approved ? 1 : 0)
                                  << " approved=" << (approved ? 1 : 0)
                                  << " fraud_count=" << static_cast<unsigned int>(fraud_count)
                                  << " query=[";
                        for (std::size_t dim = 0; dim < query.size(); ++dim) {
                            if (dim != 0) {
                                std::cerr << ',';
                            }
                            std::cerr << query[dim];
                        }
                        std::cerr << "] body=" << sample.body << '\n';
                    }
                }
            }
        }
        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_at).count();
        const auto total_queries = samples.size() * repeat;

        std::cout << "samples=" << samples.size()
                  << " repeat=" << repeat
                  << " refs=" << index.len()
                  << " clusters=" << index.clusters()
                  << " index_memory_mb=" << static_cast<double>(index.memory_bytes()) / (1024.0 * 1024.0)
                  << " fast_nprobe=" << config.fast_nprobe
                  << " full_nprobe=" << config.full_nprobe
                  << " boundary_full=" << (config.boundary_full ? 1 : 0)
                  << " bbox_repair=" << (config.bbox_repair ? 1 : 0)
                  << " repair_min=" << static_cast<unsigned int>(config.repair_min_frauds)
                  << " repair_max=" << static_cast<unsigned int>(config.repair_max_frauds)
                  << '\n';
        std::cout << "ns_per_query=" << static_cast<double>(elapsed_ns) / static_cast<double>(total_queries)
                  << " checksum=" << checksum_value
                  << " fp=" << fp
                  << " fn=" << fn
                  << " parse_errors=" << parse_errors
                  << " failure_rate_pct=" << (static_cast<double>(fp + fn + parse_errors) * 100.0 / static_cast<double>(total_queries))
                  << '\n';
#ifdef RINHA_IVF_STATS
        if (collect_stats) {
            const double queries = static_cast<double>(stats.queries);
            std::cout << "stats_queries=" << stats.queries
                      << " repaired_queries=" << stats.repaired_queries
                      << " repaired_pct=" << (static_cast<double>(stats.repaired_queries) * 100.0 / queries)
                      << " extreme_repair_queries=" << stats.extreme_repair_queries
                      << '\n';
            std::cout << "fast_fraud_counts";
            for (std::size_t index = 0; index < stats.fast_fraud_counts.size(); ++index) {
                std::cout << " f" << index << '=' << stats.fast_fraud_counts[index];
            }
            std::cout << '\n';
            std::cout << "avg_primary_clusters=" << (static_cast<double>(stats.primary_scanned_clusters) / queries)
                      << " avg_primary_blocks=" << (static_cast<double>(stats.primary_scanned_blocks) / queries)
                      << " avg_bbox_tested_clusters=" << (static_cast<double>(stats.bbox_tested_clusters) / queries)
                      << " avg_bbox_scanned_clusters=" << (static_cast<double>(stats.bbox_scanned_clusters) / queries)
                      << " avg_bbox_scanned_blocks=" << (static_cast<double>(stats.bbox_scanned_blocks) / queries)
                      << '\n';
        }
#endif
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
