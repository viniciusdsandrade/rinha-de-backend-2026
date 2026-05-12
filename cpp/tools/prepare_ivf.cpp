#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "rinha/ivf.hpp"

namespace {

std::uint32_t parse_u32(char** argv, int index, std::uint32_t fallback) {
    if (argv[index] == nullptr) {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::stoul(argv[index]));
}

std::size_t parse_size(char** argv, int index, std::size_t fallback) {
    if (argv[index] == nullptr) {
        return fallback;
    }
    return static_cast<std::size_t>(std::stoull(argv[index]));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 7) {
        std::cerr << "uso: prepare-ivf-cpp <references.json.gz> <index.bin> "
                  << "[clusters=256] [train_sample=65536] [iters=6] [max_refs=0]\n";
        return 1;
    }

    rinha::IvfBuildOptions options{};
    if (argc > 3) {
        options.clusters = parse_u32(argv, 3, options.clusters);
    }
    if (argc > 4) {
        options.train_sample = parse_u32(argv, 4, options.train_sample);
    }
    if (argc > 5) {
        options.iterations = parse_u32(argv, 5, options.iterations);
    }
    if (argc > 6) {
        options.max_references = parse_size(argv, 6, options.max_references);
    }

    std::string error;
    rinha::IvfIndex index;
    if (!rinha::IvfIndex::build_from_gzip_json(argv[1], options, index, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    if (!index.write_binary(argv[2], error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "ivf_index=" << argv[2]
              << " refs=" << index.len()
              << " padded=" << index.padded_len()
              << " clusters=" << index.clusters()
              << " memory_mb=" << static_cast<double>(index.memory_bytes()) / (1024.0 * 1024.0)
              << '\n';
    return 0;
}
