#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rinha/classifier.hpp"
#include "rinha/request.hpp"
#include "rinha/types.hpp"
#include "rinha/vectorize.hpp"
#include "simdjson.h"

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct Sample {
    std::string body;
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

    std::vector<Sample> samples;
    for (const auto entry : entries) {
        simdjson::dom::element request;
        if (entry["request"].get(request) != simdjson::SUCCESS) {
            throw std::runtime_error("entrada sem request");
        }

        samples.push_back({simdjson::minify(request)});
        if (limit != 0 && samples.size() >= limit) {
            break;
        }
    }
    return samples;
}

template <typename Fn>
std::pair<Nanoseconds, std::uint64_t> time_samples(
    const std::vector<Sample>& samples,
    std::size_t repeat,
    Fn&& fn
) {
    std::uint64_t checksum = 0;
    const auto started_at = Clock::now();
    for (std::size_t pass = 0; pass < repeat; ++pass) {
        for (const Sample& sample : samples) {
            checksum += fn(sample);
        }
    }
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - started_at);
    return {elapsed, checksum};
}

double ns_per_query(Nanoseconds elapsed, std::size_t sample_count, std::size_t repeat) {
    return static_cast<double>(elapsed.count()) / static_cast<double>(sample_count * repeat);
}

std::uint64_t payload_checksum(const rinha::Payload& payload) noexcept {
    std::uint64_t checksum = payload.transaction_installments;
    checksum += payload.customer_tx_count_24h << 8U;
    checksum += payload.transaction_requested_at.size() << 16U;
    checksum += payload.merchant_mcc.size() << 24U;
    checksum += payload.last_transaction_timestamp.size() << 32U;
    checksum += payload.known_merchant ? 17U : 0U;
    checksum += payload.terminal_is_online ? 31U : 0U;
    checksum += payload.terminal_card_present ? 47U : 0U;
    checksum += payload.has_last_transaction ? 67U : 0U;
    return checksum;
}

std::uint64_t vector_checksum(const rinha::QueryVector& vector) noexcept {
    std::uint64_t checksum = 0;
    for (std::size_t index = 0; index < rinha::kDimensions; ++index) {
        const auto scaled = static_cast<std::uint64_t>((vector[index] + 2.0f) * 1'000'000.0f);
        checksum += scaled << (index % 8U);
    }
    return checksum;
}

void print_metric(
    std::string_view name,
    Nanoseconds elapsed,
    std::uint64_t checksum,
    std::size_t sample_count,
    std::size_t repeat
) {
    std::cout << name
              << "_ns_per_query=" << ns_per_query(elapsed, sample_count, repeat)
              << " checksum=" << checksum
              << '\n';
}

