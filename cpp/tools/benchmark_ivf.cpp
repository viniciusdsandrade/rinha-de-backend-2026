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
        if (argc < 3 || argc > 10) {
            std::cerr << "uso: benchmark-ivf-cpp <test-data.json> <index.bin> "
                      << "[repeat=1] [limit=0] [fast_nprobe=1] [full_nprobe=1] "
                      << "[bbox_repair=1] [repair_min=2] [repair_max=3]\n";
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
                const std::uint8_t fraud_count = index.fraud_count(query, config);
                const bool approved = fraud_count < 3;
                checksum_value += checksum(approved, fraud_count);
                if (approved != sample.expected_approved) {
                    if (approved) {
                        ++fn;
                    } else {
                        ++fp;
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
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
