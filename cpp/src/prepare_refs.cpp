#include <filesystem>
#include <iostream>
#include <string>

#include "rinha/refs.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "uso: prepare-refs-cpp <references.json.gz> <references.bin> <labels.bin>\n";
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string references_bin_path = argv[2];
    const std::string labels_bin_path = argv[3];

    std::string error;
    rinha::ReferenceSet refs;
    if (!rinha::ReferenceSet::load_gzip_json(input_path, refs, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    if (const fs::path refs_path(references_bin_path); refs_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(refs_path.parent_path(), ec);
        if (ec) {
            std::cerr << "falha ao criar diretório de saída: " << ec.message() << '\n';
            return 1;
        }
    }

    if (const fs::path labels_path(labels_bin_path); labels_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(labels_path.parent_path(), ec);
        if (ec) {
            std::cerr << "falha ao criar diretório de saída: " << ec.message() << '\n';
            return 1;
        }
    }

    if (!refs.write_binary(references_bin_path, labels_bin_path, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    return 0;
}