void print_sample_stats(const std::vector<Sample>& samples, std::size_t repeat) {
    std::size_t bytes = 0;
    std::size_t max_bytes = 0;
    for (const Sample& sample : samples) {
        bytes += sample.body.size();
        max_bytes = std::max(max_bytes, sample.body.size());
    }

    std::cout << "samples=" << samples.size()
              << " repeat=" << repeat
              << " avg_body_bytes=" << static_cast<double>(bytes) / static_cast<double>(samples.size())
              << " max_body_bytes=" << max_bytes
              << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string test_data_path = argc > 1 ? argv[1] : "test/test-data.json";
        const std::string references_path = argc > 2 ? argv[2] : "resources/references.json.gz";
        const std::size_t repeat = argc > 3 ? static_cast<std::size_t>(std::stoull(argv[3])) : 5U;
        const std::size_t limit = argc > 4 ? static_cast<std::size_t>(std::stoull(argv[4])) : 0U;

        const std::vector<Sample> samples = load_samples(test_data_path, limit);
        if (samples.empty()) {
            throw std::runtime_error("nenhum sample carregado");
        }

        rinha::ReferenceSet refs;
        std::string error;
        if (!rinha::ReferenceSet::load_gzip_json(references_path, refs, error)) {
            throw std::runtime_error(error);
        }
        const rinha::Classifier classifier(std::move(refs));

        print_sample_stats(samples, repeat);

        const auto [body_default_elapsed, body_default_checksum] = time_samples(
            samples,
            repeat,
            [](const Sample& sample) {
                std::string body;
                body.append(sample.body.data(), sample.body.size());
                return static_cast<std::uint64_t>(body.size());
            }
        );
        print_metric("body_append_default", body_default_elapsed, body_default_checksum, samples.size(), repeat);

        const auto [body_reserve_elapsed, body_reserve_checksum] = time_samples(
            samples,
            repeat,
            [](const Sample& sample) {
                std::string body;
                body.reserve(768);
                body.append(sample.body.data(), sample.body.size());
                return static_cast<std::uint64_t>(body.size());
            }
        );
        print_metric("body_append_reserve768", body_reserve_elapsed, body_reserve_checksum, samples.size(), repeat);

        const auto [dom_padded_elapsed, dom_padded_checksum] = time_samples(
            samples,
            repeat,
            [](const Sample& sample) {
                thread_local simdjson::dom::parser parser;
                simdjson::padded_string json(std::string_view(sample.body));
                simdjson::dom::element root;
                if (parser.parse(json).get(root) != simdjson::SUCCESS) {
                    throw std::runtime_error("falha no dom_padded_parse");
                }
                simdjson::dom::object object;
                if (root.get(object) != simdjson::SUCCESS) {
                    throw std::runtime_error("root não é objeto");
                }
                return static_cast<std::uint64_t>(sample.body.size());
            }
        );
        print_metric("dom_padded_parse", dom_padded_elapsed, dom_padded_checksum, samples.size(), repeat);

        const auto [dom_reserve_elapsed, dom_reserve_checksum] = time_samples(
            samples,
            repeat,
            [](const Sample& sample) {
                thread_local simdjson::dom::parser parser;
                std::string body;
                body.reserve(768);
                body.append(sample.body.data(), sample.body.size());
                simdjson::dom::element root;
                if (parser.parse(body).get(root) != simdjson::SUCCESS) {
                    throw std::runtime_error("falha no dom_reserve_parse");
                }
                simdjson::dom::object object;
                if (root.get(object) != simdjson::SUCCESS) {
                    throw std::runtime_error("root não é objeto");
                }
                return static_cast<std::uint64_t>(body.size());
            }
        );
        print_metric("dom_reserve768_parse", dom_reserve_elapsed, dom_reserve_checksum, samples.size(), repeat);

        const auto [parse_elapsed, parse_checksum_value] = time_samples(
            samples,
            repeat,
            [](const Sample& sample) {
                rinha::Payload payload;
                std::string error;
                if (!rinha::parse_payload(std::string_view(sample.body), payload, error)) {
                    throw std::runtime_error(error);
                }
                return payload_checksum(payload);
            }
        );
        print_metric("parse_payload", parse_elapsed, parse_checksum_value, samples.size(), repeat);

        const auto [vectorize_elapsed, vectorize_checksum_value] = time_samples(
            samples,
            repeat,
            [](const Sample& sample) {
                rinha::Payload payload;
                std::string error;
                if (!rinha::parse_payload(std::string_view(sample.body), payload, error)) {
                    throw std::runtime_error(error);
                }
                rinha::QueryVector vector{};
                if (!rinha::vectorize(payload, vector, error)) {
                    throw std::runtime_error(error);
                }
                return vector_checksum(vector);
            }
        );
        print_metric("parse_vectorize", vectorize_elapsed, vectorize_checksum_value, samples.size(), repeat);

        const auto [classify_elapsed, classify_checksum_value] = time_samples(
            samples,
            repeat,
            [&classifier](const Sample& sample) {
                rinha::Payload payload;
                std::string error;
                if (!rinha::parse_payload(std::string_view(sample.body), payload, error)) {
                    throw std::runtime_error(error);
                }
                rinha::Classification classification{};
                if (!classifier.classify(payload, classification, error)) {
                    throw std::runtime_error(error);
                }
                return static_cast<std::uint64_t>(classification.approved ? 1U : 0U) +
                       static_cast<std::uint64_t>(classification.fraud_score * 10.0f);
            }
        );
        print_metric("parse_classify", classify_elapsed, classify_checksum_value, samples.size(), repeat);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
